#pragma once
#include "ecs/events.h"
#include "ecs/map_components.h"
#include <entt/entt.hpp>

namespace city_monster_combat_system {

/**
 * @brief メインシティのモンスターに、同じエリア内での追跡と攻撃を行わせる
 *
 * @param world マップの ECS world。モンスターの nav・velocity・攻撃クールダウンを更新する。
 * @param hub イベントハブ。攻撃が当たると戦闘／死亡／ヘイト解除のイベントを書き込む。
 * @param dtSec このフレームのシミュレーション秒数。
 * @param mapId 現在のマップ ID（イベントのペイロードに使う）。
 *
 * @note
 * - monster_wander_system より前、もしくは同じ意思決定フェーズで呼ぶこと。
 *   in_combat のモンスターは徘徊システム側でスキップされる。
 * - 攻撃対象は同じ CAreaMember.areaId の中からのみ選ぶ。Dying や Alive 以外の対象は攻撃できない。
 */
void update(entt::registry &world, MonsterAIEventHub &hub, float dtSec,
            uint32_t mapId);

} // namespace city_monster_combat_system
