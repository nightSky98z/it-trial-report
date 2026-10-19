#include "ecs/city_monster_factory.h"
#include "ecs/map_components.h"
#include "ecs/player_ship_components.h"
#include "generated/game_generated.h"
#include "utils/log.h"

namespace city_monster_factory {

/**
 * @brief キャッシュ済みのスポーン地点からシティモンスターのエンティティを作る
 *
 * @return 新しいエンティティ。
 * @retval entt::null 設定またはエリア境界が見つからない。
 */
entt::entity spawnAtPoint(entt::registry &world,
                          NetworkEntityIdAllocator &alloc,
                          const cache::TemplateCache &tc,
                          uint64_t spawnPointId,
                          uint32_t unitId,
                          uint32_t campId,
                          uint32_t mapId) {
  auto spIt = tc.spawnPoints.find(spawnPointId);
  if (spIt == tc.spawnPoints.end()) {
    zone_log::error(
        "city_monster_factory missing spawn_point_id=%llu "
        "(map=%u unit=%u)",
        static_cast<unsigned long long>(spawnPointId), mapId, unitId);
    return entt::null;
  }
  if (spIt->second.mapId != mapId) {
    zone_log::error(
        "city_monster_factory spawn_point map mismatch "
        "(spawn_point=%llu spawn_map=%u request_map=%u unit=%u)",
        static_cast<unsigned long long>(spawnPointId),
        spIt->second.mapId, mapId, unitId);
    return entt::null;
  }
  auto uIt = tc.units.find(unitId);
  if (uIt == tc.units.end()) {
    zone_log::error(
        "city_monster_factory missing unit_template id=%u "
        "(spawn_point=%llu map=%u)",
        unitId,
        static_cast<unsigned long long>(spawnPointId), mapId);
    return entt::null;
  }

  const float   spawnX = spIt->second.x;
  const float   spawnY = spIt->second.y;
  const uint32_t areaId = spIt->second.areaId;
  const auto   &tpl    = uIt->second;

    // エリアの矩形境界を引く（TemplateCache::loadAll で設定が揃っていてスポーン地点が範囲内であることは確認済み）。
    // ここでも念のため確認し、見つからなければ生成を拒否する。境界のないモンスターが範囲外へ出るのを防ぐ。
  auto bIt = tc.areaBounds.find(cache::areaBoundsKey(mapId, static_cast<uint8_t>(areaId)));
  if (bIt == tc.areaBounds.end()) {
    zone_log::error(
        "city_monster_factory missing area_bounds "
        "(map=%u area=%u) for spawn_point=%llu unit=%u",
        mapId, areaId,
        static_cast<unsigned long long>(spawnPointId), unitId);
    return entt::null;
  }
  const auto &areaRect = bIt->second;

  auto e = world.create();
  world.emplace<NetworkId>(e, alloc.alloc());
    // モンスターには Owner コンポーネントを付けない（Owner はプレイヤーの精霊の所有者追跡専用）
  world.emplace<UnitBase>(e, UnitBase{
      .unitId = tpl.unitId, .campId = campId,
      .unitKind = static_cast<uint8_t>(Game::UnitKind_Monster),
      .x = spawnX, .y = spawnY,
      .curHp = tpl.baseHp, .maxHp = tpl.baseHp,
  });
  world.emplace<Velocity>(e);

    // メインシティ PvE：スポーン地点が属するエリアのモンスター索引に登録する。
    // player_ship_ai のターゲット取得と、city_monster_combat_system の同エリア内ヘイト選択で使う。
  world.emplace<CAreaMember>(e, CAreaMember{areaId});

    // エリアの矩形境界は範囲外へ出たときの保険にすぎない。徘徊・追跡は CAreaMember.areaId と nav のエリアで制約する。
  world.emplace<CAreaBounds>(e, CAreaBounds{
      .minX = areaRect.minX, .maxX = areaRect.maxX,
      .minY = areaRect.minY, .maxY = areaRect.maxY,
  });

    // スポーン元への逆参照：死亡時に monster_lifecycle_system が読み取り、リスポーン枠の作成に使う。
  world.emplace<CSpawnOrigin>(e, CSpawnOrigin{
      .spawnPointId = spawnPointId,
      .unitId       = unitId,
      .campId       = campId,
  });

    // ── AI / 戦闘コンポーネント ──
  world.emplace<UnitFaction>(e, UnitFaction{
      .factionMask = kFactionEnemy,
      .enemyMask   = kEnemyEnemyMask,
  });
    // 索敵半径：city_monster_combat_system は、この値でモンスターがどこまでの精霊を見つけられるかを決める。
    //
    // この制限がないと、精霊がエリアに足を踏み入れた瞬間にエリア内の全モンスターから同時に狙われ、
    // 到着と同時に袋叩きにされる。参考値：攻撃射程は約 50〜75、エリアのアンカー間隔は 448。
    //
    // TODO: unit_templates に detect_radius 列を追加したら、テンプレートの設定値を使うように変える。
    //       そうすれば企画がモンスターごとに警戒範囲を変えられ、設定のホットリロードにも乗る。
  constexpr float kMonsterDetectRadius = 300.f;
  world.emplace<UnitSensor>(e, UnitSensor{
      .radius = kMonsterDetectRadius,
      .faction_mask = kFactionPlayer,
  });
  world.emplace<UnitAggro>(e);
  world.emplace<UnitAIState>(e);
  world.emplace<UnitCombat>(e, UnitCombat{
      .max_hp       = static_cast<int>(tpl.baseHp),
      .cur_hp       = static_cast<int>(tpl.baseHp),
      .attack_power = static_cast<int>(tpl.baseAtk),
      .move_speed   = tpl.baseSpeed,
      .attack_range = tpl.attackRange * 50.f,  // attack_range をワールド座標系にスケーリング
  });
  world.emplace<UnitTarget>(e);
  world.emplace<UnitActionIntent>(e);
  world.emplace<UnitActionState>(e);
  return e;
}

} // namespace city_monster_factory
