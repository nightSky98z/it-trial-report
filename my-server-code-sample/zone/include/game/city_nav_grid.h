#pragma once
#include "game/building_cache.h"
#include "game/nav_grid.h"
#include "game/role_buildings.h"
#include "utils/log.h"
#include <cstdint>
#include <memory>
#include <vector>
#include <entt/entt.hpp>

/**
 * @file city_nav_grid.h
 * @brief 地形の nav グリッドに建物の形状を重ね、シティごとの実際の通行可能グリッドを作る
 *
 * 地形グリッド（generated/main_city_nav_32x32.h）は地形だけを表し、建物は含まない。
 * 建物は DB から来るため実行時に重ねる必要がある。そうしないと経路探索が建物を素通りしてしまう。
 *
 * @note
 * - 現在は全プレイヤーの建物の位置が同じ（role_buildings.x/y はすべてテンプレートの初期値）なので、
 *   CityNavGrid はサーバー全体で 1 つを共有すればよく、プレイヤーごとのコストはゼロ。
 * - 将来プレイヤーが建物を自由に配置できるようになったら、「初期配置と異なる」シティだけ専用のものを作り、
 *   そのシティの world の ctx をそちらに向ければよい。経路探索や AI のコードは一切変更不要。
 *   「どのグリッドを使うか」をシティごとに選べるようにすることこそ、この型が存在する理由である。
 */
namespace game {

/**
 * @brief 1 つのシティが実際に使う nav グリッド（地形 + 建物の形状）
 *
 * @note
 * - 独自の blocked バッファを持つ。view() が返す NavGridStatic はこのバッファを借用するので、
 *   本オブジェクトの破棄後に使ってはならない。
 * - areaIds は地形グリッドの span をそのまま使う（エリア区分は建物によって変わらない）。
 */
class CityNavGrid {
public:
  CityNavGrid() = default;

    /**
     * @brief 地形グリッドをベースに、建物の形状を重ねる
     *
     * @param base 地形グリッド。blocked のデータはコピーし、areaIds は借用する。
     * @param buildings 建物テンプレートのキャッシュ。当たり判定の半径を提供する。
     * @param placed そのシティに実際にある建物（位置と状態）。
     * @param blockLocked true なら未解放の建物も通行不可にする。
     *
     * @note
     * - 建物の中心がグリッドの外にある場合はスキップして警告を出す。範囲外のメモリには書き込まない。
     */
  void build(const NavGridStatic &base, const cache::BuildingCache &buildings,
             const buildings::RoleBuildings &placed, bool blockLocked) {
    grid_ = base;
    blocked_.assign(base.blocked.begin(), base.blocked.end());

    for (const auto &b : placed.entries) {
      if (!blockLocked && b.state != buildings::kStateUnlocked) continue;

      const cache::BuildingTemplate *const tpl =
          cache::findBuilding(buildings, b.buildingId);
      if (!tpl) continue;

      stampCircle(base, b.x, b.y, tpl->colliderRadius, b.buildingId);
    }

    grid_.blocked = std::span<const uint8_t>(blocked_.data(), blocked_.size());
  }

    /**
     * @brief 経路探索に渡せるグリッドのビューを返す
     *
     * @note
     * - ビューは本オブジェクトのメモリを借用する。本オブジェクトより長く使わないこと。
     */
  const NavGridStatic &view() const { return grid_; }

    /// 通行不可セルの数（地形そのものの通行不可も含む）。起動ログと調査用。
  std::size_t blockedCellCount() const {
    std::size_t n = 0;
    for (const uint8_t v : blocked_) {
      if (v) ++n;
    }
    return n;
  }

private:
    /**
     * @brief (cx, cy) を中心とする半径 radius の範囲を通行不可にする
     *
     * @note
     * - 矩形ではなく円を使う。DB の当たり判定（buildings.default_collider_radius）が
     *   もともと半径の意味で定義されているため。
     * - セルの中心が円内にあればセル全体を通行不可にする。少し大きめでも、ユニットが
     *   建物の端に引っかかるよりはよい。
     */
  void stampCircle(const NavGridStatic &base, float cx, float cy, float radius,
                   uint32_t buildingId) {
    if (radius <= 0.f) return;

    const NavCell center = navWorldToCell(base, cx, cy);
    if (!navIsInside(base, center)) {
      zone_log::warning(
          "city nav: building id=%u at (%g,%g) outside nav grid, skipped",
          buildingId, cx, cy);
      return;
    }

    const int span = static_cast<int>(radius / static_cast<float>(base.cellSize)) + 1;
    const float r2 = radius * radius;

    for (int dy = -span; dy <= span; ++dy) {
      for (int dx = -span; dx <= span; ++dx) {
        const NavCell c{center.x + dx, center.y + dy};
        if (!navIsInside(base, c)) continue;

        const Position p = navCellCenterToWorld(base, c);
        const float ox = p.x - cx;
        const float oy = p.y - cy;
        if (ox * ox + oy * oy > r2) continue;

        const std::size_t idx =
            static_cast<std::size_t>(c.y - 0) * static_cast<std::size_t>(base.width) +
            static_cast<std::size_t>(c.x);
        if (idx < blocked_.size()) blocked_[idx] = 1;
      }
    }
  }

  NavGridStatic grid_{};
  std::vector<uint8_t> blocked_;
};

/**
 * @brief このマップインスタンスが使う nav グリッド（ctx シングルトン）
 *
 * @note
 * - シティ生成時に書き込み、サーバー全体で共有するデフォルトのシティグリッドを指す。
 *   将来あるシティが独自の建物配置を持ったら、そのシティ専用のグリッドを指すように変える。
 * - ポインタはその world の生存期間中、有効であり続けなければならない。
 */
struct CCityNavGrid {
  const NavGridStatic *grid = nullptr;
};

/**
 * @brief このマップインスタンスが使うべき nav グリッドを取得する
 *
 * @param world マップの ECS world。
 *
 * @return そのシティのグリッド。未設定の場合は建物を含まない地形グリッドにフォールバックする。
 *
 * @note
 * - システムはグリッドを必ずこの関数で取得すること。mainCityNavGrid() を直接呼ばないこと。
 *   直接呼ぶと建物の通行不可が反映されず、将来プレイヤーが配置を変えたときに誤ったグリッドを使ってしまう。
 */
inline const NavGridStatic &navGridOf(const entt::registry &world) {
  if (const auto *ctx = world.ctx().find<const CCityNavGrid>()) {
    if (ctx->grid) return *ctx->grid;
  }
  return mainCityNavGrid();
}

} // namespace game
