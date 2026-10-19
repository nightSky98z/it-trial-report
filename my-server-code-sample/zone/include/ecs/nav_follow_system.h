#pragma once

#include "game/nav_grid.h"

#include <entt/entt.hpp>

namespace game::nav_follow_system {

/**
 * @brief 現在のナビゲーション経路に沿って、ユニットの速度と到着状態を設定する
 *
 * @param world マップの ECS world。CNavAgent・Velocity・UnitBase の actionState を更新する。
 * @param grid 静的な nav グリッド。読み取りのみで、参照は保持しない。
 *
 * @note
 * - nav_request_system が経路を更新した後に呼ぶこと。
 * - 速度を位置に反映するため、呼び出し側はこの後に velocity_system を実行すること。
 */
void update(entt::registry &world, const NavGridStatic &grid);

} // namespace game::nav_follow_system
