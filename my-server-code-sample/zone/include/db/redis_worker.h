#pragma once

namespace zone {
struct ZoneConfig;
}

namespace redis_worker {

/**
 * @brief Redis のキャッシュ／転送ワーカーを実行する
 *
 * @param cfg zone 設定。ワーカー起動時に Redis の接続パラメータを読み取る。
 *
 * @note
 * - 呼び出したスレッドをブロックする。shutdown が要求され、他の producer が終了し、
 *   経験値キューが空になるまで戻らない。
 * - 呼び出し側は専用のワーカースレッドで実行し、プロセス終了時に join すること。
 * - 経験値タスクは先に MySQL の最終永続化キューへ転送する。Redis への書き込みに
 *   失敗しても影響を受けるのは短期キャッシュだけである。
 */
void run(const zone::ZoneConfig &cfg);

} // namespace redis_worker
