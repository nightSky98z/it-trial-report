#pragma once
#include "ecs/map_components.h"
#include "generated/game_generated.h"
#include <cmath>

namespace velocity_system {

namespace {

/**
 * @brief この速度未満のときは前フレームの向きを維持する
 *
 * @note
 * - AOI の動的スナップショットで、静止中に facing がちらつくのを防ぐためのしきい値。
 */
constexpr float kAoiVelocityEpsilon = 0.01f;

/**
 * @brief FlatBuffers の Facing* 定数と対応した 4 方向の列挙
 *
 * @note
 * - 戻り値はそのまま UnitBase.facing とプロトコルのスナップショットに書き込む。
 */
enum FacingDir : uint8_t {
  FacingDown = 0,
  FacingUp = 1,
  FacingLeft = 2,
  FacingRight = 3,
};

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

} // namespace

/**
 * @brief Velocity を UnitBase の座標に積分する
 *
 * @param world マップの ECS world。UnitBase の位置・facing・actionState を更新する。
 * @param dtSec このフレームのシミュレーション秒数。
 *
 * @note
 * - Velocity を書き込むすべてのシステムの後に呼ぶこと。
 * - 静止しているのに Move 状態のエンティティは Idle に戻す。
 */
inline void update(entt::registry &world, float dtSec) {
  auto view = world.view<NetworkId, Velocity, UnitBase>();
  for (auto e : view) {
    auto &velocity = view.get<Velocity>(e);
    auto &base = view.get<UnitBase>(e);
    if (velocity.vx == 0.f && velocity.vy == 0.f) {
      if (base.actionState == static_cast<uint8_t>(Game::ActionState_Move)) {
        base.actionState = static_cast<uint8_t>(Game::ActionState_Idle);
      }
      continue;
    }

    base.x += velocity.vx * dtSec;
    base.y += velocity.vy * dtSec;
    base.facing = facingFromVelocity(velocity.vx, velocity.vy, base.facing);
  }
}

} // namespace velocity_system
