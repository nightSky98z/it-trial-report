#pragma once

namespace zone {
struct ZoneConfig;
}

namespace mysql_worker {

/**
 * @brief MySQL のスキーマと初期データ（baseline）を準備する
 *
 * @param cfg zone 設定。文字列は呼び出し中だけ借用する。
 *
 * @retval true スキーマと起動に必要なデータの準備が完了した。
 * @retval false 接続または初期化に失敗した。呼び出し側は起動処理を中止すること。
 */
bool bootstrap(const zone::ZoneConfig &cfg);

/**
 * @brief MySQL 永続化ワーカーを実行する
 *
 * @param cfg zone 設定。ワーカー起動時に接続パラメータを読み取る。
 * @param flush_interval_sec 一括書き込みの間隔（秒）。0 以下の値は渡さないこと。
 *
 * @note
 * - 呼び出したスレッドをブロックする。shutdown が要求され、producer が終了し、
 *   受信キューと保留中のバッチがすべて空になるまで戻らない。
 * - 呼び出し側は専用のワーカースレッドで実行し、プロセス終了時に join すること。
 */
void run(const zone::ZoneConfig &cfg, int flush_interval_sec);

} // namespace mysql_worker
