#pragma once
#include "ecs/events.h"
#include "ecs/map_components.h"
#include <entt/entt.hpp>

namespace aggro_clear_subscriber {

/**
 * @brief 前フレームで発行されたヘイト解除イベントを処理する
 *
 * @param world マップの ECS world。UnitAggro.threat_table を更新する。
 * @param hub イベントハブ。aggro_clear.prev を消費してクリアする（curr は触らない）。
 *
 * @note
 * - event_clear_system が curr を prev へローテートした後、次のフレームで呼ぶこと。
 * - キューに積まれた対象を取り除くだけで、対象エンティティが現在も存在するかは確認しない。
 * - 同じ tick に他の購読者がいる場合、そちらは本関数より前に aggro_clear.prev を読むこと。
 */
void update(entt::registry &world, MonsterAIEventHub &hub);

} // namespace aggro_clear_subscriber
