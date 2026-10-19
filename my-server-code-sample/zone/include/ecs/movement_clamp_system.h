#pragma once
#include "ecs/map_components.h"
#include <entt/entt.hpp>

namespace movement_clamp_system {

/**
 * @brief エリア境界を持つエンティティを、矩形の内側へ強制的に押し戻す
 *
 * @param world マップの ECS world。UnitBase の座標と Velocity を更新する。
 *
 * @note
 * - velocity_system::update の後に呼ぶこと（この時点で速度による位置更新は済んでいる）。
 * - 現在対象になるのは CAreaBounds を持つシティモンスターだけで、プレイヤー船には付与されていない。
 * - 押し戻した軸は速度も 0 にする。そうしないと次フレームも境界に向かって進み続けてしまう。
 */
inline void update(entt::registry &world) {
  auto view = world.view<UnitBase, Velocity, CAreaBounds>();
  for (auto e : view) {
    auto &base = view.get<UnitBase>(e);
    auto &vel  = view.get<Velocity>(e);
    const auto &r = view.get<CAreaBounds>(e);

    if (base.x < r.minX)      { base.x = r.minX; vel.vx = 0.f; }
    else if (base.x > r.maxX) { base.x = r.maxX; vel.vx = 0.f; }

    if (base.y < r.minY)      { base.y = r.minY; vel.vy = 0.f; }
    else if (base.y > r.maxY) { base.y = r.maxY; vel.vy = 0.f; }
  }
}

} // namespace movement_clamp_system
