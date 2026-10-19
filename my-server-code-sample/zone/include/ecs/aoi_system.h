#pragma once
#include <entt/entt.hpp>
#include <vector>

struct ZoneRegistry; // 前方宣言

namespace aoi_system {

/**
 * @brief AOI のグリッドインデックスを再構築する
 *
 * @param zone メイン world の registry ラッパー。内部の AOI グリッドをクリアして作り直す。
 *
 * @note
 * - 毎フレーム、AOI の検索より前に 1 回呼ぶ。
 * - 再構築後はセル内 vector の要素順が変わるため、インデックスを保存しないこと。
 */
void rebuild(ZoneRegistry &zone);

/**
 * @brief エンティティの周囲で、AOI 離脱半径がかかるセルにいる候補エンティティを列挙する
 *
 * @param zone メイン world の registry ラッパー。AOI グリッドは読み取りのみ。
 * @param entity 検索の中心となるエンティティ。位置コンポーネントを持っていること。
 * @param out 呼び出し側が所有する出力先。クリアしてから候補を書き込む。
 *
 * @return out に書き込んだ候補の数。
 *
 * @note
 * - 走査範囲は AOI_LEAVE_RADIUS / CELL_SIZE から求める（周囲 9 マス固定ではない）。
 * - セル単位の候補を返すだけで、進入／離脱半径による絞り込みは行わない。
 */
size_t query_neighbors(ZoneRegistry &zone, entt::entity entity,
                       std::vector<entt::entity> &out);
} // namespace aoi_system
