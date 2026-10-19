#pragma once
#include "game/item_cache.h"
#include "game/role_equipment.h"
#include <cstdint>

/**
 * @file equip_stats.h
 * @brief 装備を戦闘ステータスに換算する
 *
 * 換算ルールはフロントエンドの表示文言と厳密に一致させている（SHOP_FRONTEND_GUIDE 3.2 節）。
 *   武器(sub_type=1)       → 攻撃 +attr_value
 *   防具(sub_type=2)       → HP +attr_value
 *   アクセサリ(sub_type=3) → 攻撃 +attr_value かつ HP +attr_value
 * フロントエンドも同じルールで文言を描画しているため、片方を変えたら必ずもう片方も合わせること。
 * ずれると、プレイヤーに見えるボーナスと実際の戦闘数値が食い違う。
 */
namespace equip {

/**
 * @brief 1 体の精霊が装備から得るステータスボーナス
 *
 * @note
 * - ボーナスは倍率ではなく加算値。最終値は base + bonus で求める。
 */
struct StatBonus {
  uint32_t hp = 0;
  uint32_t atk = 0;
};

/**
 * @brief 装備 1 つ分のボーナスを加算する
 *
 * @param[in,out] out 加算先。
 * @param tpl 借用するアイテム設定。装備でない、または attr_value <= 0 の場合は何も加算しない。
 *
 * @note
 * - attr_value が負の設定はステータスを減らすのではなく無視する。
 *   企画側の入力ミスでユニットの HP がマイナスになるのを防ぐため。
 */
inline void accumulateBonus(StatBonus &out, const cache::ItemTemplate &tpl) {
  if (tpl.itemType != static_cast<uint8_t>(cache::ItemType::Equipment)) return;
  if (tpl.attrValue <= 0) return;
  const uint32_t value = static_cast<uint32_t>(tpl.attrValue);

  switch (static_cast<cache::EquipSlot>(tpl.subType)) {
  case cache::EquipSlot::Weapon:
    out.atk += value;
    break;
  case cache::EquipSlot::Armor:
    out.hp += value;
    break;
  case cache::EquipSlot::Accessory:
    out.atk += value;
    out.hp += value;
    break;
  case cache::EquipSlot::None:
    break;
  }
}

/**
 * @brief ある精霊の全スロットの装備ボーナスを合計する
 *
 * @param items 読み取り専用のアイテム設定。
 * @param eq 借用するキャラクターの装備状態。
 * @param unitId 精霊のテンプレート ID。
 *
 * @return その精霊のステータスボーナス。装備がなければすべて 0。
 *
 * @note
 * - 設定に存在しないアイテムはスキップし、ボーナスに含めない。
 */
inline StatBonus bonusForUnit(const cache::ItemCache &items,
                              const equipment::RoleEquipment &eq,
                              uint32_t unitId) {
  StatBonus out;
  for (const auto &e : eq.entries) {
    if (e.unitId != unitId) continue;
    const cache::ItemTemplate *const tpl = cache::findItem(items, e.itemId);
    if (!tpl) continue;
    accumulateBonus(out, *tpl);
  }
  return out;
}

} // namespace equip
