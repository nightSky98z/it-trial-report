#pragma once
#include "ecs/events.h"
#include "ecs/map_components.h"
#include "ecs/player_ship_components.h"
#include <entt/entt.hpp>

namespace healing_station_system {

/**
 * @brief 治療施設の範囲内にいるプレイヤー船を継続的に回復する
 *
 * @param world マップの ECS world。プレイヤー船の HP コンポーネントを更新する。
 * @param hub イベントハブ。このフレームの回復イベントを hp_changed.curr に書き込む。
 * @param dtSec このフレームのシミュレーション秒数。
 * @param mapId 現在のマップ ID（イベントのペイロードに使う）。
 *
 * @note
 * - プレイヤー船 AI の後に呼ぶこと。Dying から Alive への状態遷移は
 *   引き続き player_ship_ai_system が担当する。
 * - 処理対象は BuildingKind_HealingStation のみ。治療施設エンティティの生成は行わない。
 */
void update(entt::registry &world, MonsterAIEventHub &hub, float dtSec,
            uint32_t mapId);

} // namespace healing_station_system
