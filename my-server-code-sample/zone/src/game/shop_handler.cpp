#include "game/shop_handler.h"
#include <limits>

namespace shop {
namespace {

/**
 * @brief 失敗結果を作る
 *
 * @note
 * - 失敗時は、状態を変更する前に必ず return すること。呼び出し側に見える状態をきれいに保つため。
 */
TradeOutcome fail(std::string message) {
  TradeOutcome out;
  out.success = false;
  out.message = std::move(message);
  return out;
}

} // namespace

/**
 * @brief 購入処理
 *
 * @note
 * - 検証の順序は固定：数量 → 商品の存在 → アイテム設定の存在 → 在庫 → 合計金額 → ゴールド → スタック上限。
 *   どの段階で失敗しても、状態を変更する前に return する。
 */
TradeOutcome buy(const cache::ItemCache &items, inventory::RoleInventory &inv,
                 uint32_t &gold, uint32_t shopItemId, uint32_t count) {
  if (count == 0) {
    return fail("購入数は 1 以上にしてください");
  }
  if (count > kMaxTradeCountPerRequest) {
    return fail("一度に購入できる数の上限は " +
                std::to_string(kMaxTradeCountPerRequest));
  }

  const cache::ShopEntry *const entry = cache::findShopEntry(items, shopItemId);
  if (!entry) {
    return fail("商品が存在しないか、販売が終了しています");
  }
  const cache::ItemTemplate *const tpl = cache::findItem(items, entry->itemId);
  if (!tpl) {
    return fail("商品の設定に異常があります");
  }

    // stock < 0 は在庫無制限を表す。
    // TODO: 在庫に限りのある商品を扱う場合は、実行時に変化する在庫を別に持ち、購入成功時に減らす必要がある。
    //       現在の shop_items はすべて -1 で設定されており、実行時の在庫状態は持っていない。
  if (entry->stock >= 0 && count > static_cast<uint32_t>(entry->stock)) {
    return fail("在庫が足りません！残り " + std::to_string(entry->stock) + " 個");
  }

    // 合計金額は 64 ビットで計算する。price は最大 int32、count は最大 uint32 なので
    // 積が uint32 を超えうる。広い型で計算してからゴールドと比較すること。
  const uint64_t unitPrice = static_cast<uint64_t>(entry->price);
  const uint64_t totalPrice = unitPrice * static_cast<uint64_t>(count);
  if (totalPrice > static_cast<uint64_t>(gold)) {
    return fail("ゴールドが足りません（必要 " + std::to_string(totalPrice) +
                "、所持 " + std::to_string(gold) + "）");
  }

    // 先にバッグへの追加を試算し、「ゴールドは減ったのにアイテムが入らない」中間状態を防ぐ。
  uint32_t newItemCount = 0;
  if (!inventory::addItems(inv, tpl->itemId, count, newItemCount)) {
    return fail("バッグ内のこのアイテムの数が上限に達しています");
  }

  gold -= static_cast<uint32_t>(totalPrice);

  TradeOutcome out;
  out.success      = true;
  out.itemId       = tpl->itemId;
  out.newItemCount = newItemCount;
  out.newGold      = gold;
  out.message      = "購入しました！「" + tpl->itemName + "」x" +
                     std::to_string(count);
  return out;
}

/**
 * @brief 売却処理
 *
 * @note
 * - 検証の順序は固定：数量 → アイテム設定の存在 → 売値が設定されている → 所持数 → ゴールド上限。
 */
TradeOutcome sell(const cache::ItemCache &items, inventory::RoleInventory &inv,
                  uint32_t &gold, uint32_t itemId, uint32_t count) {
  if (count == 0) {
    return fail("売却数は 1 以上にしてください");
  }
  if (count > kMaxTradeCountPerRequest) {
    return fail("一度に売却できる数の上限は " +
                std::to_string(kMaxTradeCountPerRequest));
  }

  const cache::ItemTemplate *const tpl = cache::findItem(items, itemId);
  if (!tpl) {
    return fail("アイテムが存在しません");
  }
  const cache::ShopEntry *const entry =
      cache::findShopEntryByItemId(items, itemId);
  if (!entry) {
    return fail("「" + tpl->itemName + "」は売却できません");
  }

  const uint32_t owned = inventory::countOf(inv, itemId);
  if (owned < count) {
    return fail("数が足りません！所持 " + std::to_string(owned) + " 個");
  }

  const uint64_t totalGain =
      static_cast<uint64_t>(entry->sellPrice) * static_cast<uint64_t>(count);
  const uint64_t goldAfter = static_cast<uint64_t>(gold) + totalGain;
    // roles.gold も通信上の値も uint32 なので、上限を超える場合は取引全体を拒否する。
    // 切り捨ては行わない（切り捨て＝プレイヤーの資産を勝手に消すことになる）。
  if (goldAfter > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
    return fail("ゴールドが上限に達しているため、これ以上売却できません");
  }

  uint32_t newItemCount = 0;
  if (!inventory::removeItems(inv, itemId, count, newItemCount)) {
    return fail("数が足りません！所持 " + std::to_string(owned) + " 個");
  }

  gold = static_cast<uint32_t>(goldAfter);

  TradeOutcome out;
  out.success      = true;
  out.itemId       = itemId;
  out.newItemCount = newItemCount;
  out.newGold      = gold;
  out.message      = "売却しました！「" + tpl->itemName + "」x" +
                     std::to_string(count) + "、ゴールド " +
                     std::to_string(totalGain) + " を獲得";
  return out;
}

} // namespace shop
