#pragma once
#include <cstdint>
#include <limits>
#include <vector>

/**
 * @file role_inventory.h
 * @brief キャラクターのバッグ（実行時の正となる状態）
 *
 * バッグはログイン時に role_items からまとめて GameSession に読み込み、実行中はメモリ上だけを更新して、
 * 変更分を非同期で DB に書き込む。アイテムの種類は数十程度なので、連続した vector と線形探索で扱い、
 * ハッシュテーブルや余計な間接参照は持ち込まない。
 */
namespace inventory {

/**
 * @brief アイテム 1 種類あたりのスタック上限
 *
 * @note
 * - この上限はゲームルールではなく、ストレージとプロトコルの制約から来ている。
 *   role_items.count は MySQL の INT（符号付き 32 ビット）、通信上は uint32。
 *   両者の共通範囲である int32 の上限を採用し、メモリ上の値が必ず欠損なく DB 保存・エンコードできるようにしている。
 */
inline constexpr uint32_t kMaxStackCount =
    static_cast<uint32_t>(std::numeric_limits<int32_t>::max());

/**
 * @brief バッグ内のアイテム 1 種類とその数量
 *
 * @note
 * - itemId は items.item_id に対応する。count が 0 のスタックはコンテナに残さない。
 */
struct ItemStack {
  uint32_t itemId = 0;
  uint32_t count = 0;
};

/**
 * @brief キャラクターのバッグの実行時状態
 *
 * @note
 * - GameSession が所有する。読み書きは、その接続を担当するゲームワーカーのスレッドからのみ行う。
 * - stacks 内で itemId が重複することはなく、count == 0 の要素もない。
 * - 並び順はログイン時の読み込み順とその後の追加順で、クライアントへの送信時も安定している。
 */
struct RoleInventory {
  std::vector<ItemStack> stacks;
};

/**
 * @brief アイテムのスタックを検索する
 *
 * @param inv 借用するバッグの状態。
 * @param itemId items.item_id。
 *
 * @retval nullptr バッグにそのアイテムがない。
 *
 * @note
 * - 返すポインタは inv 内部の要素を指す。追加・削除を行うと無効になるので、呼び出しをまたいで保持しないこと。
 */
inline const ItemStack *findStack(const RoleInventory &inv, uint32_t itemId) {
  for (const auto &s : inv.stacks) {
    if (s.itemId == itemId) return &s;
  }
  return nullptr;
}

/**
 * @brief アイテムの所持数を取得する
 *
 * @param inv 借用するバッグの状態。
 * @param itemId items.item_id。
 *
 * @return 所持数。持っていなければ 0。
 */
inline uint32_t countOf(const RoleInventory &inv, uint32_t itemId) {
  const ItemStack *const s = findStack(inv, itemId);
  return s ? s->count : 0;
}

/**
 * @brief アイテムを追加する
 *
 * @param[in,out] inv バッグの状態。成功時は更新され、失敗時は元のまま。
 * @param itemId items.item_id。そのアイテムが設定に存在することを呼び出し側で確認しておくこと。
 * @param count 追加する数。1 以上であること。
 * @param[out] outNewCount 成功時、そのアイテムの変更後の合計数を書き込む。
 *
 * @retval false count が 0、または追加すると kMaxStackCount を超える。この場合 inv は変更されない。
 *
 * @note
 * - false が返ったら、呼び出し側は取引全体を拒否すること。一部だけ反映してはならない。
 */
inline bool addItems(RoleInventory &inv, uint32_t itemId, uint32_t count,
                     uint32_t &outNewCount) {
  if (count == 0) return false;

  for (auto &s : inv.stacks) {
    if (s.itemId != itemId) continue;
    if (count > kMaxStackCount - s.count) return false;  // 加算する前にオーバーフローを判定する
    s.count += count;
    outNewCount = s.count;
    return true;
  }

  if (count > kMaxStackCount) return false;
  inv.stacks.push_back(ItemStack{itemId, count});
  outNewCount = count;
  return true;
}

/**
 * @brief アイテムを減らす
 *
 * @param[in,out] inv バッグの状態。成功時は更新され、失敗時は元のまま。
 * @param itemId items.item_id。
 * @param count 減らす数。1 以上であること。
 * @param[out] outNewCount 成功時、そのアイテムの変更後の残り数を書き込む。なくなった場合は 0。
 *
 * @retval false count が 0、または所持数が足りない。この場合 inv は変更されない。
 *
 * @note
 * - 使い切るとそのアイテムのスタックは stacks から削除されるが、永続化では count=0 を DB に書き戻す必要がある。
 *   そのため呼び出し側はバッグを再検索せず、outNewCount を使うこと。
 */
inline bool removeItems(RoleInventory &inv, uint32_t itemId, uint32_t count,
                        uint32_t &outNewCount) {
  if (count == 0) return false;

  for (std::size_t i = 0; i < inv.stacks.size(); ++i) {
    auto &s = inv.stacks[i];
    if (s.itemId != itemId) continue;
    if (s.count < count) return false;

    s.count -= count;
    outNewCount = s.count;
    if (s.count == 0) {
      inv.stacks.erase(inv.stacks.begin() + static_cast<std::ptrdiff_t>(i));
    }
    return true;
  }
  return false;
}

} // namespace inventory
