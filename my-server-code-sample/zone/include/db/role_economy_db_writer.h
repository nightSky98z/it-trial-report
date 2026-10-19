#pragma once
#include <cstdint>
#include <string>

/**
 * @file role_economy_db_writer.h
 * @brief バッグとゴールドを非同期に永続化するためのタスク投入口
 *
 * player_ship_db_writer と同じ方式。ロジックスレッドはまずメモリ上の値を更新して即座に応答し、
 * その後、変更後の絶対値を MySQL ワーカーへ渡して DB に書き込む。
 *
 * @note
 * - 差分ではなく絶対値を投入するので、タスクの欠落や順序の入れ替わりで値がずれることはない。
 *   ただし、プロセスがクラッシュすると最後の flush 周期内の変更は失われる可能性がある。
 *   ゴールドにより強い一貫性が必要なら、同期的に DB へ書いてから応答する方式に変える必要がある
 *   （それは別の設計になる）。
 */
namespace role_economy_db_writer {

/**
 * @brief アイテム 1 種類の数量変更を投入する
 *
 * @param roleId キャラクター ID。スレッドをまたぐタスクのため値でコピーする。
 * @param itemId items.item_id。
 * @param newCount 変更後の数量（絶対値）。0 はそのアイテムがなくなったことを表し、消費側で該当行を削除する。
 *
 * @note
 * - メモリ上のバッグの更新が成功した後に呼ぶこと。失敗したリクエストで永続化状態を汚さないため。
 */
void enqueueItemCount(const std::string &roleId, uint32_t itemId,
                      uint32_t newCount);

/**
 * @brief ゴールドの変更を投入する
 *
 * @param roleId キャラクター ID。スレッドをまたぐタスクのため値でコピーする。
 * @param newGold 変更後のゴールド（絶対値）。
 *
 * @note
 * - メモリ上のゴールドの更新が成功した後に呼ぶこと。
 */
void enqueueGold(const std::string &roleId, uint32_t newGold);

/**
 * @brief 装備スロット 1 つの変更を投入する
 *
 * @param roleId キャラクター ID。スレッドをまたぐタスクのため値でコピーする。
 * @param unitId 精霊のテンプレート ID。
 * @param slot 装備スロット（1=武器 2=防具 3=アクセサリ）。
 * @param itemId 変更後にそのスロットに入っているアイテム。0 は装備解除を表し、消費側で該当行を削除する。
 *
 * @note
 * - メモリ上の装備スロットの更新が成功した後に呼ぶこと。
 */
void enqueueUnitEquip(const std::string &roleId, uint32_t unitId, uint8_t slot,
                      uint32_t itemId);

/**
 * @brief キャラクターの建物状態の変更を投入する
 *
 * @param roleId キャラクター ID。スレッドをまたぐタスクのため値でコピーする。
 * @param buildingId buildings.id。
 * @param x 配置座標 X。
 * @param y 配置座標 Y。
 * @param level 建物レベル。
 * @param state 0=ロック 1=解放済み。
 *
 * @note
 * - メモリ上の建物状態の更新が成功した後に呼ぶこと。
 */
void enqueueRoleBuilding(const std::string &roleId, uint32_t buildingId,
                         float x, float y, uint32_t level, uint8_t state);

/**
 * @brief 募集で加わった精霊を投入する
 *
 * @param roleId キャラクター ID。
 * @param unitId 精霊のテンプレート ID。
 * @param baseHp テンプレートからコピーした基礎 HP。
 * @param baseAtk テンプレートからコピーした基礎攻撃力。
 * @param baseDef テンプレートからコピーした基礎防御力。
 * @param baseSpeed テンプレートからコピーした移動速度。
 * @param attackRange テンプレートからコピーした攻撃距離。
 *
 * @note
 * - メモリ上の ownedUnitIds とゴールドの更新が済んだ後に呼ぶこと。
 */
void enqueueRoleUnit(const std::string &roleId, uint32_t unitId,
                     uint32_t baseHp, uint32_t baseAtk, uint32_t baseDef,
                     uint32_t baseSpeed, float attackRange);

/**
 * @brief 編成スロットの変更を投入する
 *
 * @param roleId キャラクター ID。
 * @param teamId 編成番号。
 * @param slotIndex スロットのインデックス。
 * @param unitId スロットに入る精霊。0 は編成から外すことを表し、消費側で該当行を削除する。
 */
void enqueueTeamSlot(const std::string &roleId, uint8_t teamId,
                     uint8_t slotIndex, uint32_t unitId);

/**
 * @brief 出撃中の編成の変更を投入する
 *
 * @param roleId キャラクター ID。
 * @param teamId 切り替え後の編成番号。
 */
void enqueueSelectedTeam(const std::string &roleId, uint8_t teamId);

} // namespace role_economy_db_writer
