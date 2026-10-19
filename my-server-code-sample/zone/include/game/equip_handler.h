#pragma once
#include "game/item_cache.h"
#include "game/role_equipment.h"
#include "game/role_inventory.h"
#include <cstdint>
#include <string>
#include <vector>

/**
 * @file equip_handler.h
 * @brief 装備の着脱をサーバー側で確定させる（サーバー権威）
 *
 * 装備操作はバッグと装備スロットの間でアイテムを移動させる。装着ならバッグから 1 つ減らし、
 * 解除ならバッグに 1 つ戻す。付け替えは先に古い装備を戻してから新しい装備を減らす。
 * バッグとスロットは「両方成功」か「両方そのまま」のどちらかでなければならない。
 */
namespace equip {

/**
 * @brief 1 回の装備操作で数量が変わったアイテム 1 種類分
 *
 * @note
 * - 呼び出し側はこのリストを 1 件ずつ role_items の永続化キューに投入する。
 *   count は絶対値で、0 は行の削除を意味する。
 */
struct ItemCountChange {
  uint32_t itemId = 0;
  uint32_t newCount = 0;
};

/**
 * @brief 装備操作の結果
 *
 * @note
 * - success=false の場合、バッグと装備スロットはどちらも変更されていないことを保証する。
 * - success=true の場合、呼び出し側は次の処理を必ず行うこと。
 *   1) itemChanges に従ってバッグの永続化タスクを投入する
 *   2) unitId/slot/equippedItemId に従って unit_equips の永続化タスクを投入する
 *      （equippedItemId == 0 はそのスロットのレコード削除を意味する）
 *   3) OperationResult と BackpackChanged をクライアントへ送る
 */
struct EquipOutcome {
  bool        success = false;
  std::string message;
  uint32_t    unitId = 0;
  uint8_t     slot = 0;
  uint32_t    equippedItemId = 0;  // 操作後にこのスロットに入っているアイテム。0 なら空
  std::vector<ItemCountChange> itemChanges;
};

/**
 * @brief 指定した精霊の指定スロットにアイテムを装備する
 *
 * @param items 読み取り専用のアイテム設定。
 * @param[in,out] inv キャラクターのバッグ。成功時は新しい装備を減らし、外した古い装備を戻す。
 * @param[in,out] eq キャラクターの装備スロット。成功時はそのスロットを更新する。
 * @param unitId 対象の精霊のテンプレート ID。
 * @param itemId 装備する items.item_id。
 * @param slot 装備先のスロット。そのアイテムの sub_type と一致している必要がある。
 *
 * @return 操作結果。失敗時は inv も eq も変更されない。
 *
 * @note
 * - スロットとアイテムのサブタイプは一致していなければならない（武器を防具スロットには装備できない）。
 * - スロットに既に装備がある場合は付け替えとして扱う（古い装備はバッグへ、新しい装備はバッグから）。
 */
EquipOutcome equipItem(const cache::ItemCache &items,
                       inventory::RoleInventory &inv,
                       equipment::RoleEquipment &eq, uint32_t unitId,
                       uint32_t itemId, uint8_t slot);

/**
 * @brief 指定した精霊の指定スロットから装備を外す
 *
 * @param items 読み取り専用のアイテム設定。
 * @param[in,out] inv キャラクターのバッグ。成功時は外した装備をバッグに戻す。
 * @param[in,out] eq キャラクターの装備スロット。成功時はそのスロットを空にする。
 * @param unitId 対象の精霊のテンプレート ID。
 * @param slot 対象のスロット。
 *
 * @return 操作結果。失敗時は inv も eq も変更されない。
 */
EquipOutcome unequipItem(const cache::ItemCache &items,
                         inventory::RoleInventory &inv,
                         equipment::RoleEquipment &eq, uint32_t unitId,
                         uint8_t slot);

} // namespace equip
