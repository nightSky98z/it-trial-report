#pragma once
#include "ecs/components.h"
#include "ecs/aoi_grid.h"
#include <cmath>
#include <random>

namespace math_utils {

/**
 * @brief 2 つの Position 間の距離の 2 乗を返す
 *
 * @param a 借用する位置 A。
 * @param b 借用する位置 B。
 *
 * @return 2 次元距離の 2 乗。
 */
inline float dist_sq(const Position &a, const Position &b) {
  const float dx = a.x - b.x;
  const float dy = a.y - b.y;
  return dx * dx + dy * dy;
}

/**
 * @brief 2 組の座標間の距離の 2 乗を返す
 *
 * @param ax 点 A の X 座標。
 * @param ay 点 A の Y 座標。
 * @param bx 点 B の X 座標。
 * @param by 点 B の Y 座標。
 *
 * @return 2 次元距離の 2 乗。
 */
inline float dist_sq(float ax, float ay, float bx, float by) {
  const float dx = ax - bx;
  const float dy = ay - by;
  return dx * dx + dy * dy;
}

/**
 * @brief 2 つの位置が AOI の進入半径内にあるかを判定する
 *
 * @retval true 距離の 2 乗が AOI_ENTER_RADIUS_SQ 以下。
 */
inline bool in_enter_range(const Position &a, const Position &b) {
  return dist_sq(a, b) <= AOI_ENTER_RADIUS_SQ;
}

/**
 * @brief 2 つの位置がまだ AOI の離脱半径内にあるかを判定する
 *
 * @retval true 距離の 2 乗が AOI_LEAVE_RADIUS_SQ 以下。
 */
inline bool in_leave_range(const Position &a, const Position &b) {
  return dist_sq(a, b) <= AOI_LEAVE_RADIUS_SQ;
}

/**
 * @brief 目標点が円の範囲内にあるかを判定する
 *
 * @param cx 円の中心の X 座標。
 * @param cy 円の中心の Y 座標。
 * @param tx 目標の X 座標。
 * @param ty 目標の Y 座標。
 * @param radius 半径。0 以下は無効として扱う。
 *
 * @retval true 目標が円の内側または境界上にある。
 */
inline bool in_circle(float cx, float cy, float tx, float ty, float radius) {
  if (radius <= 0.f) return false;
  const float dx = tx - cx;
  const float dy = ty - cy;
  return dx * dx + dy * dy <= radius * radius;
}

/**
 * @brief 指定範囲の一様乱数（float）を返す
 *
 * @param lo 範囲の下限。
 * @param hi 範囲の上限。
 *
 * @return 現在のスレッドの乱数生成器で作った値。端点を含むかどうかは
 *         std::uniform_real_distribution の仕様に従う。
 *
 * @note
 * - 関数内部に thread_local の乱数生成器を持つ。シードを固定するためのインターフェースはない。
 */
inline float random_uniform(float lo, float hi) {
  thread_local std::mt19937 rng{std::random_device{}()};
  std::uniform_real_distribution<float> dist(lo, hi);
  return dist(rng);
}

} // namespace math_utils
