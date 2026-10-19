#pragma once
#include "game/item_cache.h"
#include "game/role_inventory.h"
#include "protocol/codec.h"
#include <cstdint>
#include <string>
#include <vector>

/**
 * @file shop_codec.h
 * @brief ショップ／バッグ／ゴールド関連の S2C メッセージのエンコード
 *
 * コアの codec.h がアイテム設定やバッグの状態に依存しないよう、ファイルを分けている。
 */
namespace codec {

/**
 * @brief 汎用の操作結果をエンコードする
 *
 * @param seq 対応するリクエストのシーケンス番号。
 * @param opType 操作の種類。クライアントはこれを見て対応する UI 処理に振り分ける。
 * @param success true なら操作が反映された。false なら拒否され、サーバーの状態は変わっていない。
 * @param message クライアントのトースト表示にそのまま使う文言。成功時も失敗理由もここに入れる。
 *
 * @return エンコード済みバイト列を持つ Envelope。
 *
 * @note
 * - success=false の場合、ゴールドとバッグがどちらも変更されていないことを呼び出し側が保証すること。
 */
inline std::string encodeOperationResult(uint32_t seq, Game::OpResultType opType,
                                         bool success,
                                         const std::string &message) {
  tlFbb.Clear();
  auto body = Game::CreateS2C_OperationResultDirect(tlFbb, opType, success,
                                                    message.c_str(), nullptr);
  return wrapEnvelope(Game::MsgType_S2C_OperationResult,
                      Game::MsgBody_S2C_OperationResult, body.Union(), seq,
                      nowServerTimeMs());
}

/**
 * @brief 追加データ付きの汎用操作結果をエンコードする
 *
 * @param seq 対応するリクエストのシーケンス番号。
 * @param opType 操作の種類。
 * @param success 操作が反映されたかどうか。
 * @param message トースト表示用の文言。
 * @param data 追加の JSON。解釈方法はクライアントが op_type に応じて決める。
 *
 * @return エンコード済みバイト列を持つ Envelope。
 */
inline std::string encodeOperationResultWithData(uint32_t seq,
                                                 Game::OpResultType opType,
                                                 bool success,
                                                 const std::string &message,
                                                 const std::string &data) {
  tlFbb.Clear();
  auto body = Game::CreateS2C_OperationResultDirect(
      tlFbb, opType, success, message.c_str(), data.c_str());
  return wrapEnvelope(Game::MsgType_S2C_OperationResult,
                      Game::MsgBody_S2C_OperationResult, body.Union(), seq,
                      nowServerTimeMs());
}

/**
 * @brief ゴールドが変動した理由
 *
 * @note
 * - S2C_GoldChanged.reason に対応し、クライアントはこれで通知文言を出し分ける。
 *   取りうる値はサーバー側で定義するので、理由を追加したらフロントエンドにも必ず共有すること。
 */
namespace gold_reason {
inline constexpr const char *kShopBuy = "buy";
inline constexpr const char *kShopSell = "sell";
inline constexpr const char *kBuyBuilding = "buy_building";
inline constexpr const char *kRecruit = "recruit";
inline constexpr const char *kKillReward = "reward";
} // namespace gold_reason

/**
 * @brief ゴールド変動の通知をエンコードする
 *
 * @param seq 対応するリクエストのシーケンス番号。
 * @param oldGold 変動前のゴールド。
 * @param newGold 変動後の正となるゴールド（差分ではない）。
 * @param reason 変動理由。値は gold_reason を参照。呼び出し中だけ借用する。
 *
 * @return エンコード済みバイト列を持つ Envelope。
 *
 * @note
 * - フィールドの順序は old_gold が先、new_gold が後。逆に渡さないこと。
 */
inline std::string encodeGoldChanged(uint32_t seq, uint32_t oldGold,
                                     uint32_t newGold, const char *reason) {
  tlFbb.Clear();
  auto body = Game::CreateS2C_GoldChangedDirect(tlFbb, oldGold, newGold, reason);
  return wrapEnvelope(Game::MsgType_S2C_GoldChanged,
                      Game::MsgBody_S2C_GoldChanged, body.Union(), seq,
                      nowServerTimeMs());
}

/**
 * @brief バッグの全件リストをエンコードする
 *
 * @param seq 対応するリクエストのシーケンス番号。
 * @param items アイテム設定のキャッシュ。呼び出し中だけ借用する。
 * @param inv キャラクターのバッグの状態。読み取りのみ。
 *
 * @return エンコード済みバイト列を持つ Envelope。
 *
 * @note
 * - バッグは常に全件送信し、クライアントはローカルのリストをそのまま置き換える。
 * - 設定に存在しないアイテムはスキップし、不完全な要素は送らない。
 */
inline std::string encodeBackpackChanged(uint32_t seq,
                                         const cache::ItemCache &items,
                                         const inventory::RoleInventory &inv) {
  tlFbb.Clear();
  std::vector<flatbuffers::Offset<Game::BackpackItem>> offsets;
  offsets.reserve(inv.stacks.size());

  for (const auto &stack : inv.stacks) {
    const cache::ItemTemplate *const t = cache::findItem(items, stack.itemId);
    if (!t) continue;
    offsets.push_back(Game::CreateBackpackItemDirect(
        tlFbb, t->itemId, t->itemName.c_str(), t->itemType, t->subType,
        t->attrValue, t->rarity, stack.count, t->description.c_str()));
  }

  auto itemsVec = tlFbb.CreateVector(offsets);
  auto body = Game::CreateS2C_BackpackChanged(tlFbb, itemsVec);
  return wrapEnvelope(Game::MsgType_S2C_BackpackChanged,
                      Game::MsgBody_S2C_BackpackChanged, body.Union(), seq,
                      nowServerTimeMs());
}

/**
 * @brief ショップの全カテゴリの商品をエンコードする
 *
 * @param seq 対応するリクエストのシーケンス番号。
 * @param items アイテムとショップの設定キャッシュ。呼び出し中だけ借用する。
 * @param inv キャラクターのバッグの状態。各商品の owned_count を埋めるのに使う。
 * @param playerGold 現在のゴールド。クライアント上部の表示に使う。
 *
 * @return エンコード済みバイト列を持つ Envelope。
 *
 * @note
 * - リクエストの category に関係なく全カテゴリを一度に送る。タブの切り替えはクライアント側で行う。
 * - カテゴリ内の並び順は shop_items.sort_order の順。
 */
inline std::string encodeShopList(uint32_t seq, const cache::ItemCache &items,
                                  const inventory::RoleInventory &inv,
                                  uint32_t playerGold) {
  tlFbb.Clear();
  std::vector<flatbuffers::Offset<Game::ShopCategory>> categoryOffsets;
  categoryOffsets.reserve(cache::kMaxShopCategory);

  for (uint8_t category = cache::kMinShopCategory;
       category <= cache::kMaxShopCategory; ++category) {
    const auto &shopIds = items.shopIdsByCategory[category];

    std::vector<flatbuffers::Offset<Game::ShopItem>> itemOffsets;
    itemOffsets.reserve(shopIds.size());

    for (const uint32_t shopItemId : shopIds) {
      const cache::ShopEntry *const e = cache::findShopEntry(items, shopItemId);
      if (!e) continue;
      const cache::ItemTemplate *const t = cache::findItem(items, e->itemId);
      if (!t) continue;

      itemOffsets.push_back(Game::CreateShopItemDirect(
          tlFbb, e->shopItemId, t->itemId, t->itemName.c_str(), t->itemType,
          t->subType, t->attrValue, t->rarity, t->description.c_str(),
          e->price, e->sellPrice, e->stock,
          inventory::countOf(inv, t->itemId)));
    }

    categoryOffsets.push_back(Game::CreateShopCategoryDirect(
        tlFbb, category, cache::kShopCategoryNames[category], &itemOffsets));
  }

  auto categoriesVec = tlFbb.CreateVector(categoryOffsets);
  auto body = Game::CreateS2C_ShopList(tlFbb, categoriesVec, playerGold);
  return wrapEnvelope(Game::MsgType_S2C_ShopList, Game::MsgBody_S2C_ShopList,
                      body.Union(), seq, nowServerTimeMs());
}

} // namespace codec
