#pragma once

#include "game/nav_grid.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace game {

/**
 * @brief A* のオープンリスト（ヒープ）に入る候補ノード
 *
 * @note
 * - NavAStarScratch / findPathAStar からのみ使う。
 * - pushOrder により、f/g が同じ候補でも取り出し順が安定する。
 */
struct NavAStarOpenNode {
  int f = 0;
  int g = 0;
  std::size_t pushOrder = 0;
  NavCell cell{};
};

/**
 * @brief A* 1 回分の探索結果
 *
 * @note
 * - cells は NavAStarScratch.pathCells を借用しているだけで、メモリは所有しない。
 * - 同じ scratch で次に findPathAStar を呼ぶと、以前の cells は無効になる。
 * - found == false のとき cells は空。
 */
struct NavPathResult {
  bool found = false;
  std::span<const NavCell> cells;
};

/**
 * @brief A* が内部で使う一時メモリの所有者
 *
 * @note
 * - world／スレッドごとに専用の scratch を持つ。同じインスタンスをスレッド間で共有しないこと。
 * - findPathAStar はこれらの vector の capacity を再利用し、呼び出しの最初に今回の探索状態をクリアする。
 * - pathCells には found=true だった今回の結果だけを保持する。失敗した呼び出しではクリアされる。
 * - nearestWalkable は探索用メモリを再利用するだけで、queue/visited は呼び出しをまたぐ意味を持たない。
 */
struct NavAStarScratch {
  std::vector<NavAStarOpenNode> openHeap;
  std::vector<int> gScore;
  std::vector<int> cameFrom;
  std::vector<uint8_t> closed;
  std::vector<NavCell> reversePath;
  std::vector<NavCell> pathCells;
  NavNearestWalkableScratch nearestWalkable;
};

/**
 * @brief 呼び出し側が用意した scratch を使って A* で経路探索する
 *
 * @param grid 静的な nav グリッド。blocked/areaIds の span が width*height 分を満たしていること。
 * @param start 開始セル。通行不可なら最も近い通行可能セルに補正する。
 * @param goal 目標セル。通行不可なら最も近い通行可能セルに補正する。
 * @param scratch 呼び出し側が所有する一時メモリプール。結果の cells はその中の pathCells を借用する。
 *
 * @return found=true: cells は補正後の start から補正後の goal までの 4 近傍経路。
 * @return found=false: cells は空。呼び出し側は「現在は到達可能な経路がない」とみなすこと。
 *
 * @note
 * - 同じ scratch で次の探索を行った後に、古い result.cells を読まないこと。
 */
NavPathResult findPathAStar(const NavGridStatic &grid, NavCell start, NavCell goal,
                            NavAStarScratch &scratch);

} // namespace game
