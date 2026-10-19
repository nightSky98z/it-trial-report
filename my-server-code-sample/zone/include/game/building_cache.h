#pragma once
#include "config/zone_config.h"
#include "db/mysql_x_session.h"
#include "utils/log.h"
#include "utils/safe_utils.h"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

/**
 * @file building_cache.h
 * @brief 建物テンプレート（buildings テーブル）の読み取り専用設定
 *
 * @note
 * - buildings.id は、プロトコルの building_id や BuildingKind の値と同一。
 *   フロントエンドの building_visual_catalog も同じ番号体系を使っており、別の対応表は存在しない。
 */
namespace cache {

/**
 * @brief buildings テーブル 1 行分のキャッシュ
 *
 * @note
 * - 文字列はキャッシュオブジェクトが所有する。読み込み後、実行中は読み取り専用。
 * - unlockCost は解放に必要なゴールド。isDefaultBuilt は新規キャラクターで最初から解放済みかどうか。
 * - defaultX/defaultY は新規キャラクターに建てるときの初期配置座標。
 */
struct BuildingTemplate {
  uint32_t    buildingId = 0;
  std::string buildingName;
  std::string description;
  uint32_t    baseLevel = 1;
  uint32_t    maxLevel = 10;
  bool        isDefaultBuilt = false;
  int32_t     sortOrder = 0;
  int32_t     unlockCost = 0;
  float       defaultX = 0.f;
  float       defaultY = 0.f;
    // 当たり判定の半径：経路探索ではこの半径内のセルを通行不可にし、ユニットが建物をすり抜けないようにする。
  float       colliderRadius = 50.f;
};

/**
 * @brief 建物テンプレートのキャッシュ
 *
 * @note
 * - ゲームワーカーが所有する。読み込み後、実行中は読み取り専用。
 * - defaultBuiltIds は sort_order 順に並んでおり、新規キャラクターへの初期建物の付与に使う。
 */
struct BuildingCache {
  std::unordered_map<uint32_t, BuildingTemplate> buildings;
  std::vector<uint32_t> defaultBuiltIds;
};

/**
 * @brief 建物テンプレートを検索する
 *
 * @param c 読み取り専用のキャッシュ。返すポインタはキャッシュ内部を指す。
 * @param buildingId buildings.id。
 *
 * @retval nullptr その建物は設定されていない。呼び出し側は不正なリクエストとして拒否すること。
 */
inline const BuildingTemplate *findBuilding(const BuildingCache &c,
                                            uint32_t buildingId) {
  const auto it = c.buildings.find(buildingId);
  return it == c.buildings.end() ? nullptr : &it->second;
}

/**
 * @brief 現在の通信プロトコルで表現できる building_kind の最大値
 *
 * @note
 * - BuildingSnapshot.building_kind は uint8 で、フロントエンドの BuildingKindData も 8 までしか定義していない。
 *   範囲外の設定があるとクライアントが認識できない建物種別を受け取るため、起動時に拒否する。
 */
inline constexpr uint32_t kMaxBuildingKind = 8;

/**
 * @brief DB から buildings の設定を読み込む
 *
 * @param cfg zone 設定。接続パラメータは呼び出し中だけ借用する。
 * @param out 呼び出し側が所有するキャッシュ。内容は上書きされる。
 *
 * @retval false DB の検索に失敗した、または範囲外の building id がある。呼び出し側は起動処理を中止すること。
 */
inline bool loadBuildings(const zone::ZoneConfig &cfg, BuildingCache &out) {
  try {
    auto sess = db::makeSession(cfg);
    const std::string dbName = cfg.mysqlDb;

    auto res = sess->sql(
        "SELECT id, building_name, COALESCE(description, ''), "
        "       COALESCE(base_level, 1), COALESCE(max_level, 10), "
        "       COALESCE(is_default_built, 0), COALESCE(sort_order, 0), "
        "       COALESCE(unlock_cost, 0), COALESCE(default_x, 0), "
        "       COALESCE(default_y, 0), COALESCE(default_collider_radius, 50) "
        "FROM `" + dbName + "`.buildings ORDER BY sort_order, id")
        .execute();

    for (auto row : *&res) {
      BuildingTemplate t;
      t.buildingId     = utils::safeToU32(row[0]);
      t.buildingName   = utils::safeToStr(row[1]);
      t.description    = utils::safeToStr(row[2]);
      t.baseLevel      = utils::safeToU32(row[3], 1);
      t.maxLevel       = utils::safeToU32(row[4], 10);
      t.isDefaultBuilt = utils::safeToU32(row[5], 0) != 0;
      t.sortOrder      = utils::safeToI32(row[6], 0);
      t.unlockCost     = utils::safeToI32(row[7], 0);
      t.defaultX       = utils::safeToF32(row[8], 0.f);
      t.defaultY       = utils::safeToF32(row[9], 0.f);
      t.colliderRadius = utils::safeToF32(row[10], 50.f);
      if (t.colliderRadius < 0.f) {
        zone_log::error("building_cache invalid buildings.id=%u "
                        "default_collider_radius=%g (negative)",
                        t.buildingId, t.colliderRadius);
        return false;
      }

      if (t.buildingId == 0 || t.buildingId > kMaxBuildingKind) {
        zone_log::error(
            "building_cache invalid buildings.id=%u outside 1..%u "
            "(building_kind is uint8 and frontend only knows 1..%u)",
            t.buildingId, kMaxBuildingKind, kMaxBuildingKind);
        return false;
      }
      if (t.unlockCost < 0) {
        zone_log::error("building_cache invalid buildings.id=%u unlock_cost=%d",
                        t.buildingId, t.unlockCost);
        return false;
      }

      if (t.isDefaultBuilt) out.defaultBuiltIds.push_back(t.buildingId);
      out.buildings[t.buildingId] = std::move(t);
    }

    zone_log::info("cache loaded %zu buildings (%zu default built)",
                   out.buildings.size(), out.defaultBuiltIds.size());
    return true;
  } catch (const std::exception &e) {
    zone_log::error("building_cache load failed: %s", e.what());
    return false;
  }
}

} // namespace cache
