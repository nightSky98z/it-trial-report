#include "db/mysql_client.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace db {
namespace {

/**
 * @brief プロセス内で libmysqlclient を一度だけ初期化する
 *
 * @note
 * - mysql_library_init はスレッドセーフではないため、どの接続を作るよりも前に一度だけ実行する必要がある。
 *   複数のワーカースレッドがそれぞれ接続を作るので、ここで call_once を使って 1 回にまとめている。
 */
void ensureLibraryInit() {
  static std::once_flag once;
  std::call_once(once, []() { mysql_library_init(0, nullptr, nullptr); });
}

/**
 * @brief 接続のコンテキスト情報を含むエラーメッセージを組み立てる
 */
std::string errorText(MYSQL *conn, const char *what) {
  std::string msg = what;
  if (conn) {
    msg += ": ";
    msg += mysql_error(conn);
    msg += " (errno ";
    msg += std::to_string(mysql_errno(conn));
    msg += ")";
  }
  return msg;
}

} // namespace

Session::Session(const char *host, int port, const char *user,
                 const char *password, const char *database, bool useTls) {
  ensureLibraryInit();

  conn_ = mysql_init(nullptr);
  if (!conn_) {
    throw Error("mysql_init failed (out of memory)");
  }

  unsigned int sslMode = useTls ? SSL_MODE_REQUIRED : SSL_MODE_DISABLED;
  mysql_options(conn_, MYSQL_OPT_SSL_MODE, &sslMode);

    // MYSQL_OPT_RECONNECT は設定しない。8.0.34 以降は非推奨で、デフォルトでも無効になっており、
    // 明示的に設定すると接続のたびに非推奨の警告が出るだけ。切断時は上位のワーカーが明示的に再接続するので、
    // ドライバによる暗黙の再接続も不要（暗黙の再接続はトランザクションなどのセッション状態を黙って失う）。
  mysql_options(conn_, MYSQL_SET_CHARSET_NAME, "utf8mb4");

    // CLIENT_FOUND_ROWS：affected_rows が「実際に変更した行数」ではなく「一致した行数」を返すようにする。
    // デフォルトの動作では、元と同じ値を書き込む UPDATE は 0 を返すため、呼び出し側は
    // 「行が存在しない」のか「値が変わらなかった」のかを区別できない。たとえば戦闘に出ていない精霊は
    // 経験値が変わらないため、role_unit が存在しないと誤って報告されてしまう。FOUND_ROWS にすることで、
    // affected_rows == 0 が本当に「対象の行が存在しない」ことを意味するようになる。
    //
    // CLIENT_MULTI_STATEMENTS は有効にしない。複数文は不要であり、SQL インジェクションの攻撃面を広げないため。
  if (!mysql_real_connect(conn_, host, user, password,
                          (database && database[0]) ? database : nullptr,
                          static_cast<unsigned int>(port), nullptr,
                          CLIENT_FOUND_ROWS)) {
    const std::string msg = errorText(conn_, "mysql connect failed");
    mysql_close(conn_);
    conn_ = nullptr;
    throw Error(msg);
  }
}

void Session::close() {
  if (conn_) {
    mysql_close(conn_);
    conn_ = nullptr;
  }
}

/**
 * @brief パラメータなしの文を 1 つ実行し、結果セットは捨てる
 *
 * @throws Error 実行に失敗した場合。
 */
static void execSimple(MYSQL *conn, const char *sql, const char *what) {
  if (!conn) throw Error(std::string(what) + ": no connection");
  if (mysql_real_query(conn, sql, static_cast<unsigned long>(std::strlen(sql))) != 0) {
    throw Error(errorText(conn, what));
  }
    // トランザクション制御文には結果セットがないが、プロトコル上は読み捨てる必要がある。
  MYSQL_RES *res = mysql_store_result(conn);
  if (res) mysql_free_result(res);
}

void Session::startTransaction() {
  execSimple(conn_, "START TRANSACTION", "start transaction failed");
}

void Session::commit() { execSimple(conn_, "COMMIT", "commit failed"); }

void Session::rollback() { execSimple(conn_, "ROLLBACK", "rollback failed"); }

void Statement::appendFloating(double v, int significantDigits) {
    // NaN / Inf は SQL にリテラル表現がない。黙って 0 として書き込むと座標などの業務データを汚してしまうため、
    // 文全体を拒否し、呼び出し側が DB との境界で問題に気づけるようにする。
  if (!std::isfinite(v)) {
    throw Error("bind failed: non-finite floating point parameter");
  }
  char buffer[64];
  const int written =
      std::snprintf(buffer, sizeof(buffer), "%.*g", significantDigits, v);
  if (written <= 0 || static_cast<std::size_t>(written) >= sizeof(buffer)) {
    throw Error("bind failed: cannot format floating point parameter");
  }
  params_.emplace_back(buffer, static_cast<std::size_t>(written));
}

std::string Statement::quoted(const std::string &raw) const {
  MYSQL *const conn = session_ ? session_->native() : nullptr;
  if (!conn) throw Error("bind failed: no connection");

    // mysql_real_escape_string は最悪の場合 1 バイトが 2 バイトに膨らむ。さらに終端の NUL 分を足す。
  std::string buffer;
  buffer.resize(raw.size() * 2 + 1);
  const unsigned long written = mysql_real_escape_string(
      conn, buffer.data(), raw.data(), static_cast<unsigned long>(raw.size()));
  buffer.resize(written);

  return "'" + buffer + "'";
}

Result Statement::execute() {
  MYSQL *const conn = session_ ? session_->native() : nullptr;
  if (!conn) throw Error("execute failed: no connection");

    // プレースホルダをパラメータで置き換える。`?` は文字列リテラルの外側にあるものだけを対象にし、
    // SQL 文中の引用符内にある疑問符をパラメータの位置と誤認しないようにする。
  std::string finalSql;
  finalSql.reserve(sql_.size() + 64);

  std::size_t nextParam = 0;
  char quoteChar = '\0';  // 0 なら現在リテラルの外にいる

  for (std::size_t i = 0; i < sql_.size(); ++i) {
    const char c = sql_[i];

    if (quoteChar != '\0') {
      finalSql.push_back(c);
      if (c == '\\' && i + 1 < sql_.size()) {
                // リテラル内のエスケープシーケンスはまとめて読み飛ばす。\' をリテラルの終わりと誤判定しないため。
        finalSql.push_back(sql_[++i]);
      } else if (c == quoteChar) {
        quoteChar = '\0';
      }
      continue;
    }

    if (c == '\'' || c == '"' || c == '`') {
      quoteChar = c;
      finalSql.push_back(c);
      continue;
    }

    if (c == '?') {
      if (nextParam >= params_.size()) {
        throw Error("execute failed: more placeholders than bound parameters");
      }
      finalSql += params_[nextParam++];
      continue;
    }

    finalSql.push_back(c);
  }

  if (nextParam != params_.size()) {
    throw Error("execute failed: more bound parameters than placeholders");
  }

  if (mysql_real_query(conn, finalSql.data(),
                       static_cast<unsigned long>(finalSql.size())) != 0) {
    throw Error(errorText(conn, "query failed"));
  }

  const uint64_t affected = mysql_affected_rows(conn);
  MYSQL_RES *const res = mysql_store_result(conn);
  if (!res && mysql_errno(conn) != 0) {
    throw Error(errorText(conn, "store result failed"));
  }

  return Result{res, affected};
}

} // namespace db
