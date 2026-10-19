#pragma once
#include "ecs/events.h"
#include "ecs/map_components.h"
#include "ecs/player_ship_components.h"
#include "game/template_cache.h"
#include <entt/entt.hpp>

namespace player_ship_ai_system {

/**
 * @brief プレイヤー船の AI ステートマシンを進める
 *
 * @param world マップの ECS world。プレイヤー船の AI・nav・velocity・HP を更新する。
 * @param hub イベントハブ。revive や aggro_clear など、このフレームのイベントを書き込む。
 * @param dtSec このフレームのシミュレーション秒数。
 * @param nowMs 現在のシミュレーション／壁時計のミリ秒。状態のタイムアウトとターゲット再選択の間引きに使う。
 * @param mapId 現在のマップ ID（イベントのペイロードに使う）。
 *
 * @note
 * - 呼び出し前に area_registry_system を更新し、同じエリアのモンスター索引を使える状態にしておくこと。
 * - 治療施設は HP の回復だけを担当する。Dying/Retreating/Healing の状態遷移は本関数が担当する。
 */
void update(entt::registry &world, MonsterAIEventHub &hub, float dtSec,
            uint64_t nowMs, uint32_t mapId);

/**
 * @brief 指定キャラクターのプレイヤー船を、別のエリアへ割り当て直す
 *
 * @param world マップの ECS world。該当するプレイヤー船の assignment・AI・nav を更新する。
 * @param hub イベントハブ。AggroClearRequestEvent を書き込む。
 * @param roleId キャラクター ID。呼び出し中だけ借用する。
 * @param areaId 割り当て先のエリア ID（1 始まり）。
 * @param nowMs 現在のタイムスタンプ（ミリ秒）。割り当て時刻の記録に使う。
 * @param mapId 現在のマップ ID（イベントのペイロードに使う）。
 *
 * @note
 * - C2S_AssignShipToArea の検証に成功した後にだけ呼ぶこと。
 * - CAreaMember は、船が割り当て先のアンカーに到着するまで元の戦闘エリアのまま維持する。
 */
/**
 * @brief キャラクターが所有する精霊 1 体を、指定したエリアに割り当てる
 *
 * @param world マップの ECS world。その精霊の割り当てと AI 状態をその場で更新する。
 * @param hub イベントハブ。aggro_clear 要求を書き込む。
 * @param roleId 精霊を所有するキャラクター。
 * @param unitId 対象の精霊のテンプレート ID。
 * @param areaId 割り当て先のエリア（1=メインシティ 2=森 3=戦場 4=墓地）。
 * @param nowMs 現在のマップインスタンスの SimClock（ミリ秒）。
 * @param mapId 現在のマップ ID。
 *
 * @retval false そのキャラクターの該当する精霊エンティティが見つからない。
 *
 * @note
 * - 編成への出撃・待機の切り替えに使う。精霊のエンティティは常に存在させたまま、
 *   行き先だけを変える。破棄して作り直すとプロトコル上のエンティティ ID が変わり、
 *   クライアント側のキャッシュが溜まり続けてしまうため。
 */
bool assignShipToAreaByUnitId(entt::registry &world, MonsterAIEventHub &hub,
                              const std::string &roleId, uint32_t unitId,
                              uint32_t areaId, uint64_t nowMs, uint32_t mapId);

/**
 * @brief 編成に従って、キャラクターが所有する精霊を割り当てる
 *
 * @param world マップの ECS world。
 * @param hub イベントハブ。aggro_clear 要求を書き込む。
 * @param roleId 精霊を所有するキャラクター。
 * @param areaId 出撃する精霊の割り当て先エリア（1=メインシティ 2=森 3=戦場 4=墓地）。
 * @param nowMs 現在のマップインスタンスの SimClock（ミリ秒）。
 * @param mapId 現在のマップ ID。
 * @param deployedUnitIds 現在の出撃編成に含まれる精霊のテンプレート ID 一覧。呼び出し中だけ借用する。
 *
 * @note
 * - ルールは 1 つだけ：**deployedUnitIds に含まれる精霊は areaId へ、含まれない精霊はすべて
 *   メインシティへ戻す**。マップ上にはキャラクターが所有する全精霊（待機中のものも含む）が
 *   存在するので、編成で区別しないと待機中の精霊まで一緒に出撃してしまう。
 * - このルールで「プレイヤーによるエリア選択」「編成の切り替え」「ボス挑戦」「ボス撃破後の
 *   移動先変更」をすべてカバーする。どれも実現したいことは同じなので、個別に実装しないこと。
 */
void assignShipsToArea(entt::registry &world, MonsterAIEventHub &hub,
                       const std::string &roleId, uint32_t areaId,
                       uint64_t nowMs, uint32_t mapId,
                       const std::vector<uint32_t> &deployedUnitIds);

/**
 * @brief 撃破イベントを処理し、該当するプレイヤー船に経験値を与える
 *
 * @param world マップの ECS world。CExperience や UnitBase/UnitCombat などの成長関連の値を更新する。
 * @param hub イベントハブ。unit_killed.prev を消費してクリアする。
 * @param tc テンプレートキャッシュ。倒したモンスターの基礎経験値を取得する（呼び出し中だけ借用）。
 * @param mapId 現在のマップ ID（成長イベントの範囲判定に使う）。
 * @param maxLevel 現在のレベル上限（env.zone の MAX_UNIT_LEVEL。ホットリロード可能）。
 *
 * @note
 * - 経験値は**編成単位**で配る。撃破したキャラクターの出撃中の精霊全員が 1 人分ずつ受け取り、
 *   総量を山分けするわけではない。各精霊は自分とモンスターのレベル差に応じて個別に減衰し、下限は 1。
 * - 待機中（CPlayerShipAssignment::deployed == false）の精霊は対象外。
 * - イベントのローテート後に、前フレームの撃破イベントを処理する。
 * - 同じ tick に他の購読者がいる場合、そちらは本関数より前に unit_killed.prev を読むこと。
 * - 経験値の DB 書き込みは、tick の最後に player_ship_db_writer が dirty フラグを走査して行う。
 */
void consumeKills(entt::registry &world, MonsterAIEventHub &hub,
                  const cache::TemplateCache &tc, uint32_t mapId,
                  uint32_t maxLevel);

} // namespace player_ship_ai_system
