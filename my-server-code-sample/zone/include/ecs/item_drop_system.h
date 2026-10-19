#pragma once
#include "ecs/events.h"
#include "ecs/map_components.h"
#include "ecs/pickable_component.h"
#include "game/template_cache.h"
#include <cstdint>
#include <entt/entt.hpp>
#include <vector>

/**
 * @file item_drop_system.h
 * @brief モンスター撃破時のドロップと、期限切れドロップの片付け
 */
namespace item_drop_system {

/**
 * @brief ドロップアイテムが地面に残る時間（ミリ秒）
 *
 * @note
 * - Java 版の DROP_EXPIRE_MS と同じ。ドロップアイテムは 1 分で消える。
 */
inline constexpr uint64_t kDropExpireMs = 60ull * 1000ull;

/**
 * @brief ドロップアイテムのプロトコル上のユニット種別
 *
 * @note
 * - fbs の UnitKind.DropItem に対応する。クライアントはこれでドロップと通常ユニットを区別する。
 */
inline constexpr uint8_t kDropItemUnitKind = 4;

/**
 * @brief ドロップアイテムの陣営
 *
 * @note
 * - Java 版の取り決めを踏襲：0=募集可能 1=モンスター 2=ボス 3=ドロップアイテム。
 *   ドロップアイテムは戦闘に参加しないため、陣営はクライアントでの区別とサーバーでの絞り込みにだけ使う。
 */
inline constexpr uint32_t kDropItemCampId = 3;

/**
 * @brief 重み付きランダムでドロップアイテムを 1 つ選ぶ
 *
 * @param drops 借用するドロップ設定。rate は相対的な重み。
 * @param roll01 [0,1) の乱数。テストで結果を固定できるよう、呼び出し側から渡す。
 *
 * @return 選ばれた items.item_id。
 * @retval 0 設定が空、または重みの合計が 0 以下（ドロップなし）。
 *
 * @note
 * - 重みの合計は 1 未満でもよい。その場合、[重みの合計, 1) に入ったらドロップなしとみなす。
 *   Java 版の `roll = random * totalRate` とは挙動が異なり、こちらは「落ちないこともある」を表現できる。
 */
uint32_t pickDropItem(const std::vector<cache::DropEntry> &drops, float roll01);

/**
 * @brief この tick の撃破イベントを処理し、ドロップアイテムのエンティティを生成する
 *
 * @param inst 対象のマップインスタンス。ドロップアイテムのエンティティを作り、network entity id を割り当てる。
 * @param tc テンプレートキャッシュ。ドロップ設定を読むだけ。
 * @param killed 借用する撃破イベントの一覧。
 * @param nowMs 現在のマップインスタンスの SimClock（ミリ秒）。消滅時刻の計算に使う。
 *
 * @note
 * - プレイヤー船 AI が unit_killed.prev を消費してクリアする前に呼ぶこと（後だとイベントを取得できない）。
 * - 処理するのはモンスター陣営の死亡だけ。プレイヤー船が倒れてもドロップは発生しない。
 */
void spawnDropsForKills(MapInstance &inst, const cache::TemplateCache &tc,
                        const std::vector<UnitKilledEvent> &killed,
                        uint64_t nowMs);

/**
 * @brief 期限切れのドロップアイテムを片付ける
 *
 * @param inst 対象のマップインスタンス。期限切れのエンティティを破棄する。
 * @param nowMs 現在のマップインスタンスの SimClock（ミリ秒）。
 * @param[out] outRemovedNetworkIds 破棄したエンティティのプロトコル ID を追記する。AOI の left で配信するために使う。
 *
 * @note
 * - outRemovedNetworkIds をそのインスタンスの全接続に送るのは呼び出し側の責任。
 *   送らないと、消えたはずのドロップアイテムがクライアントに残り続ける。
 */
void cleanupExpiredDrops(MapInstance &inst, uint64_t nowMs,
                         std::vector<uint32_t> &outRemovedNetworkIds);

} // namespace item_drop_system
