#pragma once
#include "game/building_cache.h"
#include "game/city_nav_grid.h"
#include "game/nav_grid.h"
#include "game/role_buildings.h"
#include <cstdint>
#include <vector>

/**
 * @file building_placement.h
 * @brief プレイヤーが建物の位置を変更するときの、サーバー側の妥当性チェック
 *
 * 配置可能なエリアは nav グリッドの areaIds で決まる固定データ（現在はエリア 1 = メインシティ）で、
 * 別の設定テーブルは不要。プレイヤーはそのエリア内で自由に配置できるが、
 * マップの使い勝手を壊すような位置はサーバー側で必ず弾く必要がある。
 *
 * @note
 * - チェックは読み取りのみで、状態は一切変更しない。通過した後に、呼び出し側が DB に保存し、そのシティの nav グリッドを作り直す。
 * - 最も重要なのは連結性のチェック。道をふさぐと精霊がその場から永久に動けなくなり、
 *   普通の AI のバグと見分けがつかないため、配置の時点で必ず拒否する。
 */
namespace buildings {

/**
 * @brief 配置に失敗した理由
 *
 * @note
 * - 呼び出し側はこれをもとにプレイヤー向けの文言を作る。このファイルで UI のテキストを組み立てないこと。
 */
enum class PlacementError : uint8_t {
  None = 0,
  UnknownBuilding,        // 建物テンプレートが存在しない
  OutsideGrid,            // 中心点が nav グリッドの外にある
  OutsideAllowedArea,     // 形状が配置可能なエリアからはみ出している
  OnBlockedTerrain,       // 地形の障害物と重なっている
  OverlapsAnotherBuilding,// 他の建物と重なっている
  WouldDisconnectMap      // マップを分断してしまう（出現地点から到達できない場所ができる）
};

/**
 * @brief 建物の形状が覆うセルを計算する
 *
 * @param grid 基準となるグリッド（幾何パラメータだけを使う）。
 * @param x 建物の中心のワールド X 座標。
 * @param y 建物の中心のワールド Y 座標。
 * @param radius 当たり判定の半径。
 * @param[out] out 覆われるセル。最初にクリアする。
 *
 * @note
 * - CityNavGrid::stampCircle と同じ判定（セルの中心が円内にあれば覆われているとみなす）を使う。
 *   2 か所で一致していないと、「チェックは通ったのに、重ねてみたら別のセルにかかっていた」ということになる。
 */
inline void footprintCells(const game::NavGridStatic &grid, float x, float y,
                           float radius, std::vector<game::NavCell> &out) {
  out.clear();
  if (radius <= 0.f) return;

  const game::NavCell center = game::navWorldToCell(grid, x, y);
  const int span = static_cast<int>(radius / static_cast<float>(grid.cellSize)) + 1;
  const float r2 = radius * radius;

  for (int dy = -span; dy <= span; ++dy) {
    for (int dx = -span; dx <= span; ++dx) {
      const game::NavCell c{center.x + dx, center.y + dy};
      if (!game::navIsInside(grid, c)) continue;
      const Position p = game::navCellCenterToWorld(grid, c);
      const float ox = p.x - x;
      const float oy = p.y - y;
      if (ox * ox + oy * oy > r2) continue;
      out.push_back(c);
    }
  }
}

/**
 * @brief 出現地点から塗りつぶし（flood fill）を行い、建物によってマップが分断されていないかを確認する
 *
 * @param grid 建物を重ねたシティのグリッド。
 *
 * @retval false 出現地点から到達できない通行可能セルがある、またはいずれかのエリアのアンカーに到達できない。
 *
 * @note
 * - 2 種類の問題を同時に防ぐ。エリアがまるごと閉じ込められること、そしてエリア内に孤立したくぼみができること
 *   （ユニットが一度入ると二度と出てこられない）。
 */
inline bool isFullyReachable(const game::NavGridStatic &grid) {
  const std::size_t cellCount =
      static_cast<std::size_t>(grid.width) * static_cast<std::size_t>(grid.height);
  if (cellCount == 0) return false;

  std::vector<uint8_t> seen(cellCount, 0);
  std::vector<game::NavCell> stack;
  stack.push_back(grid.playerSpawnCell);

  const auto indexOf = [&grid](game::NavCell c) {
    return static_cast<std::size_t>(c.y) * static_cast<std::size_t>(grid.width) +
           static_cast<std::size_t>(c.x);
  };

  if (!game::navIsWalkable(grid, grid.playerSpawnCell)) return false;
  seen[indexOf(grid.playerSpawnCell)] = 1;

  constexpr game::NavCell kNeighbors[4] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
  while (!stack.empty()) {
    const game::NavCell cur = stack.back();
    stack.pop_back();
    for (const game::NavCell d : kNeighbors) {
      const game::NavCell nx{cur.x + d.x, cur.y + d.y};
      if (!game::navIsInside(grid, nx) || !game::navIsWalkable(grid, nx)) continue;
      const std::size_t i = indexOf(nx);
      if (seen[i]) continue;
      seen[i] = 1;
      stack.push_back(nx);
    }
  }

    // 通行可能なセルはすべて到達済みでなければならない。そうでなければ閉じたくぼみができている。
  for (int y = 0; y < grid.height; ++y) {
    for (int x = 0; x < grid.width; ++x) {
      const game::NavCell c{x, y};
      if (!game::navIsWalkable(grid, c)) continue;
      if (!seen[indexOf(c)]) return false;
    }
  }

    // 各エリアのアンカーに到達できなければならない。精霊の割り当て先はすべてアンカーだから。
  for (std::size_t area = 1; area < grid.areaAnchorCells.size(); ++area) {
    const game::NavCell anchor = grid.areaAnchorCells[area];
    if (!game::navIsInside(grid, anchor)) continue;
    if (!game::navIsWalkable(grid, anchor) || !seen[indexOf(anchor)]) return false;
  }
  return true;
}

/**
 * @brief 建物を新しい位置へ移動してよいかを検証する
 *
 * @param terrain 地形グリッド（建物を含まない）。
 * @param tpls 建物テンプレートのキャッシュ。当たり判定の半径を提供する。
 * @param current そのシティの現在の配置。読み取りのみ。
 * @param buildingId 移動する建物。
 * @param x 移動先の中心のワールド X 座標。
 * @param y 移動先の中心のワールド Y 座標。
 * @param allowedAreaId 配置を許可するエリア（現在はメインシティ=1）。
 * @param blockLocked 未解放の建物も場所を占有するかどうか。実行時の重ね方の方針と一致させること。
 *
 * @return PlacementError::None なら配置できる。
 *
 * @note
 * - 検証の順序：テンプレートが存在する → グリッド内にある → 許可エリア内にある → 地形と重ならない
 *   → 他の建物と重ならない → マップを分断しない。前半のチェックは軽く、連結性が最も重いので最後に置く。
 * - 通過した後、呼び出し側はそのシティの nav グリッドを作り直すこと。そうしないと経路探索は古い配置のまま動く。
 */
inline PlacementError validatePlacement(const game::NavGridStatic &terrain,
                                        const cache::BuildingCache &tpls,
                                        const RoleBuildings &current,
                                        uint32_t buildingId, float x, float y,
                                        uint32_t allowedAreaId,
                                        bool blockLocked) {
  const cache::BuildingTemplate *const tpl = cache::findBuilding(tpls, buildingId);
  if (!tpl) return PlacementError::UnknownBuilding;

  const game::NavCell center = game::navWorldToCell(terrain, x, y);
  if (!game::navIsInside(terrain, center)) return PlacementError::OutsideGrid;

  std::vector<game::NavCell> cells;
  footprintCells(terrain, x, y, tpl->colliderRadius, cells);
  if (cells.empty()) return PlacementError::OutsideGrid;

  for (const game::NavCell c : cells) {
    if (game::navAreaIdAt(terrain, c) != allowedAreaId) {
      return PlacementError::OutsideAllowedArea;
    }
    if (!game::navIsWalkable(terrain, c)) return PlacementError::OnBlockedTerrain;
  }

    // 他の建物との重なり：1 つずつ形状を計算して比較する。建物は 1 桁しかないので、総当たりで十分。
  std::vector<game::NavCell> other;
  for (const auto &e : current.entries) {
    if (e.buildingId == buildingId) continue;  // 自分自身とは比較しない
    const cache::BuildingTemplate *const otherTpl =
        cache::findBuilding(tpls, e.buildingId);
    if (!otherTpl) continue;
    if (!blockLocked && e.state != kStateUnlocked) continue;

    footprintCells(terrain, e.x, e.y, otherTpl->colliderRadius, other);
    for (const game::NavCell a : cells) {
      for (const game::NavCell b : other) {
        if (a.x == b.x && a.y == b.y) {
          return PlacementError::OverlapsAnotherBuilding;
        }
      }
    }
  }

    // 最後に連結性を確認する：新しい配置でグリッドを試算し、道をふさいでいないことを確かめる。
  RoleBuildings trial = current;
  bool found = false;
  for (auto &e : trial.entries) {
    if (e.buildingId != buildingId) continue;
    e.x = x;
    e.y = y;
    found = true;
    break;
  }
  if (!found) {
    RoleBuilding e;
    e.buildingId = buildingId;
    e.x = x;
    e.y = y;
    e.state = kStateUnlocked;
    trial.entries.push_back(e);
  }

  game::CityNavGrid trialGrid;
  trialGrid.build(terrain, tpls, trial, blockLocked);
  if (!isFullyReachable(trialGrid.view())) {
    return PlacementError::WouldDisconnectMap;
  }

  return PlacementError::None;
}

} // namespace buildings
