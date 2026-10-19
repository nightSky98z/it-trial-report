#pragma once
#include "ecs/components.h" // FacingDir
#include <cmath>

namespace sensor_utils {

/**
 * @brief 円周率 π
 *
 * @note
 * - センサーの角度をラジアンに変換するときだけ使う。
 */
inline constexpr float kPi = 3.14159265358979323846f;

/**
 * @brief ローカル座標のオフセットを向きに合わせて回転する
 *
 * @param facing 現在の向き。基準方向は Down。
 * @param ox ローカルの横方向オフセット。
 * @param oy ローカルの縦方向オフセット。
 * @param rx 出力：ワールドの横方向オフセット。
 * @param ry 出力：ワールドの縦方向オフセット。
 */
inline void rotate_offset(FacingDir facing, float ox, float oy,
                          float &rx, float &ry) {
  switch (facing) {
  case FacingDir::Down:  rx =  ox; ry =  oy; return;
  case FacingDir::Up:    rx = -ox; ry = -oy; return;
  case FacingDir::Left:  rx =  oy; ry = -ox; return; // -90° 回転
  case FacingDir::Right: rx = -oy; ry =  ox; return; // +90° 回転
  }
  rx = ox; ry = oy;
}

/**
 * @brief 向きに対応する前方の単位ベクトルを返す
 *
 * @param facing 現在の向き。
 * @param fx 出力：横方向の単位成分。
 * @param fy 出力：縦方向の単位成分。
 */
inline void forward_dir(FacingDir facing, float &fx, float &fy) {
  switch (facing) {
  case FacingDir::Down:  fx =  0.f; fy = -1.f; return;
  case FacingDir::Up:    fx =  0.f; fy =  1.f; return;
  case FacingDir::Left:  fx = -1.f; fy =  0.f; return;
  case FacingDir::Right: fx =  1.f; fy =  0.f; return;
  }
  fx = 0.f; fy = -1.f;
}

/**
 * @brief ワールド座標系の方向を、向きを基準にしたローカル方向に変換する
 *
 * @param facing 現在の向き。ローカル座標は Down を基準とする。
 * @param dx ワールドの横方向の差分。
 * @param dy ワールドの縦方向の差分。
 * @param lx 出力：ローカルの横方向の差分。
 * @param ly 出力：ローカルの縦方向の差分。
 */
inline void world_to_local(FacingDir facing, float dx, float dy,
                           float &lx, float &ly) {
  switch (facing) {
  case FacingDir::Down:  lx =  dx; ly =  dy; return;
  case FacingDir::Up:    lx = -dx; ly = -dy; return;
  case FacingDir::Left:  lx = -dy; ly =  dx; return;
  case FacingDir::Right: lx =  dy; ly = -dx; return;
  }
  lx = dx; ly = dy;
}

/**
 * @brief 目標点が、向きを持つ矩形の範囲内にあるかを判定する
 *
 * @param cx 矩形の中心の X 座標。
 * @param cy 矩形の中心の Y 座標。
 * @param tx 目標の X 座標。
 * @param ty 目標の Y 座標。
 * @param width 矩形の幅。0 以下は無効。
 * @param height 矩形の高さ。0 以下は無効。
 * @param facing 矩形の向き。
 *
 * @retval true 目標が矩形の内側または境界上にある。
 */
inline bool in_rect(float cx, float cy, float tx, float ty,
                    float width, float height, FacingDir facing) {
  if (width <= 0.f || height <= 0.f) return false;
  float lx = 0.f, ly = 0.f;
  world_to_local(facing, tx - cx, ty - cy, lx, ly);
  return std::fabs(lx) <= width * 0.5f && std::fabs(ly) <= height * 0.5f;
}

/**
 * @brief 目標点が、向きを持つ扇形の範囲内にあるかを判定する
 *
 * @param cx 扇形の中心の X 座標。
 * @param cy 扇形の中心の Y 座標。
 * @param tx 目標の X 座標。
 * @param ty 目標の Y 座標。
 * @param radius 半径。0 以下は無効。
 * @param angle_deg 扇形の角度。[0, 360] に丸める。
 * @param facing 扇形の向き。
 *
 * @retval true 目標が扇形の内側または境界上にある。
 */
inline bool in_sector(float cx, float cy, float tx, float ty,
                      float radius, float angle_deg, FacingDir facing) {
  if (radius <= 0.f || angle_deg <= 0.f) return false;
  const float dx = tx - cx;
  const float dy = ty - cy;
  const float d2 = dx * dx + dy * dy;
  if (d2 > radius * radius) return false;
  if (d2 <= 0.000001f) return true;

  const float half_rad =
      std::clamp(angle_deg, 0.f, 360.f) * 0.5f * (kPi / 180.f);
  if (half_rad >= kPi) return true;

  float fx = 0.f, fy = -1.f;
  forward_dir(facing, fx, fy);
  const float inv_d = 1.f / std::sqrt(d2);
  const float nx    = dx * inv_d;
  const float ny    = dy * inv_d;
  return (fx * nx + fy * ny) >= std::cos(half_rad);
}

} // namespace sensor_utils
