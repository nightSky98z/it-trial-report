#pragma once

#include "game/nav_types.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <entt/entt.hpp>
#include <limits>
#include <span>
#include <vector>

namespace game {

/**
 * @brief nav 経路プール内の連続したセル区間
 *
 * @note
 * - offset/count は、所属する NavPathStore が生きている間だけ有効。
 * - count == 0 は経路なしを表す。その場合、呼び出し側は offset を読まないこと。
 * - CNavAgent は自分の区間を所有する。再計算やクリアの前に必ずプールへ返却すること。
 */
struct NavPathRange {
  uint32_t offset = 0;
  uint32_t count = 0;
  uint32_t generation = 0;
};

/**
 * @brief 1 つの world のナビゲーション経路ストレージ
 *
 * @note
 * - entt::registry::ctx() に置かれ、寿命は対応するマップの world に従う。
 * - allocate/release は、まだ生きている経路スライスを移動しない。
 * - rangeCounts/rangeGenerations は区間の先頭にだけ書き込む。これにより経路ハンドルの検証は O(1) で済む。
 * - freeRanges には解放済みの区間だけを記録する。liveCellCount はテストと実行時のリーク調査に使う。
 */
struct NavPathStore {
  std::vector<NavCell> cells;
  std::vector<uint32_t> rangeCounts;
  std::vector<uint32_t> rangeGenerations;
  std::vector<NavPathRange> freeRanges;
  uint32_t liveCellCount = 0;
  uint32_t nextGeneration = 1;

    /**
     * @brief 連続した経路セルの区間を確保する
     *
     * @param count 書き込むセルの数。1 以上であること。
     *
     * @return 書き込み可能なプール上の区間。
     *
     * @note
     * - 返した区間は、不要になったら呼び出し側が release() すること。
     * - cells が拡張されることがあるので、呼び出しをまたいで cells 内部へのポインタを保持しないこと。
     */
  NavPathRange allocate(uint32_t count) {
    assert(count > 0);
    for (auto it = freeRanges.begin(); it != freeRanges.end(); ++it) {
      if (it->count < count) continue;

      const NavPathRange range{
          .offset = it->offset,
          .count = count,
          .generation = allocGeneration(),
      };
      if (it->count == count) {
        freeRanges.erase(it);
      } else {
        it->offset += count;
        it->count -= count;
      }
      liveCellCount += count;
      markActiveRange(range);
      return range;
    }

    assert(cells.size() <= static_cast<std::size_t>(
                               std::numeric_limits<uint32_t>::max() - count));
    const auto offset = static_cast<uint32_t>(cells.size());
    cells.resize(cells.size() + count);
    rangeCounts.resize(cells.size());
    rangeGenerations.resize(cells.size());
    liveCellCount += count;
    const NavPathRange range{
        .offset = offset,
        .count = count,
        .generation = allocGeneration(),
    };
    markActiveRange(range);
    return range;
  }

    /**
     * @brief 所有している経路セルの区間を解放する
     *
     * @param range 以前 allocate() が返し、まだ解放していない区間。
     *
     * @note
     * - 呼び出し後、古い range を読まないこと。
     * - 同じ区間を二重に解放するのは、呼び出し側の寿命管理のミスである。
     */
  bool release(NavPathRange range) {
    if (range.count == 0) return true;
    if (range.count > liveCellCount) {
      return false;
    }
    if (!containsActiveRange(range)) return false;

    clearActiveRange(range);
    liveCellCount -= range.count;
    range.generation = 0;
    freeRanges.push_back(range);
    coalesceFreeRanges();
    return true;
  }

    /**
     * @brief 経路ハンドルが、現在のプール上でまだ生きていると記録されているかを確認する
     *
     * @param range CNavAgent が保持している経路スライスのメタデータ。
     *
     * @retval true range は現在アクティブな所有者のもの。
     * @retval false range は確保されたことがない、解放済み、generation が一致しない、または範囲外。
     */
  bool containsActiveRange(NavPathRange range) const {
    if (range.count == 0 || range.generation == 0) return false;
    const std::size_t offset = range.offset;
    const std::size_t count = range.count;
    return offset < rangeCounts.size() && offset <= cells.size() &&
           count <= cells.size() - offset &&
           rangeCounts[offset] == range.count &&
           rangeGenerations[offset] == range.generation;
  }

private:
  uint32_t allocGeneration() {
    const uint32_t generation = nextGeneration++;
    if (nextGeneration == 0) {
      nextGeneration = 1;
    }
    return generation;
  }

  void markActiveRange(NavPathRange range) {
    assert(range.offset < rangeCounts.size());
    rangeCounts[range.offset] = range.count;
    rangeGenerations[range.offset] = range.generation;
  }

  void clearActiveRange(NavPathRange range) {
    assert(range.offset < rangeCounts.size());
    rangeCounts[range.offset] = 0;
    rangeGenerations[range.offset] = 0;
  }

  void coalesceFreeRanges() {
    std::sort(freeRanges.begin(), freeRanges.end(),
              [](const NavPathRange &a, const NavPathRange &b) {
                return a.offset < b.offset;
              });

    std::size_t write = 0;
    for (std::size_t read = 0; read < freeRanges.size(); ++read) {
      if (freeRanges[read].count == 0) continue;
      if (write == 0) {
        freeRanges[write++] = freeRanges[read];
        continue;
      }

      auto &last = freeRanges[write - 1];
      const std::size_t lastEnd =
          static_cast<std::size_t>(last.offset) + last.count;
      if (lastEnd >= freeRanges[read].offset) {
        const std::size_t readEnd =
            static_cast<std::size_t>(freeRanges[read].offset) +
            freeRanges[read].count;
        if (readEnd > lastEnd) {
          assert(readEnd - last.offset <=
                 std::numeric_limits<uint32_t>::max());
          last.count = static_cast<uint32_t>(readEnd - last.offset);
        }
      } else {
        freeRanges[write++] = freeRanges[read];
      }
    }
    freeRanges.resize(write);
  }
};

/**
 * @brief ナビゲーションエージェントの実行時状態
 *
 * @note
 * - pathOffset/pathCount は NavPathStore を借用するハンドルで、cells の vector は所有しない。
 * - pathCount > 0 のとき、このエージェントは対応するプールのスライスを所有している。クリアや再計算の前に必ず解放すること。
 * - pathGeneration は、解放後に同じ offset が再利用されたとき、古いハンドルで誤って読んだり解放したりするのを防ぐ。
 * - nextIndex は経路スライス内のインデックスで、プール全体のインデックスではない。
 */
struct CNavAgent {
  bool active = false;
  bool pathDirty = false;
  bool arrived = false;
  NavCell goalCell{};
  uint32_t nextIndex = 0;
  uint32_t pathOffset = 0;
  uint32_t pathCount = 0;
  uint32_t pathGeneration = 0;
  uint64_t lastRepathAtMs = 0;
  uint64_t repathCooldownMs = 200;
};

/**
 * @brief world のナビゲーション経路プールを返す
 *
 * @param world マップの ECS world。プールの寿命はこの world に従う。
 *
 * @return 書き込み可能なプール所有者への参照。
 *
 * @note
 * - world.ctx() に NavPathStore がなければ、その場で作成する（遅延生成）。
 */
inline NavPathStore &navPathStore(entt::registry &world) {
  if (!world.ctx().contains<NavPathStore>()) {
    world.ctx().emplace<NavPathStore>();
  }
  return world.ctx().get<NavPathStore>();
}

/**
 * @brief world のナビゲーション経路プールの取得を試みる
 *
 * @param world マップの ECS world。
 *
 * @retval nullptr この world ではまだナビゲーション経路を一度も確保していない。
 */
inline NavPathStore *navTryPathStore(entt::registry &world) {
  return world.ctx().find<NavPathStore>();
}

/**
 * @brief エージェントが現在経路スライスを所有しているかを判定する
 *
 * @param nav 判定するエージェント。
 *
 * @retval true エージェントが空でない経路ハンドルを記録している。
 * @retval false エージェントは現在経路を持っていない。
 *
 * @note
 * - 実際に読めるかどうかは、navPathRangeValid または navTryPathCell で改めて確認すること。
 */
inline bool navHasPath(const CNavAgent &nav) {
  return nav.pathCount > 0;
}

/**
 * @brief エージェントの経路スライスが、ストアの現在のセルバッファ内に収まっているかを検証する
 *
 * @param store その world の経路プール。
 * @param nav 経路スライスのメタデータを持つエージェント。
 *
 * @retval true 経路がない、または経路の区間が cells の範囲内にある。
 * @retval false ハンドルが範囲外。呼び出し側は解放して経路なしの状態にすること。
 */
inline bool navPathRangeValid(const NavPathStore &store, const CNavAgent &nav) {
  if (nav.pathCount == 0) return nav.pathOffset == 0 && nav.pathGeneration == 0;
  const std::size_t offset = nav.pathOffset;
  const std::size_t count = nav.pathCount;
  return offset <= store.cells.size() && count <= store.cells.size() - offset &&
         store.containsActiveRange(NavPathRange{
             .offset = nav.pathOffset,
             .count = nav.pathCount,
             .generation = nav.pathGeneration,
         });
}

/**
 * @brief エージェントが持つ経路ハンドルだけを破棄する
 *
 * @param nav 無効な経路スライスのメタデータを持つエージェント。
 *
 * @note
 * - ハンドルが無効と確認済みで、安全に release できない場合の防御用途に限る。
 * - 通常のライフサイクルでは、必ず navClearPath() でプールのスライスを返却すること。
 */
inline void navDiscardInvalidPath(CNavAgent &nav) {
  nav.pathOffset = 0;
  nav.pathCount = 0;
  nav.pathGeneration = 0;
  nav.nextIndex = 0;
}

/**
 * @brief エージェントが現在所有している経路スライスを解放する
 *
 * @param world NavPathStore を持つ ECS world。
 * @param nav 経路スライスを所有するエージェント。
 *
 * @note
 * - 経路の解放と経路インデックスのリセットだけを行い、active/pathDirty/arrived は変更しない。
 * - 呼び出し後、古いスライスを読まないこと。
 */
inline void navClearPath(entt::registry &world, CNavAgent &nav) {
  if (nav.pathCount > 0) {
    auto *store = navTryPathStore(world);
    const bool canRelease = store && navPathRangeValid(*store, nav);
    if (canRelease) {
      (void)store->release(NavPathRange{
          .offset = nav.pathOffset,
          .count = nav.pathCount,
          .generation = nav.pathGeneration,
      });
    }
  }
  navDiscardInvalidPath(nav);
}

/**
 * @brief エンティティが持つナビゲーション経路を解放する
 *
 * @param world エンティティと NavPathStore を持つ ECS world。
 * @param entity CNavAgent を持っている可能性があるエンティティ。
 *
 * @note
 * - world.destroy(entity) の前に呼ぶこと。エンティティの破棄によってプールの release が素通りされるのを防ぐ。
 * - ナビゲーション経路だけを扱い、エンティティの他のコンポーネントは変更しない。
 */
inline void navClearEntityPath(entt::registry &world, entt::entity entity) {
  if (auto *nav = world.try_get<CNavAgent>(entity)) {
    navClearPath(world, *nav);
  }
}

/**
 * @brief world 内のすべてのナビゲーション経路を解放する
 *
 * @param world これから clear する、またはエンティティを一括破棄する ECS world。
 *
 * @note
 * - entt::registry::clear() の前に呼ぶこと。コンポーネントの破棄によってプールの release が素通りされるのを防ぐ。
 * - 経路スライスを解放するだけで、エンティティの破棄や active/pathDirty/arrived の変更は行わない。
 */
inline void navClearAllEntityPaths(entt::registry &world) {
  auto view = world.view<CNavAgent>();
  for (auto entity : view) {
    navClearPath(world, view.get<CNavAgent>(entity));
  }
}

/**
 * @brief world のナビゲーション経路プールをリセットする
 *
 * @param world NavPathStore を持つ ECS world。
 *
 * @note
 * - すべての CNavAgent の経路が解放済みの場合か、world 全体を破棄・再構築する直前にだけ呼ぶこと。
 * - 呼び出し後、古いハンドルはすべて無効になる。まだ生きているエージェントでは使わないこと。
 */
inline void navResetPathStore(entt::registry &world) {
  auto *store = navTryPathStore(world);
  if (!store) return;

  std::vector<NavCell>{}.swap(store->cells);
  std::vector<uint32_t>{}.swap(store->rangeCounts);
  std::vector<uint32_t>{}.swap(store->rangeGenerations);
  std::vector<NavPathRange>{}.swap(store->freeRanges);
  store->liveCellCount = 0;
  store->nextGeneration = 1;
}

/**
 * @brief 新しい経路を、エージェントが所有するプールのスライスに書き込む
 *
 * @param world NavPathStore を持つ ECS world。
 * @param nav 新しい経路スライスを所有することになるエージェント。
 * @param pathCells 入力経路（借用）。関数から戻った後はこの借用を保持しない。
 *
 * @note
 * - 先に nav の古い経路を解放する。
 * - active/pathDirty の管理は上位のステートマシンが担当する。
 */
inline void navAssignPath(entt::registry &world, CNavAgent &nav,
                          std::span<const NavCell> pathCells) {
  navClearPath(world, nav);

  assert(pathCells.size() <=
         static_cast<std::size_t>(std::numeric_limits<uint32_t>::max()));
  const auto count = static_cast<uint32_t>(pathCells.size());
  if (count == 0) {
    nav.arrived = true;
    return;
  }

  auto &store = navPathStore(world);
  const NavPathRange range = store.allocate(count);
  for (uint32_t i = 0; i < count; ++i) {
    store.cells[range.offset + i] = pathCells[i];
  }

  nav.pathOffset = range.offset;
  nav.pathCount = count;
  nav.pathGeneration = range.generation;
  nav.nextIndex = count > 1 ? 1u : 0u;
  nav.arrived = count <= 1;
}

/**
 * @brief エージェントの経路スライスからセルを 1 つ借用してみる
 *
 * @param world NavPathStore を持つ ECS world。
 * @param nav 経路スライスのメタデータを持つエージェント。
 * @param index 経路スライス内のインデックス。
 *
 * @return セルへのポインタ。nullptr はハンドルが無効、または index が範囲外であることを表す。
 *
 * @note
 * - 返したポインタは今回の読み取りの中でだけ使うこと。プールの allocate/release をまたいで保持しないこと。
 */
inline const NavCell *navTryPathCell(entt::registry &world, const CNavAgent &nav,
                                     uint32_t index) {
  auto *store = navTryPathStore(world);
  if (!store) return nullptr;
  if (!navPathRangeValid(*store, nav)) return nullptr;
  if (index >= nav.pathCount) return nullptr;
  return &store->cells[nav.pathOffset + index];
}

/**
 * @brief エージェントの経路スライスからセルを 1 つ読み取る
 *
 * @param world NavPathStore を持つ ECS world。
 * @param nav 有効な経路スライスを持つエージェント。
 * @param index 経路スライス内のインデックス。pathCount より小さいこと。
 *
 * @return プール上のセルへの読み取り専用の参照。
 *
 * @note
 * - ハンドルが有効と確認済みの箇所でだけ使うこと。システムのホットパスでは navTryPathCell() を優先する。
 */
inline const NavCell &navPathCell(entt::registry &world, const CNavAgent &nav,
                                  uint32_t index) {
  const auto *cell = navTryPathCell(world, nav, index);
  assert(cell != nullptr);
  return *cell;
}

} // namespace game
