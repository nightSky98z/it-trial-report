#pragma once
#include "ecs/components.h"
#include "ecs/map_components.h"
#include "ecs/player_ship_components.h"
#include <cstdint>
#include <entt/entt.hpp>
#include <string>

namespace player_ship_db_writer {

/**
 * @brief この tick で変更されたプレイヤー船の経験値を永続化キューに投入する
 *
 * @param world マップの ECS world。registry を借用するだけで、コンポーネントの参照は保持しない。
 *
 * @note
 * - このフレームの戦闘・レベルアップが確定している tick の最後にだけ呼ぶこと。
 * - 処理した船の CExperience.dirtyStats をクリアする。
 * - roleId が空、または unitId が 0 の場合はキューに積まないが、dirtyStats はクリアする。
 */
void flushDirtyExp(entt::registry &world);

/**
 * @brief プレイヤー船のエリア割り当てを永続化するタスクを投入する
 *
 * @param roleId キャラクター ID。スレッドをまたぐタスクのため値でコピーする。
 * @param unitId プレイヤー船のテンプレート／ユニット ID。
 * @param areaId 割り当て先のエリア ID（1 始まり）。0 は渡さないこと。
 *
 * @note
 * - C2S_AssignShipToArea の処理が成功した後に呼ぶ。失敗したリクエストで永続化状態を汚さないため。
 */
void enqueueShipArea(const std::string &roleId, uint32_t unitId,
                     uint32_t areaId);

/**
 * @brief ログアウト前に、指定キャラクターの全プレイヤー船の状態を投入する
 *
 * @param world マップの ECS world。コンポーネントは借用するだけで、呼び出し後まで参照を保持しない。
 * @param roleId ログアウトするキャラクターの ID。
 *
 * @note
 * - destroyEntitiesForRole より前に呼ぶこと。後だとプレイヤー船のエンティティが既に消えている。
 * - 経験値とエリアをまとめて投入する。エンティティの破棄はその後に呼び出し側が行う。
 */
void flushAllShips(entt::registry &world, const std::string &roleId);

} // namespace player_ship_db_writer
