#pragma once
#include "config/runtime_config.h"
#include "config/zone_config.h"
#include "game/building_cache.h"
#include "game/item_cache.h"
#include "game/template_cache.h"
#include "game/player_ship_constants.h"
#include "utils/log.h"
#include <array>
#include <concurrentqueue.h>
#include <memory>

/**
 * @file config_reload.h
 * @brief 企画用設定テーブルの実行時ホットリロード
 *
 * 設計のポイント（3 つとも欠かせない）：
 * 1. **構築してから切り替える**：新しい設定は呼び出し側のスレッドで完全に読み込み、
 *    すべての検証を通過してからゲームワーカーへ渡す。検証に失敗したら破棄し、古い設定で
 *    動き続ける。「半分だけ切り替わった」「壊れた設定に切り替わった」状態は決して作らない。
 * 2. **tick の境界でのみ切り替える**：ゲームワーカーはフレームの先頭、どのシステムも
 *    動く前にまとめて差し替える。1 フレームの間、各システムが見る設定は常に同じスナップショット。
 * 3. **まとめて置き換える**：3 つのキャッシュを 1 つのスナップショットにまとめて同時に差し替える。
 *    「アイテム表は新しいのにショップ表は古い」といったテーブル間の不整合を防ぐ。
 *
 * @note
 * - 読み込みは DB にアクセスするため呼び出したスレッドをブロックする。ゲームワーカー上では
 *   **呼ばない**こと。HTTP の管理 API があるスレッドから実行する。
 * - すでにマップ上に生成済みのエンティティはさかのぼって変更しない。モンスターのステータスは
 *   生成時にコピー済みなので、リロードが影響するのは以降に生成されるエンティティと、
 *   テーブルを参照するすべての処理（ショップ／募集／建物／ドロップ）だけ。
 */
namespace config_reload {

/**
 * @brief 読み取り専用設定のスナップショット一式
 *
 * @note
 * - 3 つのテーブルは必ず同じ読み込みで得たものを使う。どれか 1 つだけを差し替えないこと。
 */
struct ConfigSnapshot {
  cache::TemplateCache templates;
  cache::ItemCache items;
  cache::BuildingCache buildings;
    /**
     * レベル上限。DB ではなく env.zone の MAX_UNIT_LEVEL から読む。
     *
     * 3 つのテーブルと一緒に tick の境界で差し替えるため、スナップショットに含めている。
     * 上限と数値テーブルは同時に反映させないと、「上限は解放されたのに成長テーブルは古いまま」になる。
     */
  uint32_t maxUnitLevel = player_ship_constants::kDefaultMaxLevel;
};

/**
 * @brief 差し替え待ちの設定スナップショットのキュー（ゲームワーカーごとに 1 つ）
 *
 * @note
 * - 生産側は管理 API のスレッド、消費側は対応するゲームワーカー。
 * - 消費側は tick の境界でのみ取り出し、取り出せたら自分が持つ設定をまとめて置き換える。
 */
inline std::array<moodycamel::ConcurrentQueue<std::shared_ptr<ConfigSnapshot>>,
                  runtime_config::NUM_GAME_WORKERS>
    g_pending;

/**
 * @brief DB から設定スナップショットを 1 つ丸ごと読み込む
 *
 * @param cfg zone 設定。接続パラメータは呼び出し中だけ借用する。
 * @param[out] out 呼び出し側が所有するスナップショット。true を返したときだけ内容が有効。
 *
 * @retval false いずれかのテーブルの読み込みまたは検証に失敗した。out の内容は使えない。
 *
 * @note
 * - DB にアクセスしてブロックするので、ゲームワーカーのスレッドでは呼ばないこと。
 */
inline bool buildSnapshot(const zone::ZoneConfig &cfg, ConfigSnapshot &out) {
    // レベル上限は env.zone から読み直す。企画がファイルを編集してリロード API を 1 回呼べば反映され、
    // 再起動は不要。読み込みに失敗したら起動時の値を維持する（一時的に読めないだけで上限を 0 にしない）。
  {
    zone::ZoneConfig fresh;
    if (zone::loadFromEnvFile("env.zone", fresh) && fresh.maxUnitLevel > 0) {
      out.maxUnitLevel = static_cast<uint32_t>(fresh.maxUnitLevel);
    } else {
      out.maxUnitLevel = static_cast<uint32_t>(cfg.maxUnitLevel);
      zone_log::warning(
          "config reload: env.zone の読み込みに失敗したため、レベル上限は現在の値 %u のままにします",
          out.maxUnitLevel);
    }
  }
  if (!cache::loadAll(cfg, out.templates)) {
    zone_log::error("config reload: template cache failed validation");
    return false;
  }
  if (!cache::loadItems(cfg, out.items)) {
    zone_log::error("config reload: item cache failed validation");
    return false;
  }
  if (!cache::loadBuildings(cfg, out.buildings)) {
    zone_log::error("config reload: building cache failed validation");
    return false;
  }
  return true;
}

/**
 * @brief ホットリロードを実行する：読み込みと検証を行い、通過したら全ゲームワーカーへ渡す
 *
 * @param cfg zone 設定。
 *
 * @retval false 読み込みまたは検証に失敗した。**古い設定がそのまま有効**で、サービスへの影響はない。
 *
 * @note
 * - 管理 API から呼ぶ。false の場合は失敗理由を運用担当に返すこと。
 *   どのテーブルのどの行が不正かはログに出力済み。
 * - true は「新しい設定を渡した」という意味にすぎず、実際に反映されるのは各ワーカーの次のフレーム。
 */
inline bool requestReload(const zone::ZoneConfig &cfg) {
  auto snapshot = std::make_shared<ConfigSnapshot>();
  if (!buildSnapshot(cfg, *snapshot)) {
    return false;
  }

  for (auto &q : g_pending) {
    q.enqueue(snapshot);
  }
  zone_log::info("config reload: new snapshot queued for %u worker(s)",
                 static_cast<unsigned>(runtime_config::NUM_GAME_WORKERS));
  return true;
}

/**
 * @brief tick の境界で、差し替え待ちの設定を取り込む
 *
 * @param workerId ゲームワーカーの番号。
 * @param[out] outTemplates 現在のテンプレートキャッシュ。新しいスナップショットがあれば丸ごと置き換える。
 * @param[out] outItems 現在のアイテム／ショップキャッシュ。
 * @param[out] outBuildings 現在の建物キャッシュ。
 *
 * @retval true このフレームで設定の差し替えが発生した。
 *
 * @note
 * - 必ずフレームの最初、どのシステムも動く前に呼ぶこと。
 *   そうしないと、同じフレーム内でシステムごとに異なるバージョンの設定を見てしまう。
 * - キューに複数のスナップショットが溜まっている場合は、最後の 1 つだけを適用する（途中のバージョンに意味はない）。
 */
inline bool applyPending(uint8_t workerId, cache::TemplateCache &outTemplates,
                         cache::ItemCache &outItems,
                         cache::BuildingCache &outBuildings,
                         uint32_t &outMaxUnitLevel) {
  if (workerId >= runtime_config::NUM_GAME_WORKERS) return false;

  std::shared_ptr<ConfigSnapshot> latest;
  std::shared_ptr<ConfigSnapshot> one;
  while (g_pending[workerId].try_dequeue(one)) {
    latest = std::move(one);
  }
  if (!latest) return false;

  outTemplates = latest->templates;
  outItems = latest->items;
  outBuildings = latest->buildings;
  outMaxUnitLevel = latest->maxUnitLevel;

  zone_log::info(
      "config reloaded: %zu units, %zu items, %zu shop items, %zu buildings, "
      "max level %u",
      outTemplates.units.size(), outItems.items.size(),
      outItems.shopById.size(), outBuildings.buildings.size(),
      outMaxUnitLevel);
  return true;
}

} // namespace config_reload
