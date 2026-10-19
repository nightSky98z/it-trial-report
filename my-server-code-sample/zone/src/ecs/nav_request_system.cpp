#include "ecs/nav_request_system.h"

#include "ecs/map_components.h"
#include "ecs/nav_components.h"
#include "game/nav_astar.h"

namespace game::nav_request_system {

namespace {

/**
 * @brief 現在の world が持つ A* スクラッチ領域を返す
 *
 * @param world マップの ECS world。スクラッチ領域の寿命は ctx() が管理する。
 *
 * @return 使い回せる A* 用の一時メモリプールへの参照
 *
 * @note
 * - そのフレームで実際に経路探索が必要になったときだけ呼ぶ。
 *   経路要求のない world に、不要な ctx 状態を作らないため。
 * - nav_request_system は world ごとに単一スレッドで動く。返した参照をスレッド間で共有しないこと。
 */
NavAStarScratch &navAStarScratch(entt::registry &world) {
  if (!world.ctx().contains<NavAStarScratch>()) {
    world.ctx().emplace<NavAStarScratch>();
  }
  return world.ctx().get<NavAStarScratch>();
}

} // namespace

void update(entt::registry &world, const NavGridStatic &grid, uint64_t nowMs) {
  auto view = world.view<CNavAgent, UnitBase>();
  for (auto e : view) {
    auto &nav = view.get<CNavAgent>(e);
    if (!nav.active) {
      continue;
    }

    bool forceRepath = nav.pathDirty;
    if (!nav.pathDirty) {
      if (navHasPath(nav)) {
        auto *pathStore = navTryPathStore(world);
        if (pathStore && navPathRangeValid(*pathStore, nav)) {
          continue;
        }
        navDiscardInvalidPath(nav);
        nav.pathDirty = true;
        nav.arrived = false;
        forceRepath = true;
      }
      if (!forceRepath && nowMs < nav.lastRepathAtMs + nav.repathCooldownMs) {
        continue;
      }
    }

    const auto &base = view.get<UnitBase>(e);
    const NavCell start = navWorldToCell(grid, base.x, base.y);
    auto &scratch = navAStarScratch(world);
    const NavPathResult path = findPathAStar(grid, start, nav.goalCell, scratch);

    if (!path.found) {
      navClearPath(world, nav);
      nav.arrived = false;
      nav.pathDirty = true;
      nav.lastRepathAtMs = nowMs;
      continue;
    }

    navAssignPath(world, nav, path.cells);
    nav.pathDirty = false;
    nav.lastRepathAtMs = nowMs;
  }
}

} // namespace game::nav_request_system
