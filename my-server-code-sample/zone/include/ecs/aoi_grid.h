#pragma once

#include <cstddef>
#include <entt/entt.hpp>
#include <functional>

/**
 * @brief AOI 空間ハッシュの 1 セルの一辺の長さ
 *
 * @note
 * - rebuild と query_neighbors は同じグリッドパラメータを使うこと。ずれると近傍検索の結果が狂う。
 */
static constexpr float CELL_SIZE = 500.f;

/**
 * @brief AOI で視界に入る半径
 *
 * @note
 * - ワールド座標の単位。進入判定にだけ使う。
 */
static constexpr float AOI_ENTER_RADIUS = 900.f;

/**
 * @brief AOI で視界から外れる半径
 *
 * @note
 * - 進入半径以上にすること。ヒステリシスを持たせて、境界付近での出入りのちらつきを防ぐ。
 */
static constexpr float AOI_LEAVE_RADIUS = 940.f;
static_assert(AOI_LEAVE_RADIUS >= AOI_ENTER_RADIUS,
              "AOI_LEAVE_RADIUS must be >= AOI_ENTER_RADIUS");

/**
 * @brief AOI 進入半径の 2 乗
 *
 * @note
 * - 距離判定は 2 乗のまま比較し、毎回の sqrt を避ける。
 */
static constexpr float AOI_ENTER_RADIUS_SQ = AOI_ENTER_RADIUS * AOI_ENTER_RADIUS;

/**
 * @brief AOI 離脱半径の 2 乗
 *
 * @note
 * - 距離判定は 2 乗のまま比較し、毎回の sqrt を避ける。
 */
static constexpr float AOI_LEAVE_RADIUS_SQ = AOI_LEAVE_RADIUS * AOI_LEAVE_RADIUS;

/**
 * @brief AOI グリッドのキー
 *
 * @note
 * - x/y は floor(world_position / CELL_SIZE) で求める。負の値もありうる。
 */
struct CellKey {
  int x, y;
  bool operator==(const CellKey &o) const { return x == o.x && y == o.y; }
};

/**
 * @brief CellKey のハッシュ関数
 *
 * @note
 * - unordered_map 専用。プロセスをまたいで安定したハッシュ値になることは保証しない。
 */
struct CellKeyHash {
  std::size_t operator()(const CellKey &k) const {
        // 簡易ハッシュ：2 つの int を 1 つの 64 ビット整数にまとめる
    return std::hash<uint64_t>{}((uint64_t)(uint32_t)k.x << 32 | (uint32_t)k.y);
  }
};

/**
 * @brief AOI 空間ハッシュテーブル
 *
 * @note
 * - vector には entt::entity の値を入れるだけで、エンティティの寿命は管理しない。
 * - rebuild は各セルの vector をクリアし、メモリを再利用する。
 */
using AoiGrid =
    std::unordered_map<CellKey, std::vector<entt::entity>, CellKeyHash>;
