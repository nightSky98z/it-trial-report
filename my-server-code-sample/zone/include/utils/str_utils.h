#pragma once
#include "utils/log.h"
#include <cstring>
#include <cstdlib>

namespace utils {

/**
 * @brief 設定値の文字列が存在し、空でないことを確認する
 *
 * @param value 借用する C 文字列。nullptr でもよい。
 * @param key ログ出力用の設定項目名。有効な C 文字列であること。
 *
 * @retval true value が空でない。
 * @retval false value が空または nullptr。error ログを出力する。
 */
inline bool require_non_empty(const char* value, const char *key) {
  const bool has_value = (value != nullptr && value[0] != '\0');
  if (has_value) {
    return true;
  }
  zone_log::error("missing required config: %s", key);
  return false;
}

/**
 * @brief C 文字列を固定長配列へ安全にコピーする
 *
 * @param dst コピー先の固定長配列。NUL 終端の文字列を書き込む。
 * @param src コピー元の C 文字列。key を指定した場合は空であってはならない。
 * @param key 設定項目名。指定すると、値の欠落や長すぎる場合のログに使う。
 *
 * @retval true NUL 終端まで含めて完全にコピーできた。
 * @retval false 値がない、または dst の容量を超えている。dst の内容は意味を持たない。
 *
 * @note
 * - key を省略する場合、呼び出し側が src が nullptr でないことを保証すること。
 */
template <size_t N>
inline bool copy_checked(char (&dst)[N], const char* src, const char *key = nullptr) {
  if (key != nullptr) {
    if (!require_non_empty(src, key)) return false;
  }

  const size_t len = std::strlen(src);
  if (len >= N) {
    zone_log::error("config value too long: %s (len=%zu, max=%zu)",
                    key, len, N - 1);
    return false;
  }

  std::memcpy(dst, src, len + 1); // 終端の '\0' も含めてコピー
  return true;
}

} // namespace utils
