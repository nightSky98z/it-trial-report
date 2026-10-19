#pragma once

#include "ecs/registry.h"

namespace event_clear_system {

/**
 * @brief tick ごとのイベントバッファをローテートしてクリアする
 *
 * @param zone メイン world の registry ラッパー。ZoneRegistry 直下のイベントハブを操作する。
 *
 * @note
 * - その tick のイベント生成側がすべて実行し終わった後に呼ぶこと。
 * - 呼び出し後、全イベント列の curr は prev に移り、古い prev は破棄される。
 */
void update(ZoneRegistry &zone);

}
