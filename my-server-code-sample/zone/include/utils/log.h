#pragma once

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace zone_log {

/**
 * @brief ログレベル
 *
 * @note
 * - レベルによって、固定の接頭辞・色・出力先が決まる。
 */
enum class Level {
  Debug,
  Info,
  Warning,
  Error,
};

/**
 * @brief ログレベルの接頭辞を返す
 *
 * @param level ログレベル。
 *
 * @return コロン付きの小文字の固定接頭辞。
 */
inline const char *prefix(Level level) {
  switch (level) {
  case Level::Debug: return "debug:";
  case Level::Info: return "info:";
  case Level::Warning: return "warning:";
  case Level::Error: return "error:";
  }
  return "info:";
}

/**
 * @brief ログレベルに対応する ANSI カラーを返す
 *
 * @param level ログレベル。
 *
 * @return ANSI カラーのエスケープシーケンス。最後のリセットは呼び出し側が行う。
 */
inline const char *color(Level level) {
  switch (level) {
  case Level::Debug: return "\033[37m";
  case Level::Info: return "\033[32m";
  case Level::Warning: return "\033[33m";
  case Level::Error: return "\033[31m";
  }
  return "\033[32m";
}

/**
 * @brief ログの出力先ストリームを返す
 *
 * @param level ログレベル。
 *
 * @return warning/error は stderr、debug/info は stdout。
 */
inline FILE *stream(Level level) {
  return (level == Level::Warning || level == Level::Error) ? stderr : stdout;
}

/**
 * @brief ログ用のグローバルなミューテックスを返す
 *
 * @return プロセス内の静的な mutex への参照。
 *
 * @note
 * - ログ 1 件の書き込みを直列化するためだけに使う。複数スレッドの出力が混ざらないようにする。
 */
inline std::mutex &mutex() {
  static std::mutex m;
  return m;
}

/**
 * @brief 書式付きのログを 1 件書き込む
 *
 * @param level ログレベル。
 * @param fmt printf 形式の書式文字列。nullptr の場合は接頭辞と改行だけを出力する。
 * @param args 開始済みの va_list。呼び出し中だけ使う。
 *
 * @note
 * - fmt と可変引数の型を一致させるのは呼び出し側の責任。
 */
inline void vlog(Level level, const char *fmt, va_list args) {
  FILE *out = stream(level);
  std::lock_guard<std::mutex> lock(mutex());
  std::fprintf(out, "%s%s ", color(level), prefix(level));
  if (fmt) {
    std::vfprintf(out, fmt, args);
  }
  std::fprintf(out, "\033[0m\n");
  std::fflush(out);
}

/**
 * @brief debug ログを出力する
 *
 * @param fmt printf 形式の書式文字列。
 * @param ... 書式の引数。
 */
inline void debug(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vlog(Level::Debug, fmt, args);
  va_end(args);
}

/**
 * @brief info ログを出力する
 *
 * @param fmt printf 形式の書式文字列。
 * @param ... 書式の引数。
 */
inline void info(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vlog(Level::Info, fmt, args);
  va_end(args);
}

/**
 * @brief warning ログを出力する
 *
 * @param fmt printf 形式の書式文字列。
 * @param ... 書式の引数。
 */
inline void warning(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vlog(Level::Warning, fmt, args);
  va_end(args);
}

/**
 * @brief error ログを出力する
 *
 * @param fmt printf 形式の書式文字列。
 * @param ... 書式の引数。
 */
inline void error(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vlog(Level::Error, fmt, args);
  va_end(args);
}

} // namespace zone_log
