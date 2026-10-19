#pragma once
#include "config/zone_config.h"
#include "config/runtime_config.h"
#include "ecs/components.h"
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include "ecs/map_components.h"
#include "utils/log.h"
#include "utils/safe_utils.h"
#include "db/mysql_x_session.h"

namespace cache {

/**
 * @brief 現在の FlatBuffers の通信レイアウトでは map_id は uint8。
 *
 * @note
 * - DB の map_templates / areas / area_spawn_points の map_id は 0..255 に収まっていなければならない。
 * - プロトコルを uint16/uint32 に広げる場合は、先に fbs と送受信のすべての DTO を変更してから、ここを緩めること。
 */
constexpr uint32_t kMaxWireMapId = 255;

/**
 * @brief ボスの陣営（unit_templates.camp_id）
 *
 * @note
 * - 既存の取り決めを踏襲：0=募集可能 1=通常モンスター 2=ボス。
 *   ボスのテンプレートはマップ入場時のスポーンには含めず、C2S_ChallengeBoss のときだけ生成する。
 */
constexpr uint32_t kBossCampId = 2;
constexpr uint32_t kMinAreaId = 1;
constexpr uint32_t kMaxAreaId = 4;

/**
 * @brief DB の map_id を、現在の通信プロトコルに欠損なく載せられるかを検証する
 *
 * @param source 設定の出所となるフィールド名。呼び出し中だけ借用し、エラーログに使う。
 * @param mapId DB から読み取った map_id。
 *
 * @retval true mapId を現在の uint8 の通信フィールドにエンコードできる。
 * @retval false 設定がプロトコルの上限を超えている。呼び出し側は起動を失敗させること。
 */
inline bool validateWireMapId(const char *source, uint32_t mapId) {
  if (mapId <= kMaxWireMapId) return true;
  zone_log::error("cache invalid %s=%u exceeds current fbs uint8 map_id limit %u",
                  source, mapId, kMaxWireMapId);
  return false;
}

/**
 * @brief DB の area_id が、現在のメインシティの業務上のエリア範囲内にあるかを検証する
 *
 * @param source 設定の出所となるフィールド名。呼び出し中だけ借用し、エラーログに使う。
 * @param areaId DB から読み取った area_id。
 *
 * @retval true areaId を業務上のエリア ID として使える。
 * @retval false 0 または範囲外のエリア。呼び出し側は起動を失敗させること。
 */
inline bool validateAreaId(const char *source, uint32_t areaId) {
  if (areaId >= kMinAreaId && areaId <= kMaxAreaId) return true;
  zone_log::error("cache invalid %s=%u outside supported area range %u..%u",
                  source, areaId, kMinAreaId, kMaxAreaId);
  return false;
}

/**
 * @brief unit_templates テーブル 1 行分のキャッシュ（と、そのドロップ設定）
 *
 * @note
 * - 文字列はキャッシュオブジェクトが所有する。読み込み後、ゲームワーカーからは読み取り専用。
 * - unitId は DB の unit_templates.unit_id に対応する。
 */
struct DropEntry {
  uint32_t itemId = 0;
  float    rate = 0.f;   // 相対的な重みで、独立した確率ではない
};

struct UnitTemplate {
  uint32_t unitId = 0;
  std::string unitName;
  uint32_t campId = 0;
  std::string campName;
  std::string description;
  uint32_t baseHp = 100;
  uint32_t baseAtk = 10;
  uint32_t baseDef = 5;
  float baseSpeed = 120.f;
  float attackRange = 1.f;
  uint32_t goldReward = 0;      // 撃破報酬のゴールド
  std::vector<DropEntry> drops; // drops_json を解析したもの。空でもよい
};

/**
 * @brief unit_templates.drops_json を解析する
 *
 * @param json [{"item_id":1,"rate":0.3},{"item_id":2,"rate":0.2}] の形式のテキスト。
 * @param[out] out 解析結果。最初にクリアする。
 *
 * @note
 * - 認識するキーは item_id と rate の 2 つだけ。形式が不正な場合は起動を失敗させず、空のリストを返す。
 *   ドロップは機能を落としても動ける設定なので、1 行壊れているだけで zone 全体が起動できなくなるべきではない。
 * - rate は相対的な重み。呼び出し側は重みの合計で正規化してから抽選する。
 */
inline void parseDropsJson(const std::string &json,
                           std::vector<DropEntry> &out) {
  out.clear();
  std::size_t pos = 0;
  while (true) {
    const std::size_t objBegin = json.find('{', pos);
    if (objBegin == std::string::npos) break;
    const std::size_t objEnd = json.find('}', objBegin);
    if (objEnd == std::string::npos) break;
    const std::string obj = json.substr(objBegin + 1, objEnd - objBegin - 1);
    pos = objEnd + 1;

    DropEntry e;
    bool hasItem = false;
    bool hasRate = false;

    std::size_t fieldPos = 0;
    while (fieldPos < obj.size()) {
      const std::size_t comma = obj.find(',', fieldPos);
      const std::string field = obj.substr(
          fieldPos, comma == std::string::npos ? std::string::npos
                                               : comma - fieldPos);
      const std::size_t colon = field.find(':');
      if (colon != std::string::npos) {
        std::string key = field.substr(0, colon);
        std::string val = field.substr(colon + 1);
        const auto strip = [](std::string &t) {
          std::string r;
          for (const char c : t) {
            if (c != '"' && c != ' ' && c != '\n' && c != '\r' && c != '\t') {
              r.push_back(c);
            }
          }
          t = r;
        };
        strip(key);
        strip(val);
        try {
          if (key == "item_id") {
            e.itemId = static_cast<uint32_t>(std::stoul(val));
            hasItem = true;
          } else if (key == "rate") {
            e.rate = std::stof(val);
            hasRate = true;
          }
        } catch (...) {
                    // フィールドを 1 つでも解析できなければ、このドロップ設定だけを捨てる。他の要素には影響しない。
        }
      }
      if (comma == std::string::npos) break;
      fieldPos = comma + 1;
    }

    if (hasItem && hasRate && e.itemId != 0 && e.rate > 0.f) {
      out.push_back(e);
    }
  }
}

/**
 * @brief map_templates テーブル 1 行分のキャッシュ
 *
 * @note
 * - mapId は DB の map_templates.map_id に対応し、現在の通信上の map_id フィールドに収まっていなければならない。
 * - spawnRules はキャッシュオブジェクトが所有する。現在のメインシティの固定スポーンは主に fixedSpawns を使う。
 */
struct MapTemplate {
  uint32_t mapId = 0;
  std::string mapName;
  MapMode mapMode = MapMode::City;       // 0=City, 1=Dungeon
  uint8_t maxPlayers = 1;
  Position respawn = {0.f, 0.f};

    // このマップのスポーンルール（複数を重ねてよい）
  std::vector<SpawnRule> spawnRules;
};

/**
 * @brief エリアの矩形境界のキャッシュ
 *
 * @note
 * - (mapId, areaId) ごとに、正しい矩形境界を 1 つ設定しなければならない。
 * - スポーン地点の設定チェック、モンスターの CAreaBounds の初期化、movement_clamp_system による押し戻しの保険に使う。
 * - プレイヤー船は現在この矩形の制約を受けず、巡回・追跡は nav のエリアで制約する。
 */
struct AreaBounds {
  uint32_t mapId = 0;
  uint8_t  areaId = 0;
  float    minX = 0.f, maxX = 0.f;
  float    minY = 0.f, maxY = 0.f;
};

/**
 * @brief ボス出現地点のキャッシュ用キーを作る
 *
 * @param mapId マップ ID。
 * @param bossUnitId ボスの unit_templates.unit_id。
 *
 * @return (mapId << 32) | bossUnitId。
 */
inline uint64_t bossSpawnKey(uint32_t mapId, uint32_t bossUnitId) {
  return (static_cast<uint64_t>(mapId) << 32) |
         static_cast<uint64_t>(bossUnitId);
}

/**
 * @brief エリア境界のキャッシュ用キーを作る
 *
 * @param mapId マップ ID。現在の通信上の map_id の範囲チェックを通過済みであること。
 * @param areaId 業務上のエリア ID（1 始まり）。0 は無効なエリアとして予約している。
 *
 * @return (mapId << 8) | areaId。TemplateCache::areaBounds で使う。
 */
inline uint64_t areaBoundsKey(uint32_t mapId, uint8_t areaId) {
  return (static_cast<uint64_t>(mapId) << 8) | static_cast<uint64_t>(areaId);
}

/**
 * @brief area_spawn_points テーブル 1 行分のキャッシュ
 *
 * @note
 * - id は DB の area_spawn_points.id に対応する。
 * - x/y は同じ mapId + areaId の AreaBounds 内になければならない。loadAll で検証する。
 */
struct AreaSpawnPoint {
  uint64_t id = 0;
  uint32_t mapId = 0;
  uint8_t areaId = 0;
  float x = 0.f;
  float y = 0.f;
};

/**
 * @brief monster_spawns_point テーブル 1 行分のキャッシュ
 *
 * @note
 * - spawnPointId は AreaSpawnPoint.id を参照する。参照先がない場合、loadAll は拒否する。
 * - mapId は参照先のスポーン地点から逆引きして求める。設定テーブルで同じ情報を重複して持ち、食い違うのを防ぐため。
 */
struct MapFixedSpawn {
  uint32_t mapId = 0;
  uint32_t unitId = 0;
  uint32_t campId = 0;
  uint64_t spawnPointId = 0;  // area_spawn_points.id を参照する
};

/**
 * @brief 起動時に読み込む読み取り専用のテンプレートキャッシュ
 *
 * @note
 * - ゲームワーカーが所有する。読み込み後、実行中は読み取り専用。
 * - すべての vector/map のメモリは TemplateCache が所有する。呼び出し側はリロードをまたいで可変参照を保持しないこと。
 */
struct TemplateCache {
  std::unordered_map<uint32_t, UnitTemplate> units;  // unitId → UnitTemplate
  std::unordered_map<uint32_t, MapTemplate> maps;    // mapId → MapTemplate

    // ── スポーン地点システム ──
    // spawnPointId -> スポーン地点の座標。
  std::unordered_map<uint64_t, AreaSpawnPoint> spawnPoints;
    // mapId -> そのマップの通常のスポーン設定（ボスは含まない）。各設定は spawnPointId から座標を逆引きする。
    // ボスはここに入れない。ボスはプレイヤーが挑戦したときだけ生成し、マップ入場時には出現させない。
  std::unordered_map<uint32_t, std::vector<MapFixedSpawn>> fixedSpawns;

    // (mapId << 32) | bossUnitId -> そのボスの出現地点 ID。
    // データは同じく monster_spawns_point から来るが、陣営で分けている。camp_id=2 の行は
    // 通常のスポーンには使わず、各ボスの出現位置を定義する。
  std::unordered_map<uint64_t, uint64_t> bossSpawnPoints;
    // mapId -> スポーン地点 ID の一覧
  std::unordered_map<uint32_t, std::vector<uint64_t>> spawnPointIdsByMap;

    // ── エリアシステム ──
    // areaBoundsKey(mapId, areaId) -> エリアの矩形境界。
  std::unordered_map<uint64_t, AreaBounds> areaBounds;
};




/**
 * @brief DB から実行時に使うテンプレートキャッシュをすべて読み込む
 *
 * @param cfg zone 設定。接続文字列は呼び出し中だけ借用する。
 * @param out 呼び出し側が所有するキャッシュ。各テーブルのデータを書き込む。
 *
 * @retval true すべてのテンプレートと参照・範囲のチェックを通過した。
 * @retval false DB の検索に失敗した、または設定の参照やプロトコルの上限が不正。呼び出し側は起動処理を中止すること。
 *
 * @note
 * - ゲームワーカーの起動時に 1 回呼ぶ。実行中に同じ out をその場でリロードしないこと（ホットリロードは新しいスナップショットに読み込む）。
 */
inline bool loadAll(const zone::ZoneConfig &cfg, TemplateCache &out) {
  try {
    auto sess = db::makeSession(cfg);

        // db という名前にしないこと。db:: 名前空間を隠してしまう。
    const std::string dbName = cfg.mysqlDb;

        // ── unit_templates ──
    {
      auto res = sess->sql(
          "SELECT unit_id, unit_name, camp_id, camp_name, description,"
          "       base_hp, base_atk, base_def, base_speed, attack_range,"
          "       COALESCE(gold_reward, 0), COALESCE(drops_json, '') "
          "FROM `" + dbName + "`.unit_templates")
          .execute();

      for (auto row : res) {
        UnitTemplate t;
        t.unitId      = utils::safeToU32(row[0]);
        t.unitName    = utils::safeToStr(row[1]);
        t.campId      = utils::safeToU32(row[2]);
        t.campName    = utils::safeToStr(row[3]);
        t.description = utils::safeToStr(row[4]);
        t.baseHp      = utils::safeToU32(row[5], 100);
        t.baseAtk     = utils::safeToU32(row[6], 10);
        t.baseDef     = utils::safeToU32(row[7], 5);
        t.baseSpeed   = utils::safeToF32(row[8], 120.f);
        t.attackRange = utils::safeToF32(row[9], 1.f);
        t.goldReward  = utils::safeToU32(row[10], 0);
        parseDropsJson(utils::safeToStr(row[11]), t.drops);
        out.units[t.unitId] = std::move(t);
      }
      zone_log::info("cache loaded %zu unit_templates", out.units.size());
    }

        // ── map_templates ──
    {
      auto res = sess->sql(
          "SELECT map_id, map_name, map_mode, max_players, respawn_x, respawn_y "
          "FROM `" + dbName + "`.map_templates")
          .execute();

      for (auto row : res) {
        MapTemplate t;
        t.mapId      = utils::safeToU32(row[0]);
        if (!validateWireMapId("map_templates.map_id", t.mapId)) return false;
        t.mapName    = utils::safeToStr(row[1]);
        t.mapMode    = static_cast<MapMode>(utils::safeToU32(row[2]));
        t.maxPlayers = static_cast<uint8_t>(utils::safeToU32(row[3], 1));
        t.respawn.x   = utils::safeToF32(row[4]);
        t.respawn.y   = utils::safeToF32(row[5]);
        out.maps[t.mapId] = std::move(t);
      }
      zone_log::info("cache loaded %zu map_templates", out.maps.size());
    }

        // ── areas（エリアの矩形境界）────────────────────────────────────
        // area_spawn_points より先に areas を読み込み、各スポーン地点の範囲チェックができるようにする。
    {
      auto res = sess->sql(
          "SELECT map_id, area_id, min_x, max_x, min_y, max_y "
          "FROM `" + dbName + "`.areas")
          .execute();

      for (auto row : res) {
        AreaBounds b;
        b.mapId  = utils::safeToU32(row[0]);
        if (!validateWireMapId("areas.map_id", b.mapId)) return false;
        const uint32_t rawAreaId = utils::safeToU32(row[1]);
        if (!validateAreaId("areas.area_id", rawAreaId)) return false;
        b.areaId = static_cast<uint8_t>(rawAreaId);
        b.minX   = utils::safeToF32(row[2]);
        b.maxX   = utils::safeToF32(row[3]);
        b.minY   = utils::safeToF32(row[4]);
        b.maxY   = utils::safeToF32(row[5]);
        if (b.minX >= b.maxX || b.minY >= b.maxY) {
          zone_log::error(
              "cache invalid AreaBounds map=%u area=%u "
              "x[%g,%g] y[%g,%g] (min >= max)",
              b.mapId, b.areaId, b.minX, b.maxX, b.minY, b.maxY);
          return false;
        }
        out.areaBounds[areaBoundsKey(b.mapId, b.areaId)] = b;
      }
      zone_log::info("cache loaded %zu areas", out.areaBounds.size());
    }

        // ── area_spawn_points（スポーン地点の座標）──────────────────────────
    {
      auto res = sess->sql(
          "SELECT id, map_id, area_id, x, y "
          "FROM `" + dbName + "`.area_spawn_points")
          .execute();

      for (auto row : res) {
        AreaSpawnPoint sp;
        sp.id     = utils::safeToU64(row[0]);
        sp.mapId  = utils::safeToU32(row[1]);
        if (!validateWireMapId("area_spawn_points.map_id", sp.mapId)) return false;
        const uint32_t rawAreaId = utils::safeToU32(row[2]);
        if (!validateAreaId("area_spawn_points.area_id", rawAreaId)) return false;
        sp.areaId = static_cast<uint8_t>(rawAreaId);
        sp.x      = utils::safeToF32(row[3]);
        sp.y      = utils::safeToF32(row[4]);

                // 検証：スポーン地点は所属するエリアの矩形内になければならない（設定ミスは即座に失敗させる）。
        auto bIt = out.areaBounds.find(areaBoundsKey(sp.mapId, sp.areaId));
        if (bIt == out.areaBounds.end()) {
          zone_log::error(
              "cache spawn_point id=%llu references missing "
              "area (map=%u area=%u)",
              static_cast<unsigned long long>(sp.id),
              sp.mapId, sp.areaId);
          return false;
        }
        const auto &b = bIt->second;
        if (sp.x < b.minX || sp.x > b.maxX || sp.y < b.minY || sp.y > b.maxY) {
          zone_log::error(
              "cache spawn_point id=%llu (map=%u area=%u) at "
              "(%g,%g) outside bounds x[%g,%g] y[%g,%g]",
              static_cast<unsigned long long>(sp.id),
              sp.mapId, sp.areaId, sp.x, sp.y,
              b.minX, b.maxX, b.minY, b.maxY);
          return false;
        }

        out.spawnPoints[sp.id] = sp;
        out.spawnPointIdsByMap[sp.mapId].push_back(sp.id);
      }
      size_t total = 0;
      for (auto &kv : out.spawnPointIdsByMap) total += kv.second.size();
      zone_log::info("cache loaded %zu area_spawn_points (across %zu maps)",
                     total, out.spawnPointIdsByMap.size());
    }

        // ── monster_spawns_point（モンスターの出現設定）─────────────────────
    {
      auto res = sess->sql(
          "SELECT unit_id, camp_id, spawn_point_id "
          "FROM `" + dbName + "`.monster_spawns_point")
          .execute();

      for (auto row : res) {
        const uint64_t spId = utils::safeToU64(row[2]);
        auto it = out.spawnPoints.find(spId);
        if (it == out.spawnPoints.end()) {
          zone_log::error(
              "cache invalid monster_spawns_point: missing spawn_point_id=%llu",
              static_cast<unsigned long long>(spId));
          return false;
        }
        MapFixedSpawn ms;
        ms.mapId        = it->second.mapId;
        ms.unitId       = utils::safeToU32(row[0]);
        ms.campId       = utils::safeToU32(row[1]);
        ms.spawnPointId = spId;
        const auto unitIt = out.units.find(ms.unitId);
        if (unitIt == out.units.end()) {
          zone_log::error(
              "cache invalid monster_spawns_point: missing unit_id=%u "
              "(spawn_point_id=%llu)",
              ms.unitId, static_cast<unsigned long long>(spId));
          return false;
        }

                // ボス（camp_id=2）は通常のスポーン表に入れない。ボスは挑戦時にだけ生成する。
                // この行は、そのボスの出現位置を記録するためのもの。
        if (unitIt->second.campId == kBossCampId) {
          out.bossSpawnPoints[bossSpawnKey(ms.mapId, ms.unitId)] = spId;
          continue;
        }
        out.fixedSpawns[ms.mapId].push_back(ms);
      }
      size_t total = 0;
      for (auto &kv : out.fixedSpawns) total += kv.second.size();
      zone_log::info(
          "cache loaded %zu monster_spawns_point (across %zu maps), "
          "%zu boss spawn points",
          total, out.fixedSpawns.size(), out.bossSpawnPoints.size());
    }

    return true;
  } catch (const std::exception &e) {
    zone_log::error("cache load failed: %s", e.what());
    return false;
  }
}

/**
 * @brief キャラクターのメインシティのデータを読み込む（なければ作成する）
 *
 * @param sess 接続済みの MySQL セッション。借用して SQL を実行する。
 * @param db データベース名。現状は値渡しで、関数内で SQL に連結する。
 * @param roleId キャラクター ID の C 文字列。空でなく、呼び出し中は有効であること。
 *
 * @return 既存の、または新しく作成した CityData。
 *
 * @note
 * - role_cities のスキーマが mysql_worker::bootstrap で作成済みであることは、呼び出し側が保証すること。
 */
inline CityData loadCityData(db::Session &sess, const std::string db, const char* const roleId) {
    // role_cities を検索する
  auto res = sess.sql("SELECT city_level FROM `" + db + "`.role_cities WHERE role_id = ?")
      .bind(roleId).execute();
  auto row = res.fetchOne();
  if (row) {
    return CityData{ .cityLevel = utils::safeToU32(row[0], 1)};
  }
    // 新規プレイヤー：デフォルトのメインシティを挿入する
  sess.sql("INSERT INTO `" + db + "`.role_cities (role_id, city_level) VALUES (?, 1)")
    .bind(roleId).execute();
  return CityData{ .cityLevel = 1 };
}
} // namespace cache
