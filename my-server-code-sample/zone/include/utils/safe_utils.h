#pragma once
#include "db/mysql_client.h"
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

namespace utils {

namespace detail {

/**
 * @brief 符号なし 10 進数のテキストを最後まで解析する
 *
 * @param text MySQL の文字列値のコピー（所有権あり）。
 * @param out 成功時に解析した値を書き込む。
 *
 * @retval true 文字列全体が符号なし 10 進数で、uint64_t をオーバーフローしない。
 * @retval false 空、符号・空白・数字以外を含む、または uint64_t の範囲外。
 */
inline bool parseFullU64(const std::string &text, uint64_t &out) {
  if (text.empty()) return false;
  uint64_t parsed = 0;
  for (const char ch : text) {
    const auto uch = static_cast<unsigned char>(ch);
    if (!std::isdigit(uch)) return false;
    const uint64_t digit =
        static_cast<uint64_t>(uch - static_cast<unsigned char>('0'));
    if (parsed > (std::numeric_limits<uint64_t>::max() - digit) / 10u) {
      return false;
    }
    parsed = parsed * 10u + digit;
  }
  out = parsed;
  return true;
}

/**
 * @brief 有限の float のテキストを最後まで解析する
 *
 * @param text MySQL の文字列値のコピー（所有権あり）。
 * @param out 成功時に解析した値を書き込む。
 *
 * @retval true 文字列全体を有限の float として解析できる。
 * @retval false 空、解析されずに残る末尾がある、NaN/Inf、または float の範囲外。
 */
inline bool parseFullF32(const std::string &text, float &out) {
  if (text.empty()) return false;
  try {
    size_t idx = 0;
    const double parsed = std::stod(text, &idx);
    if (idx != text.size()) return false;
    if (!std::isfinite(parsed)) return false;
    if (parsed > std::numeric_limits<float>::max() ||
        parsed < -std::numeric_limits<float>::max()) {
      return false;
    }
    out = static_cast<float>(parsed);
    return true;
  } catch (...) {
    return false;
  }
}

/**
 * @brief 符号付き 10 進数のテキストを最後まで解析する
 *
 * @param text MySQL のテキスト結果のコピー（所有権あり）。
 * @param out 成功時に解析した値を書き込む。
 *
 * @retval true 文字列全体が「省略可能な符号 + 10 進数字」で、int64_t をオーバーフローしない。
 * @retval false 空、空白や数字以外を含む、または int64_t の範囲外。
 */
inline bool parseFullI64(const std::string &text, int64_t &out) {
  if (text.empty()) return false;
  std::size_t i = 0;
  bool negative = false;
  if (text[0] == '+' || text[0] == '-') {
    negative = (text[0] == '-');
    i = 1;
    if (text.size() == 1) return false;
  }

  uint64_t magnitude = 0;
    // 負数の絶対値の上限は正数より 1 大きい。いったん uint64 で累積してから、符号に応じて範囲を判定する。
  const uint64_t limit = negative
      ? static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1u
      : static_cast<uint64_t>(std::numeric_limits<int64_t>::max());

  for (; i < text.size(); ++i) {
    const auto uch = static_cast<unsigned char>(text[i]);
    if (!std::isdigit(uch)) return false;
    const uint64_t digit =
        static_cast<uint64_t>(uch - static_cast<unsigned char>('0'));
    if (magnitude > (limit - digit) / 10u) return false;
    magnitude = magnitude * 10u + digit;
  }
  if (magnitude > limit) return false;

  out = negative ? -static_cast<int64_t>(magnitude)
                 : static_cast<int64_t>(magnitude);
  return true;
}

} // namespace detail

/**
 * @brief MySQL の値を uint32_t として安全に読み取る
 *
 * @param v MySQL の値。呼び出し中だけ借用する。従来プロトコルのテキスト結果セットでは
 *          すべての列が文字列なので、一律にテキストとして解析する。
 * @param fb NULL、解析失敗、または uint32_t の範囲外のときに返す代替値。
 *
 * @return 変換に成功した uint32_t。
 */
inline uint32_t safeToU32(const db::Value &v, uint32_t fb = 0) {
  if (v.isNull()) return fb;
  uint64_t parsed = 0;
  if (!detail::parseFullU64(v.str(), parsed)) return fb;
  if (parsed > std::numeric_limits<uint32_t>::max()) return fb;
  return static_cast<uint32_t>(parsed);
}

/**
 * @brief MySQL の値を uint64_t として安全に読み取る
 *
 * @param v MySQL の値。呼び出し中だけ借用する。
 * @param fb NULL または解析失敗のときに返す代替値。
 *
 * @return 変換に成功した uint64_t。
 */
inline uint64_t safeToU64(const db::Value &v, uint64_t fb = 0) {
  if (v.isNull()) return fb;
  uint64_t parsed = 0;
  if (!detail::parseFullU64(v.str(), parsed)) return fb;
  return parsed;
}

/**
 * @brief MySQL の値を int32_t として安全に読み取る
 *
 * @param v MySQL の値。呼び出し中だけ借用する。意味的に負数を取りうる列に使う
 *          （例：shop_items.stock では -1 が在庫無制限を表す）。
 * @param fb NULL、解析失敗、または int32_t の範囲外のときに返す代替値。
 *
 * @return 変換に成功した int32_t。
 */
inline int32_t safeToI32(const db::Value &v, int32_t fb = 0) {
  if (v.isNull()) return fb;
  int64_t parsed = 0;
  if (!detail::parseFullI64(v.str(), parsed)) return fb;
  if (parsed < std::numeric_limits<int32_t>::min() ||
      parsed > std::numeric_limits<int32_t>::max()) {
    return fb;
  }
  return static_cast<int32_t>(parsed);
}

/**
 * @brief MySQL の値を float として安全に読み取る
 *
 * @param v MySQL の値。呼び出し中だけ借用する。
 * @param fb NULL または解析失敗のときに返す代替値。
 *
 * @return 変換に成功した float。
 */
inline float safeToF32(const db::Value &v, float fb = 0.f) {
  if (v.isNull()) return fb;
  float parsed = 0.f;
  if (!detail::parseFullF32(v.str(), parsed)) return fb;
  return parsed;
}

/**
 * @brief MySQL の値を文字列として安全に読み取る
 *
 * @param v MySQL の値。呼び出し中だけ借用する。
 *
 * @return フィールドのテキスト。
 * @retval 空文字列 値が NULL。
 */
inline std::string safeToStr(const db::Value &v) {
  return v.str();
}

} // namespace utils
