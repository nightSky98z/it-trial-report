#pragma once
#include "game/item_cache.h"
#include "game/role_inventory.h"
#include <cstdint>
#include <string>

/**
 * @file shop_handler.h
 * @brief ショップでの売買をサーバー側で確定させる（サーバー権威）
 *
 * 売買はリクエスト／レスポンス型の経済操作で、tick ごとのシミュレーションではないため ECS のシステムにはしない。
 * 確定処理はメモリ上のゴールドとバッグだけを変更し、永続化と応答の送信は呼び出し側が行う。
 * こうすることで確定処理自体がネットワークや DB に依存せず、単体でテストできる。
 */
namespace shop {

/**
 * @brief 1 回の取引で指定できる数量の上限
 *
 * @note
 * - これはゲームルールではなく改ざん対策の境界。ゴールドや所持数のチェックでほとんどの不正値は弾けるが、
 *   クライアントは count=4294967295 のような極端な値を送って境界の計算を探ってくる可能性がある。
 *   明示的に上限を設けておけば、こうしたリクエストを金額計算の前に拒否でき、失敗理由も明確になる。
 * - 通常のまとめ買い（99 個など）はこの上限よりはるかに小さく、通常のプレイには影響しない。
 */
inline constexpr uint32_t kMaxTradeCountPerRequest = 9999;

/**
 * @brief 売買 1 回分の確定結果
 *
 * @note
 * - success=false の場合、ゴールドとバッグは変更されていないことを保証する。呼び出し側は失敗の OperationResult を 1 つ返すだけでよい。
 * - success=true の場合、呼び出し側は次の処理を必ず行うこと。
 *   1) itemId / newItemCount に従ってバッグの永続化タスクを投入する
 *   2) newGold に従ってゴールドの永続化タスクを投入する
 *   3) OperationResult + GoldChanged + BackpackChanged を送る
 * - message はそのまま表示できる文言になっている。呼び出し側で文字列を付け足さないこと。
 */
struct TradeOutcome {
  bool        success = false;
  std::string message;
  uint32_t    itemId = 0;        // 今回変化したアイテム
  uint32_t    newItemCount = 0;  // そのアイテムの変更後の数量（絶対値）。0 ならなくなった
  uint32_t    newGold = 0;       // 変更後のゴールド（絶対値）
};

/**
 * @brief 商品を購入する
 *
 * @param items 読み取り専用のアイテム／ショップ設定。
 * @param[in,out] inv キャラクターのバッグ。確定に成功したときだけ変更される。
 * @param[in,out] gold キャラクターのゴールド。確定に成功したときだけ減る。
 * @param shopItemId クライアントから渡された shop_items.id（items.item_id ではない）。
 * @param count 購入数。1 以上であること。
 *
 * @return 確定結果。失敗時は inv も gold も変更されない。
 *
 * @note
 * - 合計金額・在庫・ゴールドはすべてここで計算し直す。クライアントから送られた金額は一切信用しない。
 */
TradeOutcome buy(const cache::ItemCache &items, inventory::RoleInventory &inv,
                 uint32_t &gold, uint32_t shopItemId, uint32_t count);

/**
 * @brief バッグのアイテムを売却する
 *
 * @param items 読み取り専用のアイテム／ショップ設定。
 * @param[in,out] inv キャラクターのバッグ。確定に成功したときだけ減る。
 * @param[in,out] gold キャラクターのゴールド。確定に成功したときだけ増える。
 * @param itemId クライアントから渡された items.item_id（shop_items.id ではない）。
 * @param count 売却数。1 以上であること。
 *
 * @return 確定結果。失敗時は inv も gold も変更されない。
 *
 * @note
 * - 売値はサーバー設定の sell_price だけを使う。ショップに並んでいないアイテムは正となる売値がないため、売却を拒否する。
 */
TradeOutcome sell(const cache::ItemCache &items, inventory::RoleInventory &inv,
                  uint32_t &gold, uint32_t itemId, uint32_t count);

} // namespace shop
