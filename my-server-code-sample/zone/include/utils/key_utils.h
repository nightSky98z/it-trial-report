#pragma once
#include <cstdint>
#include <string>

namespace key_utils {

/**
 * @brief マップ単位のスコープキーを作る
 *
 * @param map_id マップ ID。
 *
 * @return 現状は map_id そのもの。将来インスタンスを分割するときのために関数として残している。
 */
inline uint32_t make_scope_key(uint32_t map_id) {
  return map_id;
}

/**
 * @brief モンスターインスタンスの一意キーを作る
 *
 * @param map_id マップ ID。
 * @param inst_id 設定上または実行時のモンスターインスタンス ID。借用して戻り値にコピーする。
 *
 * @return "map_id:inst_id" 形式の文字列。
 */
inline std::string make_monster_key(uint32_t map_id,
                                    const std::string &inst_id) {
  return std::to_string(map_id) + ":" + inst_id;
}

/**
 * @brief エンティティのタグが検知マスクに該当するかを判定する
 *
 * @param tags エンティティのタグ（ビット集合）。
 * @param detector_mask 検知側が受け付けるタグ（ビット集合）。
 *
 * @retval true 2 つのビット集合に共通するビットが 1 つ以上ある。
 */
inline bool has_mask(uint32_t tags, uint32_t detector_mask) {
  return (tags & detector_mask) != 0;
}

} // namespace key_utils
