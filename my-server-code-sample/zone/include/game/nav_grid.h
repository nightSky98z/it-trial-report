#pragma once

#include "ecs/map_components.h"
#include "game/nav_types.h"

#include <cstdint>
#include <vector>

namespace game {

/**
 * @brief 最寄りの通行可能セル探索で使う一時メモリの所有者
 *
 * @note
 * - 呼び出し側が所有する。同じインスタンスをスレッド間で共有しないこと。
 * - findNearestWalkableCell は visited/queue の capacity を再利用する。
 * - queue/visited は関数の呼び出し中だけ意味を持つ。呼び出し側はその中身に意味を求めないこと。
 */
struct NavNearestWalkableScratch {
  std::vector<uint8_t> visited;
  std::vector<NavCell> queue;
};

/**
 * @brief エリア内ランダム地点選択で使う一時メモリの所有者
 *
 * @note
 * - 呼び出し側が所有する。同じインスタンスをスレッド間で共有しないこと。
 * - pickRandomWalkableCellInArea は、メインシティ以外のグリッドのときだけ cells の capacity を再利用する。
 * - cells は関数の呼び出し中だけ候補を保持する。呼び出し側はその中身に意味を求めないこと。
 */
struct NavAreaPickScratch {
  std::vector<NavCell> cells;
};

/**
 * @brief メインシティの静的な nav グリッドを返す
 *
 * @return プロセス内の静的グリッドへの参照。blocked/areaIds は生成済みの静的配列を指す。
 *
 * @note
 * - 戻り値は読み取り専用で、span が指すメモリは所有しない。
 */
const NavGridStatic &mainCityNavGrid();

/**
 * @brief セルがグリッドの矩形内にあるかを判定する
 *
 * @param grid 静的な nav グリッド。
 * @param cell 判定するセル。
 *
 * @retval true セル座標がグリッドの矩形内にあり、行優先のインデックス計算に使える。
 * @retval false セルが範囲外。
 *
 * @note
 * - blocked/areaIds を読む前には、対応する span が width*height 分を満たしているかも別途確認すること。
 */
bool navIsInside(const NavGridStatic &grid, NavCell cell);

/**
 * @brief セルが通行可能かを判定する
 *
 * @param grid 静的な nav グリッド。blocked の span が width*height 分を満たす場合のみ読む。
 * @param cell 判定するセル。
 *
 * @retval true セルがグリッド内にあり、blocked が 0。
 * @retval false セルが範囲外、blocked のデータがない、またはそのセルが通行不可。
 */
bool navIsWalkable(const NavGridStatic &grid, NavCell cell);

/**
 * @brief セルに対応するエリア ID を取得する
 *
 * @param grid 静的な nav グリッド。areaIds の span が width*height 分を満たす場合のみ読む。
 * @param cell 対象のセル。
 *
 * @return エリア ID。0 は範囲外・データなし・無効なエリアのいずれか。
 */
uint32_t navAreaIdAt(const NavGridStatic &grid, NavCell cell);

/**
 * @brief ワールド座標を nav セルに変換する
 *
 * @param grid 静的な nav グリッド。
 * @param x ワールドの X 座標。
 * @param y ワールドの Y 座標。
 *
 * @return 対応するセル。cellSize が不正、または座標が有限でない場合は {-1, -1}。
 */
NavCell navWorldToCell(const NavGridStatic &grid, float x, float y);

/**
 * @brief セルの中心点のワールド座標を返す
 *
 * @param grid 静的な nav グリッド。
 * @param cell 変換するセル。グリッド内かどうかはチェックしない。
 *
 * @return セル中心に対応するワールド座標の Position。
 *
 * @note
 * - セルが正しい nav データから得たものであることは呼び出し側が保証すること。
 */
Position navCellCenterToWorld(const NavGridStatic &grid, NavCell cell);
/**
 * @brief desired から 4 近傍の BFS で最寄りの通行可能セルを探す
 *
 * @param grid 静的な nav グリッド。blocked の span がなくても範囲外を読むことはない。
 * @param desired 希望するセル。グリッド外でもよく、最初にグリッドの境界内へ丸める。
 * @param scratch 呼び出し側が所有する一時メモリプール。関数は capacity を再利用するだけ。
 *
 * @return desired が通行可能ならそのまま返す。
 * @return 通行可能なセルが見つかれば最も近い候補を返す。どこも通行できなければ playerSpawnCell を返す。
 * @return グリッドのサイズが無効なら、元の desired を返す。
 *
 * @note
 * - 同じ scratch をスレッド間で共有しないこと。
 * - 呼び出し側は scratch.queue / scratch.visited の中身に意味を求めないこと。
 */
NavCell findNearestWalkableCell(const NavGridStatic &grid, NavCell desired,
                                NavNearestWalkableScratch &scratch);

/**
 * @brief 指定エリアから通行可能なセルをランダムに 1 つ選ぶ
 *
 * @param grid 静的な nav グリッド。メインシティのグリッドは事前構築したキャッシュを使い、
 *             それ以外は scratch に走査結果を集める。
 * @param areaId 業務上のエリア ID。0 は無効。
 * @param scratch 呼び出し側が所有する、候補集合用の一時メモリプール。関数は capacity を再利用するだけ。
 *
 * @return エリア内のランダムな通行可能セル。
 * @return areaId が無効、静的データがない、またはエリア内に通行可能セルがない場合は playerSpawnCell。
 *
 * @note
 * - 同じ scratch をスレッド間で共有しないこと。
 * - 呼び出し側は scratch.cells の中身に意味を求めないこと。
 */
NavCell pickRandomWalkableCellInArea(const NavGridStatic &grid, uint32_t areaId,
                                    NavAreaPickScratch &scratch);

} // namespace game
