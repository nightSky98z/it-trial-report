#pragma once
#include <cstdint>

namespace utils {

/**
 * @brief サーバー内部のエラーコード
 *
 * @note
 * - 値はログやプロトコルのエラー情報に載るため、列挙子を追加しても既存の値は変えないこと。
 */
enum class ErrorCode {
  LoadEnvFileFailed = 9000,
};

/**
 * @brief 内部エラーコードを、プロトコル／ログ用の整数に変換する
 *
 * @param code 内部エラーの列挙値。
 *
 * @return 対応する uint32_t の値。
 */
inline uint32_t Error(const ErrorCode code) {
  return static_cast<uint32_t>(code);
}

} // namespace utils
