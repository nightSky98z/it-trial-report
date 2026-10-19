#pragma once

#include "config/zone_config.h"
#include <cstdint>
#include <string>

namespace game {
/**
 * @brief ゲームループを実行する（呼び出したスレッドをブロックする）
 *
 * @param zone_id このスレッドが担当するゾーン番号（map_id 単位でルーティングする）
 * @param cfg     実行時設定（DB 接続情報や tick_rate など）
 */
void run(uint8_t zone_id, const zone::ZoneConfig &cfg);
} // namespace game
