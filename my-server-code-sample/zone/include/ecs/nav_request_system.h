#pragma once

#include "game/nav_grid.h"

#include <cstdint>
#include <entt/entt.hpp>

namespace game::nav_request_system {

/**
 * @brief アクティブなナビゲーションエージェントの経路を生成・修復する
 *
 * @param world マップの ECS world。CNavAgent/UnitBase を読み、必要に応じて NavPathStore と
 *              A* 用のスクラッチ領域を作成する。
 * @param grid 静的な nav グリッド。経路はこのグリッドのセル単位で計算する。
 * @param nowMs 現在のロジック時刻（ミリ秒）。再探索のクールダウン判定に使う。
 *
 * @note
 * - AI が goalCell/pathDirty を書き込んだ後、nav_follow_system より前に呼ぶこと。
 * - 古い経路スライスを解放し、新しい経路を NavPathStore にコピーする。
 */
void update(entt::registry &world, const NavGridStatic &grid, uint64_t nowMs);

} // namespace game::nav_request_system
