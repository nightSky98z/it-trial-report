#include "game/nav_grid.h"

#include "generated/main_city_nav_32x32.h"

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

namespace game {
namespace {

/**
 * @brief データテーブル／プロトコルで area_id=0 は無効なエリアを表す
 *
 * @note
 * - ランダムな出現地点や巡回地点の選択では 0 を必ず除外し、未設定のエリアに入らないようにする。
 */
constexpr int kInvalidAreaId = 0;

/**
 * @brief 現在のメインシティの静的 nav データに含まれる最大のエリア ID
 *
 * @note
 * - areaWalkableCache は固定長配列のインデックスで引くため、エリアを追加したら配列サイズも合わせて広げること。
 */
constexpr int kMaxAreaId = 4;

/**
 * @brief 整数のセル座標がグリッドの矩形内にあるかを判定する
 *
 * @param grid 静的な nav グリッド。width/height だけを読む。
 * @param x セルの X 座標。
 * @param y セルの Y 座標。
 *
 * @retval true 座標を安全に行優先のインデックスへ変換できる。
 */
constexpr bool isInsideBounds(const NavGridStatic &grid, int x, int y) {
  return x >= 0 && x < grid.width && y >= 0 && y < grid.height;
}

/**
 * @brief 検証済みのセル座標を行優先のインデックスに変換する
 *
 * @param grid width > 0 の静的な nav グリッド。
 * @param x [0, width) にあることを確認済みの X 座標。
 * @param y [0, height) にあることを確認済みの Y 座標。
 *
 * @return blocked/areaIds の span のインデックス。
 *
 * @note
 * - 呼び出し前に座標がグリッド内にあることを保証すること。
 */
constexpr std::size_t cellIndex(const NavGridStatic &grid, int x, int y) {
  return static_cast<std::size_t>(y) * static_cast<std::size_t>(grid.width) +
         static_cast<std::size_t>(x);
}

/**
 * @brief グリッドのセル総数を計算する（size_t のオーバーフローも防ぐ）
 *
 * @param grid 静的な nav グリッド。
 * @param out 成功時に width*height を書き込む。
 *
 * @retval true out に有効なセル数を書き込んだ。
 * @retval false width/height が正でない、または乗算がオーバーフローする。
 */
bool navCellCount(const NavGridStatic &grid, std::size_t &out) {
  if (grid.width <= 0 || grid.height <= 0) {
    return false;
  }

  const std::size_t width = static_cast<std::size_t>(grid.width);
  const std::size_t height = static_cast<std::size_t>(grid.height);
  if (width > std::numeric_limits<std::size_t>::max() / height) {
    return false;
  }

  out = width * height;
  return true;
}

/**
 * @brief blocked の span がグリッド全体を満たしているかを確認する
 *
 * @param grid 静的な nav グリッド。blocked は借用した span。
 *
 * @retval true navIsWalkable がグリッド内の任意のセルを安全に読める。
 */
bool hasBlockedStorage(const NavGridStatic &grid) {
  std::size_t cellCount = 0;
  return navCellCount(grid, cellCount) && grid.blocked.size() >= cellCount;
}

/**
 * @brief areaIds の span がグリッド全体を満たしているかを確認する
 *
 * @param grid 静的な nav グリッド。areaIds は借用した span。
 *
 * @retval true navAreaIdAt がグリッド内の任意のセルを安全に読める。
 */
bool hasAreaStorage(const NavGridStatic &grid) {
  std::size_t cellCount = 0;
  return navCellCount(grid, cellCount) && grid.areaIds.size() >= cellCount;
}

/**
 * @brief セルをグリッドの矩形内に丸める
 *
 * @param grid 静的な nav グリッド。
 * @param cell グリッド外かもしれない候補セル。
 *
 * @return width/height が有効なら、境界内に収めたセル。
 * @return width/height が無効なら、デフォルトの NavCell{}。
 */
NavCell clampToGrid(const NavGridStatic &grid, NavCell cell) {
  if (grid.width <= 0 || grid.height <= 0) {
    return NavCell{};
  }

  cell.x = std::clamp(cell.x, 0, grid.width - 1);
  cell.y = std::clamp(cell.y, 0, grid.height - 1);
  return cell;
}

/**
 * @brief メインシティのエリアごとの通行可能セルのキャッシュを作る
 *
 * @return areaId で引ける通行可能セルのリスト。0 番は無効エリア用に予約している。
 *
 * @note
 * - mainCityNavGrid の静的データ専用。メインシティ以外のグリッドはその場で走査する経路を通る。
 */
std::array<std::vector<NavCell>, kMaxAreaId + 1> buildAreaWalkableCache() {
  std::array<std::vector<NavCell>, kMaxAreaId + 1> cache{};
  const auto &grid = mainCityNavGrid();

  for (int y = 0; y < grid.height; ++y) {
    for (int x = 0; x < grid.width; ++x) {
      const std::size_t index = cellIndex(grid, x, y);
      const uint32_t areaId = grid.areaIds[index];
      if (areaId > kInvalidAreaId && areaId <= kMaxAreaId && grid.blocked[index] == 0) {
        cache[areaId].push_back(NavCell{x, y});
      }
    }
  }

  return cache;
}

/**
 * @brief メインシティのエリア別通行可能セルのキャッシュを返す
 *
 * @return プロセス内の静的キャッシュへの参照。プロセス終了まで有効。
 *
 * @note
 * - 戻り値は読み取り専用。中の vector への可変参照を保持しないこと。
 */
const std::array<std::vector<NavCell>, kMaxAreaId + 1> &areaWalkableCache() {
  static const std::array<std::vector<NavCell>, kMaxAreaId + 1> cache = buildAreaWalkableCache();
  return cache;
}

} // namespace

const NavGridStatic &mainCityNavGrid() {
  static const NavGridStatic grid = [] {
    NavGridStatic value{};
    value.cellSize = generated::kMainCityNavCellSize;
    value.minX = generated::kMainCityNavMinX;
    value.minY = generated::kMainCityNavMinY;
    value.width = generated::kMainCityNavWidth;
    value.height = generated::kMainCityNavHeight;
    value.blocked = std::span<const uint8_t>(generated::kMainCityNavBlocked);
    value.areaIds = std::span<const uint8_t>(generated::kMainCityNavAreaIds);
    value.healingStationCell = generated::kMainCityNavHealingStationCell;
    value.playerSpawnCell = generated::kMainCityNavPlayerSpawnCell;
    value.areaAnchorCells = generated::kMainCityNavAreaAnchorCells;
    return value;
  }();

  return grid;
}

bool navIsInside(const NavGridStatic &grid, NavCell cell) {
  return isInsideBounds(grid, cell.x, cell.y);
}

bool navIsWalkable(const NavGridStatic &grid, NavCell cell) {
  if (!navIsInside(grid, cell) || !hasBlockedStorage(grid)) {
    return false;
  }

  return grid.blocked[cellIndex(grid, cell.x, cell.y)] == 0;
}

uint32_t navAreaIdAt(const NavGridStatic &grid, NavCell cell) {
  if (!navIsInside(grid, cell) || !hasAreaStorage(grid)) {
    return 0;
  }

  return static_cast<uint32_t>(grid.areaIds[cellIndex(grid, cell.x, cell.y)]);
}

NavCell navWorldToCell(const NavGridStatic &grid, float x, float y) {
  if (grid.cellSize <= 0 || !std::isfinite(x) || !std::isfinite(y)) {
    return NavCell{-1, -1};
  }

  const float cellSize = static_cast<float>(grid.cellSize);
  const float cellX = std::floor((x - static_cast<float>(grid.minX)) / cellSize);
  const float cellY = std::floor((y - static_cast<float>(grid.minY)) / cellSize);
  return NavCell{static_cast<int>(cellX), static_cast<int>(cellY)};
}

Position navCellCenterToWorld(const NavGridStatic &grid, NavCell cell) {
  const float halfCell = static_cast<float>(grid.cellSize) * 0.5f;
  return Position{
      static_cast<float>(grid.minX + cell.x * grid.cellSize) + halfCell,
      static_cast<float>(grid.minY + cell.y * grid.cellSize) + halfCell,
  };
}

NavCell findNearestWalkableCell(const NavGridStatic &grid, NavCell desired,
                                NavNearestWalkableScratch &scratch) {
  if (navIsWalkable(grid, desired)) {
    return desired;
  }

  std::size_t cellCount = 0;
  if (!navCellCount(grid, cellCount)) {
    return desired;
  }

  desired = clampToGrid(grid, desired);

  if (scratch.visited.size() < cellCount) {
    scratch.visited.resize(cellCount);
  }
  std::fill_n(scratch.visited.data(), cellCount, uint8_t{0});

  scratch.queue.clear();
  if (scratch.queue.capacity() < cellCount) {
    scratch.queue.reserve(cellCount);
  }

  const auto pushCell = [&](NavCell cell) {
    if (!navIsInside(grid, cell)) {
      return;
    }

    const std::size_t index = cellIndex(grid, cell.x, cell.y);
    if (scratch.visited[index] != 0) {
      return;
    }

    scratch.visited[index] = 1;
    scratch.queue.push_back(cell);
  };

  pushCell(desired);

  for (std::size_t head = 0; head < scratch.queue.size(); ++head) {
    const NavCell current = scratch.queue[head];
    if (navIsWalkable(grid, current)) {
      return current;
    }

    pushCell(NavCell{current.x, current.y - 1});
    pushCell(NavCell{current.x - 1, current.y});
    pushCell(NavCell{current.x + 1, current.y});
    pushCell(NavCell{current.x, current.y + 1});
  }

  return grid.playerSpawnCell;
}

NavCell pickRandomWalkableCellInArea(const NavGridStatic &grid, uint32_t areaId,
                                    NavAreaPickScratch &scratch) {
  if (areaId == kInvalidAreaId || areaId > kMaxAreaId) {
    return grid.playerSpawnCell;
  }

  std::size_t cellCount = 0;
  if (!navCellCount(grid, cellCount) ||
      grid.blocked.size() < cellCount ||
      grid.areaIds.size() < cellCount) {
    return grid.playerSpawnCell;
  }

  const std::vector<NavCell> *cells = nullptr;
  if (&grid == &mainCityNavGrid()) {
    cells = &areaWalkableCache()[areaId];
  } else {
    scratch.cells.clear();
    if (scratch.cells.capacity() < cellCount) {
      scratch.cells.reserve(cellCount);
    }

    for (int y = 0; y < grid.height; ++y) {
      for (int x = 0; x < grid.width; ++x) {
        const NavCell cell{x, y};
        if (navAreaIdAt(grid, cell) == areaId && navIsWalkable(grid, cell)) {
          scratch.cells.push_back(cell);
        }
      }
    }
    cells = &scratch.cells;
  }

  if (cells && !cells->empty()) {
    static thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<std::size_t> dist(0, cells->size() - 1);
    const std::size_t pick = dist(rng);
    return (*cells)[pick];
  }

  const NavCell anchor = grid.areaAnchorCells[areaId];
  if (navAreaIdAt(grid, anchor) == areaId && navIsWalkable(grid, anchor)) {
    return anchor;
  }
  return grid.playerSpawnCell;
}

} // namespace game
