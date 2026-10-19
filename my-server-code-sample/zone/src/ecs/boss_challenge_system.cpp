#include "ecs/boss_challenge_system.h"
#include "ecs/city_monster_factory.h"
#include "ecs/components.h"
#include "generated/game_generated.h"
#include "utils/log.h"
#include <algorithm>

namespace boss_challenge_system {
namespace {

/// ボスの陣営（unit_templates.camp_id）。募集可能・モンスターの陣営と区別する。
constexpr uint32_t kBossCampId = 2;

/**
 * @brief ボスの出現地点を検索する
 *
 * @param tc テンプレートキャッシュ。
 * @param mapId 対象のマップ。
 * @param bossUnitId ボスのテンプレート ID。
 * @param[out] outAreaId 見つかった出現地点が属するエリア。
 *
 * @return area_spawn_points.id。
 * @retval 0 このマップにそのボスの出現地点が設定されていない。
 *
 * @note
 * - 位置は monster_spawns_point の camp_id=2 の行から取る（ボス 1 体につき 1 行）。
 *   企画の設定で決まるもので、サーバー側で推測することはもうしない。
 */
uint64_t bossSpawnPointOf(const cache::TemplateCache &tc, uint32_t mapId,
                          uint32_t bossUnitId, uint8_t &outAreaId) {
  const auto it =
      tc.bossSpawnPoints.find(cache::bossSpawnKey(mapId, bossUnitId));
  if (it == tc.bossSpawnPoints.end()) return 0;

  const auto spIt = tc.spawnPoints.find(it->second);
  if (spIt == tc.spawnPoints.end()) return 0;

  outAreaId = spIt->second.areaId;
  return it->second;
}

} // namespace

/**
 * @brief ボス挑戦を開始する
 *
 * @note
 * - 検証の順序：テンプレートが存在する → ボスである → 生存中のボスがいない → 出現地点がある。
 *   すべて通過してからフィールドを片付けるので、失敗時にマップの状態は変わらない。
 */
ChallengeError startChallenge(MapInstance &inst, const cache::TemplateCache &tc,
                              uint32_t bossUnitId,
                              std::vector<uint32_t> &outRemovedNetworkIds,
                              uint32_t &outBossAreaId) {
  const auto tplIt = tc.units.find(bossUnitId);
  if (tplIt == tc.units.end()) return ChallengeError::UnknownBoss;
  if (tplIt->second.campId != kBossCampId) return ChallengeError::NotABoss;

    // 生存中のボスがいれば拒否する。同じマップにボスが複数重ならないようにするため。
  {
    auto bossView = inst.world.view<CBossTag>();
    if (bossView.begin() != bossView.end()) return ChallengeError::BossAlive;
  }

  uint8_t bossArea = 0;
  const uint64_t spawnPointId =
      bossSpawnPointOf(tc, inst.mapId, bossUnitId, bossArea);
  if (spawnPointId == 0) return ChallengeError::NoSpawnPoint;

    // ── フィールドの片付け：通常モンスターをすべて破棄し、戦闘後に固定設定どおり再配置する ──
  {
    std::vector<entt::entity> monsters;
    auto view = inst.world.view<NetworkId, UnitBase>();
    for (auto e : view) {
      const auto &base = view.get<UnitBase>(e);
      if (base.unitKind != static_cast<uint8_t>(Game::UnitKind_Monster)) {
        continue;
      }
      monsters.push_back(e);
      outRemovedNetworkIds.push_back(view.get<NetworkId>(e).value);
    }
    for (const auto e : monsters) inst.world.destroy(e);
  }

  const entt::entity bossEntity = city_monster_factory::spawnAtPoint(
      inst.world, inst.networkEntityIdAlloc, tc, spawnPointId, bossUnitId,
      kBossCampId, inst.mapId);
  if (bossEntity == entt::null) {
        // 片付けは済んだのにボスが出現しなかった：通常モンスターをすぐ元に戻し、空のマップが残らないようにする。
    respawnNormalMonsters(inst, tc);
    return ChallengeError::NoSpawnPoint;
  }
  inst.world.emplace<CBossTag>(bossEntity,
                               CBossTag{bossUnitId, bossArea});

    // ボスは通常モンスターの生成処理を流用しているため、CSpawnOrigin が付いている。
    // このコンポーネントは monster_lifecycle_system が「固定スポーンのモンスター」と判定する根拠なので、
    // 残しておくとボスが倒れた後に自動でリスポーンしてしまう。ボスはプレイヤーが挑戦したときだけ現れ、
    // 固定スポーンではないため、ここで明示的に取り除く。
  inst.world.remove<CSpawnOrigin>(bossEntity);

  outBossAreaId = bossArea;
  zone_log::info("boss challenge started map=%u boss=%u area=%u",
                 inst.mapId, bossUnitId, static_cast<uint32_t>(bossArea));
  return ChallengeError::None;
}

/**
 * @brief 倒されたエンティティのボステンプレート ID を取得する
 */
uint32_t bossUnitIdOf(const entt::registry &world, entt::entity victim) {
  if (victim == entt::null || !world.valid(victim)) return 0;
  const auto *tag = world.try_get<CBossTag>(victim);
  return tag ? tag->bossUnitId : 0;
}

/**
 * @brief ボスがいるエリアを取得する
 */
uint32_t bossAreaIdOf(const entt::registry &world, entt::entity victim) {
  if (victim == entt::null || !world.valid(victim)) return 0;
  const auto *tag = world.try_get<CBossTag>(victim);
  return tag ? tag->areaId : 0;
}

/**
 * @brief 通常モンスターを再配置する
 *
 * @note
 * - マップ初期化時と同じ固定スポーン設定をそのまま使う。実行時のスナップショットを別に持たないことで、
 *   2 か所の状態が食い違うのを防ぐ。
 */
void respawnNormalMonsters(MapInstance &inst, const cache::TemplateCache &tc) {
  const auto it = tc.fixedSpawns.find(inst.mapId);
  if (it == tc.fixedSpawns.end()) return;
  for (const auto &fs : it->second) {
    city_monster_factory::spawnAtPoint(inst.world, inst.networkEntityIdAlloc,
                                       tc, fs.spawnPointId, fs.unitId,
                                       fs.campId, inst.mapId);
  }
}

} // namespace boss_challenge_system
