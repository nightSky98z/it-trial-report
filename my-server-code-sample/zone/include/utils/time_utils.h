#pragma once
#include <chrono>
#include <cstdint>

namespace timeutil {

/**
 * @brief 単調増加クロックの現在値をミリ秒で返す
 *
 * @return steady_clock のミリ秒値。システム時刻の巻き戻しの影響を受けない。
 *
 * @note
 * - タイムアウト、処理時間の計測、tick のフレーム計測、ハートビート、DB の一括書き込み間隔に使う。
 * - 現実の時刻として永続化しないこと。
 */
inline uint64_t steady_now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(
             steady_clock::now().time_since_epoch())
      .count();
}

/**
 * @brief 現実世界の時刻（壁時計）をミリ秒で返す
 *
 * @return system_clock のミリ秒値。システムの時刻同期で変動することがある。
 *
 * @note
 * - ログのタイムスタンプ、トークンの絶対有効期限、DB への保存、Envelope の sendTimeMs に使う。
 * - フレーム間隔やタイムアウトの計算には使わないこと。
 */
inline uint64_t wall_now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(
             system_clock::now().time_since_epoch())
      .count();
}

} // namespace timeutil
