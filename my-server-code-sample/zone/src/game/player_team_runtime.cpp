#include "game/player_team_runtime.h"

#include "ecs/city_combat_helpers.h"
#include "ecs/nav_components.h"
#include "ecs/map_components.h"
#include "game/player_ship_constants.h"
#include "game/city_nav_grid.h"
#include "game/nav_grid.h"
#include "game/template_cache.h"
#include "protocol/codec.h"
#include "utils/log.h"

#include <algorithm>

namespace game {

namespace {

/**
 * @brief プレイヤー船のエリア ID を正規化する
 *
 * @retval 1..4 元の値が有効なエリア。
 * @retval kDefaultAreaId 入力が範囲外、または 0。
 */
uint32_t sanitizePlayerShipAreaId(uint32_t areaId) {
  if (areaId < 1 || areaId > 4) return player_ship_constants::kDefaultAreaId;
  return areaId;
}

/**
 * @brief 永続化されたレベル、なければデフォルトのレベルを返す
 *
 * @return 1 以上の実行時レベル。
 */
uint32_t runtimeLevelOrDefault(const codec::TeamUnitInfoDto &unit,
                              uint32_t maxLevel) {
  if (!unit.hasRuntimePersistence) return 1;
    // 上限を下げた場合、保存データの超過分のレベルは実行時に丸めるだけで、DB には書き戻さない。
    // 上限を元に戻せば、プレイヤーのレベルもそのまま戻る。
  const uint32_t cap =
      maxLevel > 0 ? maxLevel : player_ship_constants::kDefaultMaxLevel;
  return std::clamp<uint32_t>(unit.runtimeLevel, 1, cap);
}

/**
 * @brief 永続化された経験値、なければデフォルトの経験値を返す
 *
 * @return 永続化されたレコードがあれば runtimeExp、なければ 0。
 */
uint64_t runtimeExpOrDefault(const codec::TeamUnitInfoDto &unit) {
  if (!unit.hasRuntimePersistence) return 0;
  return unit.runtimeExp;
}

/**
 * @brief 永続化されたエリア、なければデフォルトのエリアを返す
 *
 * @return 正規化した業務上のエリア ID。
 */
uint32_t runtimeAreaOrDefault(const codec::TeamUnitInfoDto &unit) {
  if (!unit.hasRuntimePersistence) return player_ship_constants::kDefaultAreaId;
  return sanitizePlayerShipAreaId(unit.runtimeAreaId);
}

} // namespace

/**
 * @brief キャラクターの現在のエンティティのプロトコル ID を集める
 *
 * @return 所有権を持つ vector。空ならそのキャラクターのエンティティはない。
 */
std::vector<uint32_t>
collectNetworkEntityIdsForRole(entt::registry &world, const std::string &roleId) {
  std::vector<uint32_t> result;
  auto view = world.view<NetworkId, Owner>();
  for (auto e : view) {
    if (view.get<Owner>(e).roleId == roleId) {
      result.push_back(view.get<NetworkId>(e).value);
    }
  }
  return result;
}

/**
 * @brief キャラクターが現在所有しているすべてのエンティティを破棄する
 *
 * @note
 * - 先に nav の経路を返却してから、エンティティを破棄する。
 */
void destroyEntitiesForRole(entt::registry &world, const std::string &roleId) {
  auto view = world.view<Owner>();
  std::vector<entt::entity> toDestroy;
  for (auto e : view) {
    if (view.get<Owner>(e).roleId == roleId) {
      toDestroy.push_back(e);
    }
  }
  for (auto e : toDestroy) {
    game::navClearEntityPath(world, e);
    world.destroy(e);
  }
}

/**
 * @brief プレイヤー船の基礎ステータスを計算する
 *
 * @note
 * - エンティティの生成時と装備変更時の再計算はこの関数を共用する。成長式はここにしかない。
 */
PlayerShipBaseStats baseStatsForUnit(const cache::TemplateCache &tc,
                                     uint32_t unitId, uint32_t level) {
  PlayerShipBaseStats out;
  const auto it = tc.units.find(unitId);
  if (it != tc.units.end()) {
    const auto &tpl = it->second;
    out.hp = tpl.baseHp;
    out.atk = tpl.baseAtk;
    out.speed = tpl.baseSpeed;
    out.attackRange = tpl.attackRange;
    out.campId = tpl.campId;
  }

  const uint32_t levelDelta = level > 1 ? level - 1 : 0;
  out.hp += levelDelta * player_ship_constants::kHpPerLevel;
  out.atk += levelDelta * player_ship_constants::kAtkPerLevel;
  return out;
}

/**
 * @brief 現在の装備に基づいてプレイヤー船の戦闘ステータスを再計算する
 *
 * @note
 * - 「差分で増減する」のではなく「絶対値を計算し直す」方式をとる。何度装備を付け替えてもステータスがずれない。
 */
bool refreshUnitCombatStats(entt::registry &world, const std::string &roleId,
                            uint32_t unitId, const cache::TemplateCache &tc,
                            const cache::ItemCache &items,
                            const equipment::RoleEquipment &equipment) {
  bool found = false;
  auto view = world.view<Owner, UnitBase, UnitCombat, CExperience>();
  for (auto e : view) {
    if (view.get<Owner>(e).roleId != roleId) continue;
    auto &base = view.get<UnitBase>(e);
    if (base.unitId != unitId) continue;

    const uint32_t level = view.get<CExperience>(e).level;
    const PlayerShipBaseStats st = baseStatsForUnit(tc, unitId, level);
    const equip::StatBonus bonus =
        equip::bonusForUnit(items, equipment, unitId);

    const uint32_t newMaxHp = st.hp + bonus.hp;
    const uint32_t newAtk = st.atk + bonus.atk;

        // 最大 HP が変わったら現在 HP も同じだけ増減させ、「装備の付け替えで回復も減少もしない」状態を保つ。
        // 下限は 1 に丸める。装備を外しただけでユニットが倒れてはならない。
    const uint32_t oldMaxHp = base.maxHp;
    uint32_t curHp = base.curHp;
    if (newMaxHp > oldMaxHp) {
      curHp += newMaxHp - oldMaxHp;
    } else if (oldMaxHp > newMaxHp) {
      const uint32_t drop = oldMaxHp - newMaxHp;
      curHp = curHp > drop ? curHp - drop : 1;
    }
    if (curHp > newMaxHp) curHp = newMaxHp;
    if (curHp == 0) curHp = 1;

    base.maxHp = newMaxHp;
    base.curHp = curHp;

    auto &combat = view.get<UnitCombat>(e);
    combat.max_hp = static_cast<int>(newMaxHp);
    combat.cur_hp = static_cast<int>(curHp);
    combat.attack_power = static_cast<int>(newAtk);

        // UnitCombat を変更したら、クライアントにキャッシュの更新を必ず通知する。そうしないと編成パネルや頭上のラベルに
        // 付け替え前の数値が表示され続ける。呼び出し側の送り忘れを防ぐため、この関数の中で送信する。
    if (auto *hub = world.ctx().find<MonsterAIEventHub>()) {
      StatsChangedEvent sc;
      sc.unit = e;
      sc.map_id = 0;  // パック時に閲覧者のいるマップで絞り込むので、ここで正確な mapId は不要
      hub->stats_changed.curr.push_back(sc);
    }

    found = true;
  }
  return found;
}

/**
 * @brief プレイヤーの編成のユニットエンティティを生成する
 *
 * @note
 * - roleId をコピーし、Owner/UnitBase/Nav/Experience などの実行時コンポーネントを作成する。
 */
/**
 * @brief プレイヤーの精霊エンティティを 1 体生成する
 *
 * @note
 * - spawnPlayerTeamUnits と同じコンポーネントの組み立てを使う。募集時は新しく加わった 1 体だけを生成する。
 */
void spawnSinglePlayerUnit(entt::registry &world,
                           NetworkEntityIdAllocator &alloc,
                           const std::string &roleId,
                           const codec::TeamUnitInfoDto &unit,
                           const cache::TemplateCache &tc,
                           const cache::ItemCache &items,
                           const equipment::RoleEquipment &equipment,
                           float spawnX, float spawnY, bool isDeployed,
                           uint32_t maxLevel) {
  codec::RoleInfoDto solo;
  solo.roleId = roleId;
    // 出撃状態は呼び出し側が実際の編成に基づいて渡すもので、ここで作る一時的な編成から推測してはならない。
    // selectedTeamId をこの一時的な編成に向けてしまうと、どんな新しい精霊も出撃中とみなされ、
    // 募集したばかりで編成に入っていない精霊まで撃破経験値を受け取り始めてしまう。
  solo.selectedTeamId = isDeployed ? 1 : 0;
  codec::TeamInfoDto team;
  team.teamId = 1;
  team.units.push_back(unit);
  solo.teams.push_back(team);

    // 全体を生成する処理を流用し、この 1 体だけを渡す。
    // これにより、コンポーネントの組み立て・レベル成長・装備ボーナスの実装が 1 か所にまとまる。
  spawnPlayerTeamUnits(world, alloc, roleId, solo, tc, items, equipment,
                       {unit}, spawnX, spawnY, maxLevel);
}

void spawnPlayerTeamUnits(entt::registry &world,
                          NetworkEntityIdAllocator &alloc,
                          const std::string &roleId,
                          const codec::RoleInfoDto &roleDto,
                          const cache::TemplateCache &tc,
                          const cache::ItemCache &items,
                          const equipment::RoleEquipment &equipment,
                          const std::vector<codec::TeamUnitInfoDto> &ownedUnits,
                          float spawnX,
                          float spawnY,
                          uint32_t maxLevel) {
  const codec::TeamInfoDto *selectedTeam = nullptr;
  for (const auto &t : roleDto.teams) {
    if (t.teamId == roleDto.selectedTeamId) {
      selectedTeam = &t;
      break;
    }
  }

  auto spawnUnit = [&](const codec::TeamUnitInfoDto &unit, float x, float y,
                       bool isDeployed) {
    const uint32_t unitId = unit.unitId;
    const uint32_t initLevel = runtimeLevelOrDefault(unit, maxLevel);
    const uint32_t targetAreaId = runtimeAreaOrDefault(unit);
    auto e = world.create();
    world.emplace<NetworkId>(e, alloc.alloc());
    world.emplace<Owner>(e, roleId);

    auto it = tc.units.find(unitId);
    if (it == tc.units.end()) {
      zone_log::warning("game unit_template not found: %u, using defaults",
                        unitId);
    }

    const PlayerShipBaseStats base = baseStatsForUnit(tc, unitId, initLevel);
    const equip::StatBonus bonus =
        equip::bonusForUnit(items, equipment, unitId);
    const float baseSpeed = base.speed;
    const float attackRange = base.attackRange;
    const uint32_t runtimeHp = base.hp + bonus.hp;
    const uint32_t runtimeAtk = base.atk + bonus.atk;

    world.emplace<UnitBase>(e, UnitBase{
        .unitId = unitId,
        .campId = it != tc.units.end() ? it->second.campId : 1,
        .unitKind = static_cast<uint8_t>(Game::UnitKind_PlayerUnit),
        .x = x,
        .y = y,
        .curHp = runtimeHp,
        .maxHp = runtimeHp,
        .lifeStatus = 0,
        .actionState = 0,
        .level = initLevel,
    });

    world.emplace<UnitFaction>(e, UnitFaction{
        .factionMask = kFactionPlayer,
        .enemyMask = kPlayerEnemyMask,
    });
    world.emplace<UnitSensor>(e, UnitSensor{
        .radius = 300.f,
        .faction_mask = kFactionEnemy,
    });
    world.emplace<UnitAggro>(e);
    world.emplace<Velocity>(e);
    world.emplace<UnitAIState>(e);
    world.emplace<UnitCombat>(e, UnitCombat{
        .max_hp = static_cast<int>(runtimeHp),
        .cur_hp = static_cast<int>(runtimeHp),
        .attack_power = static_cast<int>(runtimeAtk),
        .move_speed = baseSpeed,
        .attack_range = attackRange * 50.f,
    });
    world.emplace<UnitTarget>(e);
    world.emplace<UnitActionIntent>(e);
    world.emplace<UnitActionState>(e);
    world.emplace<CPlayerShipAI>(e, CPlayerShipAI{});
    const auto &grid = navGridOf(world);
    const Position anchor =
        navCellCenterToWorld(grid, grid.areaAnchorCells[targetAreaId]);
    world.emplace<CAreaMember>(e,
                               CAreaMember{player_ship_constants::kDefaultAreaId});
    world.emplace<CPlayerShipAssignment>(
        e,
        CPlayerShipAssignment{targetAreaId, anchor.x, anchor.y, 0, isDeployed});
    world.emplace<game::CNavAgent>(e, game::CNavAgent{
        .active = true,
        .pathDirty = true,
        .arrived = false,
        .goalCell = grid.areaAnchorCells[targetAreaId],
    });
    const uint64_t initExp = runtimeExpOrDefault(unit);
    CExperience expComp{};
    expComp.level = initLevel;
    expComp.currentXp = initExp;
    expComp.nextLevelXp = city_combat::xpRequiredForLevel(initLevel + 1);
    world.emplace<CExperience>(e, expComp);
  };

    // マップ上には出撃編成だけでなく「キャラクターが所持する全精霊」を出現させる。
    // クライアントの「マイ精霊」一覧はマップ上の PlayerUnit エンティティそのものなので、
    // 待機中の精霊もその場にいないと、プレイヤーが選んで出撃させることができない。
    //
    // 出撃編成の精霊は永続化されたエリアをそのまま使い、待機中の精霊は一律メインシティにとどまる。
    // メインシティは安全地帯でモンスターがいないため、「待機に戻したらシティへ帰って戦わない」のと同じ意味になる。
  std::size_t spawnIndex = 0;
  for (const auto &owned : ownedUnits) {
    const codec::TeamUnitInfoDto *deployed = nullptr;
    if (selectedTeam) {
      for (const auto &u : selectedTeam->units) {
        if (u.unitId != owned.unitId) continue;
        deployed = &u;
        break;
      }
    }

        // 出撃中なら編成側のデータ（スロットとエリアを含む）を使い、待機中ならメインシティへ強制的に戻す。
    codec::TeamUnitInfoDto entry = deployed ? *deployed : owned;
    if (!deployed) {
      entry.runtimeAreaId = player_ship_constants::kDefaultAreaId;
    }

    const float offsetX = static_cast<float>(spawnIndex) * 32.f;
    spawnUnit(entry, spawnX + offsetX, spawnY, deployed != nullptr);
    ++spawnIndex;
  }
}

/**
 * @brief キャラクターがマップに入るための、プレイヤー編成のエンティティを準備する
 *
 * @return 古いエンティティの削除リストと、既存のエンティティを再利用したかどうか。
 */
PlayerTeamEnterMapResult preparePlayerTeamUnitsForEnterMap(
    entt::registry &world,
    NetworkEntityIdAllocator &alloc,
    const std::string &roleId,
    const codec::RoleInfoDto &roleDto,
    const cache::TemplateCache &tc,
    const cache::ItemCache &items,
    const equipment::RoleEquipment &equipment,
    const std::vector<codec::TeamUnitInfoDto> &ownedUnits,
    float spawnX,
    float spawnY,
    bool reuseExistingUnits,
    uint32_t maxLevel) {
  PlayerTeamEnterMapResult result;
  if (reuseExistingUnits) {
    result.reusedExistingUnits =
        !collectNetworkEntityIdsForRole(world, roleId).empty();
    if (result.reusedExistingUnits) {
      return result;
    }
  }

  result.removedNetworkEntityIds = collectNetworkEntityIdsForRole(world, roleId);
  if (!result.removedNetworkEntityIds.empty()) {
    destroyEntitiesForRole(world, roleId);
  }

  spawnPlayerTeamUnits(world, alloc, roleId, roleDto, tc, items, equipment,
                       ownedUnits, spawnX, spawnY, maxLevel);
  return result;
}

} // namespace game
