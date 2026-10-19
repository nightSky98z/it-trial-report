#include "ecs/aoi_system.h"
#include "ecs/map_components.h"
#include "ecs/registry.h"
#include <cmath>
#include <unordered_map>
#include <vector>

/**
 * @brief ワールド座標を AOI のセルに変換する
 *
 * @return floor(world / CELL_SIZE) で求めた CellKey。
 */
static CellKey to_cell(float x, float y) {
  return {(int)std::floor(x / CELL_SIZE), (int)std::floor(y / CELL_SIZE)};
}

/**
 * @brief AOI のグリッドインデックスを再構築する
 *
 * @note
 * - 各セルの entity vector はクリアするが、確保済みの容量は再利用する。
 */
void aoi_system::rebuild(ZoneRegistry &zone) {
    // 全セルを空にする（確保済みのメモリは再利用）
  for (auto &[key, vec] : zone.grid)
    vec.clear();

    // 位置を持つエンティティを、それぞれ該当するセルに入れる
  auto view = zone.registry.view<Position>();
  view.each([&](entt::entity entity, const Position &pos) {
    auto key = to_cell(pos.x, pos.y);
    zone.grid[key].push_back(entity);
  });
}

/**
 * @brief AOI 離脱半径がかかるセルにいる候補エンティティを列挙する
 *
 * @return out に入れた候補エンティティの数。
 */
size_t aoi_system::query_neighbors(ZoneRegistry &zone, entt::entity entity,
                                   std::vector<entt::entity> &out) {
  const auto &pos = zone.registry.get<Position>(entity);
  auto center = to_cell(pos.x, pos.y);
  const int cell_range =
      static_cast<int>(std::ceil(AOI_LEAVE_RADIUS / CELL_SIZE));

  out.clear();
  for (int dy = -cell_range; dy <= cell_range; ++dy) {
    for (int dx = -cell_range; dx <= cell_range; ++dx) {
      CellKey k{center.x + dx, center.y + dy};
      auto it = zone.grid.find(k);
      if (it == zone.grid.end())
        continue;
      for (auto e : it->second) {
        out.push_back(e);
      }
    }
  }
  return out.size();
}
