#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace game {

/**
 * @brief nav グリッド上の整数セル座標
 *
 * @note
 * - 値型であり、リソースは所有しない。
 * - x/y の範囲チェックは呼び出し側または nav_grid の API で行う。
 */
struct NavCell {
  int x = 0;
  int y = 0;
};

/**
 * @brief 静的な nav グリッドのビュー
 *
 * @note
 * - blocked と areaIds は借用した span で、生成済みデータや外部グリッドのメモリは所有しない。
 * - blocked/areaIds を読む前に、span が width*height 分を満たしていることを保証すること。
 * - areaAnchorCells は業務上の areaId を添字に使う。0 番は無効エリア用に予約している。
 * - width/height/cellSize が 0 以下のときは無効なグリッドを表す。
 */
struct NavGridStatic {
  int cellSize = 0;
  int minX = 0;
  int minY = 0;
  int width = 0;
  int height = 0;
  std::span<const uint8_t> blocked;
  std::span<const uint8_t> areaIds;
  NavCell healingStationCell{};
  NavCell playerSpawnCell{};
  std::array<NavCell, 5> areaAnchorCells{};
};

} // namespace game
