#include "ecs/area_registry_system.h"

#include <limits>
#include "generated/game_generated.h"

namespace area_registry_system {

/**
 * @brief エリア別モンスター索引を ctx に作成する
 *
 * @note
 * - 既に存在する場合はそのまま使い、二重に emplace しない。
 */
void initialize(entt::registry &world) {
  if (!world.ctx().contains<AreaMonsterRegistry>()) {
    world.ctx().emplace<AreaMonsterRegistry>();
  }
}

/**
 * @brief エリア別モンスター索引を毎フレーム作り直す
 *
 * @note
 * - Dead 以外のモンスターを集める。攻撃対象を最終的に選ぶ際に、Alive かどうかをもう一度確認する。
 */
void update(entt::registry &world) {
  auto *reg = world.ctx().find<AreaMonsterRegistry>();
  if (!reg) return;

  for (auto &vec : reg->byArea) vec.clear();

  auto view = world.view<CAreaMember, UnitBase>();
  for (auto e : view) {
    const auto &base = view.get<UnitBase>(e);
    if (base.unitKind != static_cast<uint8_t>(Game::UnitKind_Monster)) continue;
    if (base.lifeStatus == static_cast<uint8_t>(Game::LifeStatus_Dead)) continue;

    const auto areaId = view.get<CAreaMember>(e).areaId;
    if (areaId < AreaMonsterRegistry::kAreaCount) {
      reg->byArea[areaId].push_back(e);
    }
  }
}

/**
 * @brief エリア内で指定座標に最も近い Alive のモンスターを返す
 *
 * @retval entt::null 索引がない、areaId が無効、または生存しているモンスターがいない。
 */
entt::entity pickNearestAliveMonster(entt::registry &world, uint32_t areaId,
                                     float fromX, float fromY) {
  auto *reg = world.ctx().find<AreaMonsterRegistry>();
  if (!reg) return entt::null;
  if (areaId >= AreaMonsterRegistry::kAreaCount) return entt::null;

  entt::entity best = entt::null;
  float bestD2 = std::numeric_limits<float>::max();

  for (auto e : reg->byArea[areaId]) {
    if (!world.valid(e)) continue;
    if (!world.all_of<UnitBase>(e)) continue;
    const auto &base = world.get<UnitBase>(e);
    if (base.lifeStatus != static_cast<uint8_t>(Game::LifeStatus_Alive)) {
      continue;
    }

    const float dx = base.x - fromX;
    const float dy = base.y - fromY;
    const float d2 = dx * dx + dy * dy;
    if (d2 < bestD2) {
      bestD2 = d2;
      best = e;
    }
  }
  return best;
}

} // namespace area_registry_system
