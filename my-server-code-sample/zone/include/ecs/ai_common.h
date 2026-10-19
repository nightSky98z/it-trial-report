#pragma once
#include <cmath>
#include <cstdint>

namespace ai_common {

/**
 * @brief FlatBuffers の Facing* 定数と対応した 4 方向の列挙
 *
 * @note
 * - 戻り値はそのまま UnitBase.facing とプロトコルのスナップショットに書き込める。
 */
enum FacingDir : uint8_t {
  FacingDown  = 0,
  FacingUp    = 1,
  FacingLeft  = 2,
  FacingRight = 3,
};

/**
 * @brief この速度未満のときは前フレームの向きを維持する
 *
 * @note
 * - AI と AOI の動的スナップショットで、静止中に facing がちらつくのを防ぐためのしきい値。
 */
inline constexpr float kAoiVelocityEpsilon = 0.01f;

/**
 * @brief 2 次元ベクトルの長さの 2 乗を返す
 *
 * @param x 横成分。
 * @param y 縦成分。
 *
 * @return x*x + y*y。
 */
inline float lengthSquared(float x, float y) {
  return x * x + y * y;
}

/**
 * @brief 2 次元ベクトルをその場で正規化する
 *
 * @param x 横成分への参照。正規化した結果を書き戻す。
 * @param y 縦成分への参照。正規化した結果を書き戻す。
 *
 * @return 正規化する前のベクトルの長さ。
 * @retval 0 入力の長さが 0。x/y には 0 を書き込む。
 */
inline float normalizeOrZero(float &x, float &y) {
  const float lenSq = lengthSquared(x, y);
  if (lenSq <= 0.f) {
    x = 0.f;
    y = 0.f;
    return 0.f;
  }
  const float len = std::sqrt(lenSq);
  x /= len;
  y /= len;
  return len;
}

/**
 * @brief 速度から 4 方向の向き（facing）を求める
 *
 * @param vx 横方向の速度。
 * @param vy 縦方向の速度。
 * @param fallback 速度がほぼ 0 のときに維持する、元の向き。
 *
 * @return 新しい向き。静止している場合は fallback。
 */
inline uint8_t facingFromVelocity(float vx, float vy, uint8_t fallback) {
  if (std::fabs(vx) <= kAoiVelocityEpsilon &&
      std::fabs(vy) <= kAoiVelocityEpsilon) {
    return fallback;
  }
  if (std::fabs(vx) >= std::fabs(vy)) {
    return vx >= 0.f ? FacingRight : FacingLeft;
  }
  return vy >= 0.f ? FacingUp : FacingDown;
}

} // namespace ai_common
