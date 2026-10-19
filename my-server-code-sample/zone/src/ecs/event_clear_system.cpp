#include "ecs/event_clear_system.h"

namespace event_clear_system {

/**
 * @brief メイン world のイベントバッファをローテートする
 *
 * @note
 * - tick の最後に呼ぶ。購読側は次のフレームで prev を読む。
 */
void update(ZoneRegistry &zone) {
  rotateMonsterAIEventHub(zone.monster_ai_events);
  rotatePlayerAIEventHub(zone.player_ai_events);
}

} // namespace event_clear_system
