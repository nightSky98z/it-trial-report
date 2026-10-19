#pragma once

#include "ecs/events.h"
#include "protocol/codec.h"

#include <entt/entt.hpp>
#include <unordered_set>

namespace game {

/**
 * @brief ECS の戦闘イベントをプロトコルの batch item に変換する
 *
 * @param world 現在の MapInstance のエンティティ registry。借用するだけで保持しない。
 * @param hub curr -> prev のローテートが済んだイベントバッファ。prev のみ読む。
 * @param fbb 呼び出し側が持つ FlatBufferBuilder。返す offset はすべてこの builder の寿命に依存する。
 * @param serverTimeMs 各戦闘イベントに書き込むサーバー側のシミュレーション時刻。
 * @param visibleNetworkIds 現在の閲覧者から見えるプロトコル上のエンティティ ID 集合。
 *                          nullptr ならフィルタしない。
 * @param out 変換したイベントを追記する先。呼び出し側は続けて同じ fbb で batch をエンコードする。
 *
 * @note
 * - 呼び出し前に `codec::beginBattleEventBatch()` で builder をクリアして取得しておくこと。
 * - out に入った offset は、次に builder を Clear する前に使い切ること。
 */
void appendBattleEventItems(entt::registry &world,
                            const MonsterAIEventHub &hub,
                            flatbuffers::FlatBufferBuilder &fbb,
                            uint64_t serverTimeMs,
                            const std::unordered_set<uint32_t> *visibleNetworkIds,
                            codec::BattleEventBatchSoA &out);

} // namespace game
