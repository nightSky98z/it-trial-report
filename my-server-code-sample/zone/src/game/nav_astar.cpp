#include "game/nav_astar.h"

#include <algorithm>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace game {
namespace {

struct CompareNode {
    /**
     * @brief オープンリスト（ヒープ）内の 2 つの候補ノードの優先度を比較する
     *
     * @param lhs 比較の左辺。借用するだけで保持しない。
     * @param rhs 比較の右辺。借用するだけで保持しない。
     *
     * @retval true lhs の優先度が rhs より低い。std::push_heap/std::pop_heap と組み合わせて
     *              f/g が最小のものを先頭にするヒープを作る。
     */
  bool operator()(const NavAStarOpenNode &lhs, const NavAStarOpenNode &rhs) const {
    if (lhs.f != rhs.f) return lhs.f > rhs.f;
    if (lhs.g != rhs.g) return lhs.g > rhs.g;
    return lhs.pushOrder > rhs.pushOrder;
  }
};

/**
 * @brief nav グリッドを A* が安全に読めるかを検証する
 *
 * @param grid 呼び出し側が渡す静的グリッド。blocked/areaIds は借用した span。
 *
 * @retval true cellSize/width/height が妥当で、blocked/areaIds が width*height 分を満たしている。
 * @retval false 呼び出し側は「現在は到達可能な経路がない」とみなし、セル番号で span を読んではならない。
 */
bool isValidGrid(const NavGridStatic &grid) {
  if (grid.cellSize <= 0 || grid.width <= 0 || grid.height <= 0) {
    return false;
  }

  const std::size_t width = static_cast<std::size_t>(grid.width);
  const std::size_t height = static_cast<std::size_t>(grid.height);
  if (width == 0 || height == 0) {
    return false;
  }

  if (width > std::numeric_limits<std::size_t>::max() / height) {
    return false;
  }

  const std::size_t cellCount = width * height;
  return grid.blocked.size() >= cellCount && grid.areaIds.size() >= cellCount;
}

/**
 * @brief グリッドのセルを連続配列のインデックスに変換する
 *
 * @param grid isValidGrid の検証を通過した静的グリッド。
 * @param cell グリッド内にあることを確認済みのセル。
 *
 * @return 行優先（row-major）のインデックス。
 *
 * @note
 * - 呼び出し前に cell がグリッド内にあることを保証すること。負の座標を size_t に変換すると範囲外アクセスになる。
 */
std::size_t cellIndex(const NavGridStatic &grid, NavCell cell) {
  return static_cast<std::size_t>(cell.y) * static_cast<std::size_t>(grid.width) +
         static_cast<std::size_t>(cell.x);
}

/**
 * @brief 4 近傍 A* 用のマンハッタン距離ヒューリスティックを計算する
 *
 * @param a 開始セル（値渡し）。
 * @param b 目標セル（値渡し）。
 *
 * @return 0 以上の推定コスト。現在のメインシティのグリッドサイズでは int がオーバーフローしないことを保証している。
 */
int heuristic(NavCell a, NavCell b) {
  return std::abs(a.x - b.x) + std::abs(a.y - b.y);
}

/**
 * @brief A* 1 回分の呼び出しで外部に見える経路状態をクリアする
 *
 * @param scratch 呼び出し側が所有する A* の一時メモリプール。
 *
 * @note
 * - クリアするのは今回の探索経路とオープンリストだけ。gScore/cameFrom/closed の有効範囲は
 *   prepareSearchScratch がリセットする。
 * - 呼び出し後、以前の NavPathResult.cells の借用は直ちに無効になる。
 */
void clearSearchScratch(NavAStarScratch &scratch) {
  scratch.openHeap.clear();
  scratch.reversePath.clear();
  scratch.pathCells.clear();
}

/**
 * @brief A* 1 回分の探索に必要な固定長のスクラッチ領域を準備する
 *
 * @param scratch 呼び出し側が所有する A* の一時メモリプール。
 * @param cellCount 現在のグリッドのセル総数。検証済みのグリッドから求めた値であること。
 *
 * @note
 * - vector の capacity を再利用し、有効範囲 [0, cellCount) だけを初期化する。
 * - 以前により大きなグリッドで確保した capacity は縮めない。
 */
void prepareSearchScratch(NavAStarScratch &scratch, std::size_t cellCount) {
  clearSearchScratch(scratch);

  if (scratch.gScore.size() < cellCount) {
    scratch.gScore.resize(cellCount);
  }
  if (scratch.cameFrom.size() < cellCount) {
    scratch.cameFrom.resize(cellCount);
  }
  if (scratch.closed.size() < cellCount) {
    scratch.closed.resize(cellCount);
  }

  std::fill_n(scratch.gScore.data(), cellCount, std::numeric_limits<int>::max());
  std::fill_n(scratch.cameFrom.data(), cellCount, -1);
  std::fill_n(scratch.closed.data(), cellCount, uint8_t{0});

  if (scratch.openHeap.capacity() < cellCount) {
    scratch.openHeap.reserve(cellCount);
  }
}

/**
 * @brief オープンリストに候補ノードを 1 つ追加する
 *
 * @param heap NavAStarScratch.openHeap の可変借用。
 * @param node 追加する候補ノード。ヒープに値でコピーする。
 *
 * @note
 * - heap は現在の A* 呼び出しだけが使うこと。同じ scratch をスレッド間で共有しないこと。
 */
void pushOpenNode(std::vector<NavAStarOpenNode> &heap,
                  NavAStarOpenNode node) {
  heap.push_back(node);
  std::push_heap(heap.begin(), heap.end(), CompareNode{});
}

/**
 * @brief 現在もっとも優先度の高いオープンノードを取り出す
 *
 * @param heap 空でないオープンリストの可変借用。
 *
 * @return f/g/pushOrder が最小の候補ノード。
 *
 * @note
 * - 呼び出し前に heap が空でないことを保証すること。
 */
NavAStarOpenNode popOpenNode(std::vector<NavAStarOpenNode> &heap) {
  std::pop_heap(heap.begin(), heap.end(), CompareNode{});
  NavAStarOpenNode node = heap.back();
  heap.pop_back();
  return node;
}

} // namespace

NavPathResult findPathAStar(const NavGridStatic &grid, NavCell start, NavCell goal,
                            NavAStarScratch &scratch) {
  NavPathResult result{};
  clearSearchScratch(scratch);
  if (!isValidGrid(grid)) {
    return result;
  }

  start = navIsWalkable(grid, start)
              ? start
              : findNearestWalkableCell(grid, start, scratch.nearestWalkable);
  goal = navIsWalkable(grid, goal)
             ? goal
             : findNearestWalkableCell(grid, goal, scratch.nearestWalkable);

  if (!navIsWalkable(grid, start) || !navIsWalkable(grid, goal)) {
    return result;
  }

  if (!navIsInside(grid, start) || !navIsInside(grid, goal)) {
    return result;
  }

  const std::size_t width = static_cast<std::size_t>(grid.width);
  const std::size_t height = static_cast<std::size_t>(grid.height);
  const std::size_t cellCount = width * height;

  prepareSearchScratch(scratch, cellCount);
  std::size_t nextPushOrder = 0;

  const std::size_t startIndex = cellIndex(grid, start);
  const std::size_t goalIndex = cellIndex(grid, goal);
  scratch.gScore[startIndex] = 0;
  pushOpenNode(scratch.openHeap, NavAStarOpenNode{
      .f = heuristic(start, goal),
      .g = 0,
      .pushOrder = nextPushOrder++,
      .cell = start,
  });

  const NavCell neighbors[4] = {
      NavCell{0, -1},
      NavCell{-1, 0},
      NavCell{1, 0},
      NavCell{0, 1},
  };

  while (!scratch.openHeap.empty()) {
    const NavAStarOpenNode current = popOpenNode(scratch.openHeap);

    if (!navIsInside(grid, current.cell)) {
      continue;
    }

    const std::size_t currentIndex = cellIndex(grid, current.cell);
    if (scratch.closed[currentIndex] != 0) {
      continue;
    }
    scratch.closed[currentIndex] = 1;

    if (currentIndex == goalIndex) {
      result.found = true;
      break;
    }

    for (const NavCell delta : neighbors) {
      const NavCell next{current.cell.x + delta.x, current.cell.y + delta.y};
      if (!navIsInside(grid, next) || !navIsWalkable(grid, next)) {
        continue;
      }

      const std::size_t nextIndex = cellIndex(grid, next);
      if (scratch.closed[nextIndex] != 0) {
        continue;
      }

      const int tentativeG = current.g + 1;
      if (tentativeG >= scratch.gScore[nextIndex]) {
        continue;
      }

      scratch.cameFrom[nextIndex] = static_cast<int>(currentIndex);
      scratch.gScore[nextIndex] = tentativeG;
      pushOpenNode(scratch.openHeap, NavAStarOpenNode{
          .f = tentativeG + heuristic(next, goal),
          .g = tentativeG,
          .pushOrder = nextPushOrder++,
          .cell = next,
      });
    }
  }

  if (!result.found) {
    return result;
  }

  scratch.reversePath.clear();
  for (std::size_t index = goalIndex;;
       index = static_cast<std::size_t>(scratch.cameFrom[index])) {
    const int parent = scratch.cameFrom[index];
    scratch.reversePath.push_back(NavCell{
        static_cast<int>(index % width),
        static_cast<int>(index / width),
    });
    if (index == startIndex) {
      break;
    }
    if (parent < 0) {
      result.found = false;
      scratch.pathCells.clear();
      result.cells = std::span<const NavCell>{};
      return result;
    }
  }

  scratch.pathCells.clear();
  if (scratch.pathCells.capacity() < scratch.reversePath.size()) {
    scratch.pathCells.reserve(scratch.reversePath.size());
  }
  for (auto it = scratch.reversePath.rbegin(); it != scratch.reversePath.rend(); ++it) {
    scratch.pathCells.push_back(*it);
  }
  result.cells = std::span<const NavCell>{scratch.pathCells.data(), scratch.pathCells.size()};
  return result;
}

} // namespace game
