#pragma once
#include "config/runtime_config.h"
#include "config/zone_config.h"
#include "db/mysql_client.h"
#include <memory>

/**
 * @file mysql_x_session.h
 * @brief 起動時の設定読み込みで使う MySQL 接続を作成する
 *
 * X DevAPI（33060）から従来プロトコル（3306）へ移行済み。
 * ここで zone 設定から db::Session を作る処理を一元化している。
 */
namespace db {

/**
 * @brief 現在のビルド設定が TLS を要求しているかを返す
 *
 * @retval true runtime_config が TLS を要求している。接続は SSL_MODE_REQUIRED を使う。
 */
inline bool tlsRequiredFromConfig() {
  return runtime_config::kMySqlXTlsMode != runtime_config::MySqlXTlsMode::Disabled;
}

/**
 * @brief zone 設定から新しい MySQL 接続を 1 本作成する
 *
 * @param cfg zone 設定。接続パラメータは呼び出し中だけ借用する。
 *
 * @return 所有権を持つ Session。寿命の管理は呼び出し側が行う。
 *
 * @throws db::Error 接続に失敗した場合。呼び出し側で捕捉し、起動失敗として扱うこと。
 *
 * @note
 * - 起動時の設定読み込みのような一度きりの用途専用。
 *   常時動く書き込み処理では mysql_worker の常駐接続を使い、操作のたびに接続を作らないこと。
 */
inline std::unique_ptr<Session> makeSession(const zone::ZoneConfig &cfg) {
  return std::make_unique<Session>(cfg.mysqlHost, cfg.mysqlPort, cfg.mysqlUser,
                                   cfg.mysqlPasswd, cfg.mysqlDb,
                                   tlsRequiredFromConfig());
}

} // namespace db
