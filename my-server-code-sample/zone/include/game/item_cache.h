#pragma once
#include "config/zone_config.h"
#include "db/mysql_x_session.h"
#include "utils/log.h"
#include "utils/safe_utils.h"
#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

/**
 * @file item_cache.h
 * @brief items / shop_items の読み取り専用設定キャッシュ
 *
 * TemplateCache とは別に管理する。アイテムとショップは純粋に経済システムの設定であり、
 * マップのインスタンス化やモンスターのスポーンには関わらないので、マップテンプレートと同じオブジェクトにまとめる理由がない。
 */
namespace cache {

/**
 * @brief アイテムの種類（items.item_type）
 *
 * @note
 * - 値は DB のコメントで固定されており、通信上の ShopItem.item_type / BackpackItem.item_type の値でもある。
 */
enum class ItemType : uint8_t {
  Consumable = 1,  // 消耗品
  Equipment  = 2,  // 装備
  Material   = 3   // 素材
};

/**
 * @brief 装備のサブタイプ（items.sub_type）。item_type=Equipment のときだけ有効
 *
 * @note
 * - 値はそのまま装備スロット番号でもある。武器は 1 番、防具は 2 番、アクセサリは 3 番のスロットに入る。
 * - unit_equips.slot は装備したアイテムの sub_type と等しくなければならない。装備処理で必ず検証すること。
 */
enum class EquipSlot : uint8_t {
  None     = 0,
  Weapon   = 1,   // 攻撃力が上がる
  Armor    = 2,   // HP が上がる
  Accessory = 3   // 攻撃力と HP がそれぞれ attr_value 上がる
};

/// 装備スロットの有効範囲（両端を含む）。受信したパラメータの検証に使う。
inline constexpr uint8_t kMinEquipSlot = 1;
inline constexpr uint8_t kMaxEquipSlot = 3;

/// ショップのカテゴリ（shop_items.category）の有効範囲。両端を含む。
inline constexpr uint8_t kMinShopCategory = 1;
inline constexpr uint8_t kMaxShopCategory = 5;

/**
 * @brief ショップのカテゴリの表示名
 *
 * @note
 * - shop_items テーブルにはカテゴリ名の列がない。カテゴリ名はプロトコルの表示用フィールドで、サーバー側で固定値を提供する。
 * - 添字 0 は未使用。添字と category の値をそのまま対応させ、呼び出し側で -1 する必要をなくしている。
 */
inline constexpr std::array<const char *, kMaxShopCategory + 1>
    kShopCategoryNames = {"", "武器", "防具", "アクセサリ", "消耗品", "素材"};

static_assert(kShopCategoryNames.size() == kMaxShopCategory + 1,
              "カテゴリ名の表は 1..kMaxShopCategory をすべて含み、添字が category と一致していること");

/**
 * @brief items テーブル 1 行分のキャッシュ（アイテムの静的な定義）
 *
 * @note
 * - 文字列はキャッシュオブジェクトが所有する。読み込み後、実行中は読み取り専用。
 * - itemId は DB の items.item_id に対応し、バッグや装備テーブルから参照されるキー。
 */
struct ItemTemplate {
  uint32_t    itemId = 0;
  std::string itemName;
  uint8_t     itemType = 0;   // ItemType
  uint8_t     subType = 0;    // EquipSlot。装備以外は 0
  int32_t     attrValue = 0;
  uint8_t     rarity = 1;     // 1=ノーマル 2=レア 3=エピック 4=レジェンド
  std::string description;
};

/**
 * @brief shop_items テーブル 1 行分のキャッシュ（ショップに並ぶ商品）
 *
 * @note
 * - shopItemId は shop_items.id に対応し、C2S_ShopBuy のキーになる。
 * - itemId は items.item_id に対応し、C2S_ShopSell とバッグのキーになる。両者は意味が異なるので、取り違えないこと。
 * - stock = -1 は在庫無制限。現在の設定はすべて -1 なので、実行時の在庫の減算は行わない。
 */
struct ShopEntry {
  uint32_t shopItemId = 0;
  uint32_t itemId = 0;
  uint8_t  category = 0;
  int32_t  price = 0;
  int32_t  sellPrice = 0;
  int32_t  stock = -1;
  int32_t  sortOrder = 0;
};

/**
 * @brief 起動時に読み込むアイテム／ショップの読み取り専用設定
 *
 * @note
 * - ゲームワーカーが所有する。読み込み後、実行中は読み取り専用で、複数のセッションから並行して読める。
 * - shopIdsByCategory は sort_order 順に並んでおり、送信順がそのままフロントエンドの表示順になる。
 */
struct ItemCache {
  std::unordered_map<uint32_t, ItemTemplate> items;      // itemId -> アイテム定義
  std::unordered_map<uint32_t, ShopEntry>    shopById;   // shop_items.id -> ショップの設定
  std::unordered_map<uint32_t, uint32_t>     shopIdByItemId;  // itemId -> shop_items.id

    // 添字が category（0 は未使用）。各 vector はそのカテゴリの shop_items.id を sort_order の昇順で持つ。
  std::array<std::vector<uint32_t>, kMaxShopCategory + 1> shopIdsByCategory;
};

/**
 * @brief アイテムの定義を検索する
 *
 * @param c 読み取り専用の設定キャッシュ。返すポインタはキャッシュ内部を指すので、キャッシュの寿命を超えて保持しないこと。
 * @param itemId items.item_id。
 *
 * @retval nullptr itemId が設定されていない。呼び出し側は不正なリクエストとして拒否すること。
 */
inline const ItemTemplate *findItem(const ItemCache &c, uint32_t itemId) {
  const auto it = c.items.find(itemId);
  return it == c.items.end() ? nullptr : &it->second;
}

/**
 * @brief ショップの商品を検索する
 *
 * @param c 読み取り専用の設定キャッシュ。返すポインタはキャッシュ内部を指す。
 * @param shopItemId shop_items.id。
 *
 * @retval nullptr 商品が存在しない、または販売終了（enabled=0 の行はキャッシュに入らない）。
 */
inline const ShopEntry *findShopEntry(const ItemCache &c, uint32_t shopItemId) {
  const auto it = c.shopById.find(shopItemId);
  return it == c.shopById.end() ? nullptr : &it->second;
}

/**
 * @brief アイテム ID からショップの設定を逆引きする（売却価格の決定に使う）
 *
 * @param c 読み取り専用の設定キャッシュ。返すポインタはキャッシュ内部を指す。
 * @param itemId items.item_id。
 *
 * @retval nullptr そのアイテムは現在ショップに並んでいない。呼び出し側は売却を拒否すること（正となる売値がないため）。
 */
inline const ShopEntry *findShopEntryByItemId(const ItemCache &c,
                                              uint32_t itemId) {
  const auto it = c.shopIdByItemId.find(itemId);
  if (it == c.shopIdByItemId.end()) return nullptr;
  return findShopEntry(c, it->second);
}

/**
 * @brief DB から items / shop_items の設定を読み込む
 *
 * @param cfg zone 設定。接続パラメータは呼び出し中だけ借用する。
 * @param out 呼び出し側が所有するキャッシュ。内容は上書きされる。
 *
 * @retval true 読み込みが完了し、すべての shop_items の item_id の参照先が存在する。
 * @retval false DB の検索に失敗した、または設定の参照が不正。呼び出し側は起動処理を中止すること。
 *
 * @note
 * - 起動時に 1 回呼ぶ。実行中に同じ out をその場でリロードしないこと（ホットリロードは新しいスナップショットに読み込む）。
 * - enabled=0 の商品はキャッシュに入らず、存在しないのと同じ扱いになる。
 */
inline bool loadItems(const zone::ZoneConfig &cfg, ItemCache &out) {
  try {
    auto sess = db::makeSession(cfg);

        // このローカル変数を db という名前にしないこと。db:: 名前空間を隠してしまう。
    const std::string dbName = cfg.mysqlDb;

        // ── items（アイテムの静的な定義）──────────────────────────────────
    {
      auto res = sess->sql(
          "SELECT item_id, item_name, item_type, COALESCE(sub_type, 0), "
          "       COALESCE(attr_value, 0), COALESCE(rarity, 1), "
          "       COALESCE(description, '') "
          "FROM `" + dbName + "`.items")
          .execute();

      for (auto row : res) {
        ItemTemplate t;
        t.itemId      = utils::safeToU32(row[0]);
        t.itemName    = utils::safeToStr(row[1]);
        t.itemType    = static_cast<uint8_t>(utils::safeToU32(row[2]));
        t.subType     = static_cast<uint8_t>(utils::safeToU32(row[3], 0));
        t.attrValue   = static_cast<int32_t>(utils::safeToI32(row[4], 0));
        t.rarity      = static_cast<uint8_t>(utils::safeToU32(row[5], 1));
        t.description = utils::safeToStr(row[6]);

        if (t.itemId == 0) {
          zone_log::error("item_cache invalid items.item_id=0");
          return false;
        }
                // 装備には有効なスロットが必要。そうでないと装備処理がどのスロットに入れるか決められない。
        if (t.itemType == static_cast<uint8_t>(ItemType::Equipment) &&
            (t.subType < kMinEquipSlot || t.subType > kMaxEquipSlot)) {
          zone_log::error(
              "item_cache invalid items.item_id=%u equipment sub_type=%u "
              "outside %u..%u",
              t.itemId, t.subType, kMinEquipSlot, kMaxEquipSlot);
          return false;
        }
        out.items[t.itemId] = std::move(t);
      }
      zone_log::info("cache loaded %zu items", out.items.size());
    }

        // ── shop_items（ショップに並ぶ商品）─────────────────────────────────
    {
      auto res = sess->sql(
          "SELECT id, item_id, category, price, sell_price, "
          "       COALESCE(stock, -1), COALESCE(sort_order, 0) "
          "FROM `" + dbName + "`.shop_items "
          "WHERE COALESCE(enabled, 1) = 1 "
          "ORDER BY category, sort_order, id")
          .execute();

      for (auto row : res) {
        ShopEntry e;
        e.shopItemId = utils::safeToU32(row[0]);
        e.itemId     = utils::safeToU32(row[1]);
        e.category   = static_cast<uint8_t>(utils::safeToU32(row[2]));
        e.price      = static_cast<int32_t>(utils::safeToI32(row[3], 0));
        e.sellPrice  = static_cast<int32_t>(utils::safeToI32(row[4], 0));
        e.stock      = static_cast<int32_t>(utils::safeToI32(row[5], -1));
        e.sortOrder  = static_cast<int32_t>(utils::safeToI32(row[6], 0));

        if (e.category < kMinShopCategory || e.category > kMaxShopCategory) {
          zone_log::error(
              "item_cache invalid shop_items.id=%u category=%u outside %u..%u",
              e.shopItemId, e.category, kMinShopCategory, kMaxShopCategory);
          return false;
        }
        if (out.items.find(e.itemId) == out.items.end()) {
          zone_log::error(
              "item_cache shop_items.id=%u references missing items.item_id=%u",
              e.shopItemId, e.itemId);
          return false;
        }
        if (e.price < 0 || e.sellPrice < 0) {
          zone_log::error(
              "item_cache invalid shop_items.id=%u price=%d sell_price=%d "
              "(negative)",
              e.shopItemId, e.price, e.sellPrice);
          return false;
        }
                // 売却時は item_id から売値を逆引きするため、同じアイテムが複数並んでいると売値が曖昧になる。
        if (out.shopIdByItemId.find(e.itemId) != out.shopIdByItemId.end()) {
          zone_log::error(
              "item_cache duplicate shop entry for items.item_id=%u "
              "(shop_items.id=%u); sell price would be ambiguous",
              e.itemId, e.shopItemId);
          return false;
        }

        out.shopIdByItemId[e.itemId] = e.shopItemId;
        out.shopIdsByCategory[e.category].push_back(e.shopItemId);
        out.shopById[e.shopItemId] = e;
      }
      zone_log::info("cache loaded %zu shop_items", out.shopById.size());
    }

    return true;
  } catch (const std::exception &e) {
    zone_log::error("item_cache load failed: %s", e.what());
    return false;
  }
}

} // namespace cache
