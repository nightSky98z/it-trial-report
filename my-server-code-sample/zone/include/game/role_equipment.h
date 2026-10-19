#pragma once
#include <cstdint>
#include <vector>

/**
 * @file role_equipment.h
 * @brief キャラクターが所有する全精霊の装備スロット（実行時の状態）
 *
 * DB の unit_equips に対応する。(role_id, unit_id, slot) は一意で、1 体の精霊の
 * 1 スロットにつき装備は最大 1 つ。ログイン時にまとめて GameSession に読み込み、
 * 実行中はメモリ上だけを更新して、変更分を非同期で DB に書き込む。
 * 1 キャラクターあたりの精霊は 1 桁、スロットは 3 つ固定なので、連続した vector と線形探索で十分。
 */
namespace equipment {

/**
 * @brief 使用中の装備スロット 1 つ
 *
 * @note
 * - slot の値は items.sub_type と同じ（1=武器 2=防具 3=アクセサリ）。
 * - コンテナに itemId == 0 の要素は入らない。装備を外したら要素ごと削除する。
 */
struct EquipSlotEntry {
  uint32_t unitId = 0;
  uint8_t  slot = 0;
  uint32_t itemId = 0;
};

/**
 * @brief キャラクターの全装備スロットの状態
 *
 * @note
 * - GameSession が所有する。読み書きは、その接続を担当するゲームワーカーのスレッドからのみ行う。
 * - entries 内で (unitId, slot) は一意。
 */
struct RoleEquipment {
  std::vector<EquipSlotEntry> entries;
};

/**
 * @brief 指定した精霊の指定スロットに装備中のアイテムを取得する
 *
 * @param eq 借用する装備状態。
 * @param unitId 精霊のテンプレート ID。
 * @param slot スロット。
 *
 * @return そのスロットの items.item_id。
 * @retval 0 スロットが空。
 */
inline uint32_t equippedItem(const RoleEquipment &eq, uint32_t unitId,
                             uint8_t slot) {
  for (const auto &e : eq.entries) {
    if (e.unitId == unitId && e.slot == slot) return e.itemId;
  }
  return 0;
}

/**
 * @brief スロットにアイテムを設定する（上書き）
 *
 * @param[in,out] eq 装備状態。
 * @param unitId 精霊のテンプレート ID。
 * @param slot スロット。
 * @param itemId 装備するアイテム。0 以外であること。
 *
 * @note
 * - 古い装備をバッグに戻す処理は呼び出し側で先に済ませること。本関数はスロットの状態を書き換えるだけ。
 */
inline void setSlot(RoleEquipment &eq, uint32_t unitId, uint8_t slot,
                    uint32_t itemId) {
  for (auto &e : eq.entries) {
    if (e.unitId == unitId && e.slot == slot) {
      e.itemId = itemId;
      return;
    }
  }
  eq.entries.push_back(EquipSlotEntry{unitId, slot, itemId});
}

/**
 * @brief スロットを空にする
 *
 * @param[in,out] eq 装備状態。
 * @param unitId 精霊のテンプレート ID。
 * @param slot スロット。
 *
 * @retval false もともと空のスロットだった。呼び出し側は失敗として扱うこと。
 */
inline bool clearSlot(RoleEquipment &eq, uint32_t unitId, uint8_t slot) {
  for (std::size_t i = 0; i < eq.entries.size(); ++i) {
    const auto &e = eq.entries[i];
    if (e.unitId != unitId || e.slot != slot) continue;
    eq.entries.erase(eq.entries.begin() + static_cast<std::ptrdiff_t>(i));
    return true;
  }
  return false;
}

} // namespace equipment
