#include "game/equip_handler.h"

namespace equip {
namespace {

/**
 * @brief 失敗結果を作る
 *
 * @note
 * - 失敗時は、状態を変更する前に必ず return すること。
 */
EquipOutcome fail(std::string message) {
  EquipOutcome out;
  out.success = false;
  out.message = std::move(message);
  return out;
}

} // namespace

/**
 * @brief 装備処理
 *
 * @note
 * - 検証の順序：スロットが正しい → アイテムが存在する → 装備品である → サブタイプとスロットが一致
 *   → バッグに所持している。
 *   すべて通ってから状態を変更する。付け替え時の 2 回のバッグ操作の間で失敗することはない。
 *   古い装備を先に戻し、新しい装備を後から減らすが、減らす側の前提条件（所持数）は事前に検証済み。
 */
EquipOutcome equipItem(const cache::ItemCache &items,
                       inventory::RoleInventory &inv,
                       equipment::RoleEquipment &eq, uint32_t unitId,
                       uint32_t itemId, uint8_t slot) {
  if (unitId == 0) {
    return fail("精霊が存在しません");
  }
  if (slot < cache::kMinEquipSlot || slot > cache::kMaxEquipSlot) {
    return fail("装備スロットが不正です");
  }

  const cache::ItemTemplate *const tpl = cache::findItem(items, itemId);
  if (!tpl) {
    return fail("アイテムが存在しません");
  }
  if (tpl->itemType != static_cast<uint8_t>(cache::ItemType::Equipment)) {
    return fail("「" + tpl->itemName + "」は装備品ではありません");
  }
  if (tpl->subType != slot) {
    return fail("「" + tpl->itemName + "」はこのスロットに装備できません");
  }
  if (inventory::countOf(inv, itemId) == 0) {
    return fail("バッグに「" + tpl->itemName + "」がありません");
  }

  const uint32_t oldItemId = equipment::equippedItem(eq, unitId, slot);
  if (oldItemId == itemId) {
    return fail("「" + tpl->itemName + "」は既にこのスロットに装備されています");
  }

  EquipOutcome out;

    // 先に古い装備をバッグに戻す。この操作はバッグの数を増やすだけなので、
    // 失敗するのはスタック上限を超えたときだけ。その場合は操作全体を拒否し、古い装備が消えるのを防ぐ。
  if (oldItemId != 0) {
    uint32_t oldNewCount = 0;
    if (!inventory::addItems(inv, oldItemId, 1, oldNewCount)) {
      return fail("バッグがいっぱいのため、現在の装備を外せません");
    }
    out.itemChanges.push_back(ItemCountChange{oldItemId, oldNewCount});
  }

  uint32_t newCount = 0;
  if (!inventory::removeItems(inv, itemId, 1, newCount)) {
        // 所持数は事前に検証済みなので、ここに来るのはメモリ上の状態と検証結果が食い違った場合だけ。
        // 直前に戻した分をロールバックし、失敗時に状態が変わらないことを保証する。
    if (oldItemId != 0) {
      uint32_t rollback = 0;
      inventory::removeItems(inv, oldItemId, 1, rollback);
      out.itemChanges.clear();
    }
    return fail("バッグに「" + tpl->itemName + "」がありません");
  }
  out.itemChanges.push_back(ItemCountChange{itemId, newCount});

  equipment::setSlot(eq, unitId, slot, itemId);

  out.success        = true;
  out.unitId         = unitId;
  out.slot           = slot;
  out.equippedItemId = itemId;
  out.message        = "「" + tpl->itemName + "」を装備しました";
  return out;
}

/**
 * @brief 装備解除処理
 *
 * @note
 * - バッグに入りきらない場合は操作全体を拒否し、装備は付けたままにする。アイテムを消失させない。
 */
EquipOutcome unequipItem(const cache::ItemCache &items,
                         inventory::RoleInventory &inv,
                         equipment::RoleEquipment &eq, uint32_t unitId,
                         uint8_t slot) {
  if (unitId == 0) {
    return fail("精霊が存在しません");
  }
  if (slot < cache::kMinEquipSlot || slot > cache::kMaxEquipSlot) {
    return fail("装備スロットが不正です");
  }

  const uint32_t itemId = equipment::equippedItem(eq, unitId, slot);
  if (itemId == 0) {
    return fail("このスロットには何も装備されていません");
  }

  uint32_t newCount = 0;
  if (!inventory::addItems(inv, itemId, 1, newCount)) {
    return fail("バッグがいっぱいのため、装備を外せません");
  }

    // ここからは必ず成功する（スロットに装備があることは上で確認済み）。
  equipment::clearSlot(eq, unitId, slot);

  const cache::ItemTemplate *const tpl = cache::findItem(items, itemId);

  EquipOutcome out;
  out.success        = true;
  out.unitId         = unitId;
  out.slot           = slot;
  out.equippedItemId = 0;
  out.itemChanges.push_back(ItemCountChange{itemId, newCount});
  out.message = tpl ? ("「" + tpl->itemName + "」を外しました") : "装備を外しました";
  return out;
}

} // namespace equip
