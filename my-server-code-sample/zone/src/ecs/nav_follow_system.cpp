#include "ecs/nav_follow_system.h"

#include "ecs/ai_common.h"
#include "ecs/map_components.h"
#include "ecs/nav_components.h"
#include "generated/game_generated.h"

namespace game::nav_follow_system {

namespace {

/**
 * @brief 移動速度を 0 にし、必要なら Move 状態を抜ける
 *
 * @note
 * - 到着したとき、経路が無効になったとき、移動できないときに呼ぶ。
 */
inline void clearMoveVelocity(UnitBase &base, Velocity &vel) {
  vel.vx = 0.f;
  vel.vy = 0.f;
  if (base.actionState == static_cast<uint8_t>(Game::ActionState_Move)) {
    base.actionState = static_cast<uint8_t>(Game::ActionState_Idle);
  }
}

} // namespace

/**
 * @brief 現在の経路を進め、速度を設定する
 *
 * @note
 * - 経路の終端に着いたら nav.arrived=true にして速度をクリアする。
 */
void update(entt::registry &world, const NavGridStatic &grid) {
  auto view = world.view<CNavAgent, UnitBase, UnitCombat, Velocity>();
  for (auto e : view) {
    auto &nav = view.get<CNavAgent>(e);
    auto &base = view.get<UnitBase>(e);
    auto &combat = view.get<UnitCombat>(e);
    auto &vel = view.get<Velocity>(e);

    auto *pathStore = navTryPathStore(world);
    if (!nav.active || nav.arrived || !pathStore ||
        !navPathRangeValid(*pathStore, nav) ||
        nav.nextIndex >= nav.pathCount) {
      if (navHasPath(nav) && (!pathStore || !navPathRangeValid(*pathStore, nav))) {
        navDiscardInvalidPath(nav);
        nav.pathDirty = true;
        nav.arrived = false;
      }
      clearMoveVelocity(base, vel);
      continue;
    }

    if (combat.move_speed <= 0.f) {
      clearMoveVelocity(base, vel);
      continue;
    }

    while (nav.nextIndex < nav.pathCount) {
      const auto *cell = navTryPathCell(world, nav, nav.nextIndex);
      if (!cell) {
        navDiscardInvalidPath(nav);
        nav.pathDirty = true;
        nav.arrived = false;
        clearMoveVelocity(base, vel);
        break;
      }
      const Position target = navCellCenterToWorld(grid, *cell);
      float dx = target.x - base.x;
      float dy = target.y - base.y;
      if (ai_common::lengthSquared(dx, dy) > 16.f) {
        ai_common::normalizeOrZero(dx, dy);
        vel.vx = dx * combat.move_speed;
        vel.vy = dy * combat.move_speed;
        base.facing = ai_common::facingFromVelocity(vel.vx, vel.vy, base.facing);
        base.actionState = static_cast<uint8_t>(Game::ActionState_Move);
        break;
      }

      ++nav.nextIndex;
      if (nav.nextIndex >= nav.pathCount) {
        nav.arrived = true;
        clearMoveVelocity(base, vel);
        break;
      }
    }
  }
}

} // namespace game::nav_follow_system
