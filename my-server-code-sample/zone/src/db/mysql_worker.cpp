#include "db/mysql_worker.h"
#include "db/db_types.h"
#include "config/zone_config.h"
#include "config/runtime_config.h"
#include "runtime/shutdown.h"
#include "utils/log.h"
#include "utils/time_utils.h"

// MySQL 従来プロトコル（3306）のクライアント
#include "db/mysql_client.h"
#include "db/mysql_x_session.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>


/**
 * @brief MySQL の例外が「インデックスが既に存在する」ことを表しているかを判定する
 *
 * @retval true エラーメッセージに Duplicate key name が含まれる。
 */
static bool isDuplicateKeyName(const db::Error &e) {
  const std::string msg = e.what();
  return msg.find("Duplicate key name") != std::string::npos;
}

/**
 * @brief MySQL の例外が「列が既に存在する」ことを表しているかを判定する
 *
 * @retval true エラーメッセージに Duplicate column name が含まれる。
 */
static bool isDuplicateColumnName(const db::Error &e) {
  const std::string msg = e.what();
  return msg.find("Duplicate column name") != std::string::npos;
}

/**
 * @brief 古いテーブルに足りない列を 1 つ追加する
 *
 * @param sess 接続済みの MySQL セッション。借用して DDL を実行する。
 * @param db データベース名。
 * @param table テーブル名。
 * @param columnSpec `ADD COLUMN` に続く列定義全体。
 *
 * @note
 * - 後方互換のための列追加だけを扱う。型を狭める変更や意味の移行は、個別に明示的な DDL を書くこと。
 */
static void addColumnIfMissing(db::Session &sess, const std::string &db,
                               const char *table,
                               const char *columnSpec) {
  try {
    sess.sql("ALTER TABLE `" + db + "`." + table +
             " ADD COLUMN " + columnSpec).execute();
  } catch (const db::Error &e) {
    if (!isDuplicateColumnName(e)) {
      throw;
    }
  }
}

/**
 * @brief 指定した列の COLUMN_TYPE が期待どおりかを判定する
 *
 * @param sess 接続済みの MySQL セッション。借用して INFORMATION_SCHEMA を検索する。
 * @param db データベース名。
 * @param table テーブル名。
 * @param column 列名。
 * @param expectedColumnType 期待する型（例：`bigint unsigned`）。
 *
 * @retval true 列が存在し、型の文字列が期待どおり。
 * @retval false 列が存在しない、または型が一致しない。
 */
static bool columnTypeIs(db::Session &sess, const std::string &db,
                         const char *table, const char *column,
                         const char *expectedColumnType) {
  auto res = sess.sql(
      "SELECT COLUMN_TYPE FROM INFORMATION_SCHEMA.COLUMNS "
      "WHERE TABLE_SCHEMA = ? AND TABLE_NAME = ? AND COLUMN_NAME = ? LIMIT 1")
      .bind(db, table, column).execute();
  auto row = res.fetchOne();
  if (!row) return false;
  return std::string(row[0]) == expectedColumnType;
}

/**
 * @brief zone の実行に必要なスキーマを作成・移行する
 *
 * @param sess 接続済みの MySQL セッション。借用して DDL/DML を実行する。
 * @param db データベース名。文字列は呼び出し中だけ借用する。
 *
 * @note
 * - 起動時の bootstrap 段階で呼ぶ。失敗した場合は db::Error を上位へ投げる。
 */
static void ensureSchema(db::Session &sess, const std::string &db) {
  sess.sql("CREATE DATABASE IF NOT EXISTS `" + db + "`").execute();

  // roles
  sess.sql("CREATE TABLE IF NOT EXISTS `" + db + "`.roles ("
    "role_id VARCHAR(32) NOT NULL PRIMARY KEY,"
    "user_id VARCHAR(64) NOT NULL,"
    "role_name VARCHAR(32) NOT NULL DEFAULT '',"
    "gold INT UNSIGNED NOT NULL DEFAULT 0,"
    "stamina INT UNSIGNED NOT NULL DEFAULT 100,"
    "crystal INT UNSIGNED NOT NULL DEFAULT 0,"
    "combat_power INT UNSIGNED NOT NULL DEFAULT 0,"
    "current_map_id TINYINT UNSIGNED NOT NULL DEFAULT 1,"
    "selected_team_id TINYINT UNSIGNED NOT NULL DEFAULT 1,"
    "created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,"
    "updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,"
    "UNIQUE KEY uq_roles_user_id(user_id)"
    ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4").execute();

  try {
    sess.sql("ALTER TABLE `" + db +
             "`.roles ADD UNIQUE KEY uq_roles_user_id(user_id)").execute();
  } catch (const db::Error &e) {
    if (!isDuplicateKeyName(e)) {
      zone_log::error("mysql_worker roles.user_id unique migration failed: %s",
                      e.what());
      throw;
    }
  }
  addColumnIfMissing(sess, db, "roles",
                     "combat_power INT UNSIGNED NOT NULL DEFAULT 0");
  addColumnIfMissing(sess, db, "roles",
                     "current_map_id TINYINT UNSIGNED NOT NULL DEFAULT 1");
  addColumnIfMissing(sess, db, "roles",
                     "selected_team_id TINYINT UNSIGNED NOT NULL DEFAULT 1");

    // unit_templates（企画が管理する設定テーブル）
  sess.sql("CREATE TABLE IF NOT EXISTS `" + db + "`.unit_templates ("
    "unit_id INT UNSIGNED NOT NULL PRIMARY KEY,"
    "unit_name VARCHAR(32) NOT NULL DEFAULT '',"
    "camp_id INT UNSIGNED NOT NULL DEFAULT 0,"
    "camp_name VARCHAR(32) NOT NULL DEFAULT '',"
    "description TEXT,"
    "base_hp INT UNSIGNED NOT NULL DEFAULT 100,"
    "base_atk INT UNSIGNED NOT NULL DEFAULT 10,"
    "base_def INT UNSIGNED NOT NULL DEFAULT 5,"
    "base_speed FLOAT NOT NULL DEFAULT 120,"
    "attack_range FLOAT NOT NULL DEFAULT 1"
    ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4").execute();

    // role_units（プレイヤーが所持する精霊）
  sess.sql("CREATE TABLE IF NOT EXISTS `" + db + "`.role_units ("
    "id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,"
    "role_id VARCHAR(32) NOT NULL,"
    "unit_id INT UNSIGNED NOT NULL,"
    "level INT UNSIGNED NOT NULL DEFAULT 1,"
    "exp BIGINT UNSIGNED NOT NULL DEFAULT 0,"
    "UNIQUE KEY uk_role_unit(role_id, unit_id),"
    "KEY idx_role_id(role_id)"
    ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4").execute();
  if (!columnTypeIs(sess, db, "role_units", "exp", "bigint unsigned")) {
    sess.sql("ALTER TABLE `" + db +
             "`.role_units MODIFY COLUMN exp BIGINT UNSIGNED NOT NULL DEFAULT 0")
        .execute();
  }

    // team_slots（編成）
  sess.sql("CREATE TABLE IF NOT EXISTS `" + db + "`.team_slots ("
    "id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,"
    "role_id VARCHAR(32) NOT NULL,"
    "team_id TINYINT UNSIGNED NOT NULL,"
    "slot_index TINYINT UNSIGNED NOT NULL,"
    "unit_id INT UNSIGNED NOT NULL,"
    "UNIQUE KEY uk_role_team_slot(role_id, team_id, slot_index),"
    "KEY idx_role_team(role_id, team_id)"
    ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4").execute();

    // map_templates（マップテンプレートの設定）
  sess.sql("CREATE TABLE IF NOT EXISTS `" + db + "`.map_templates ("
    "map_id INT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,"
    "map_name VARCHAR(64) DEFAULT '{プレイヤー名}のメインシティ',"
    "map_mode TINYINT(1) DEFAULT 0,"  // 0=City, 1=Dungeon
    "max_players TINYINT DEFAULT 1,"
    "respawn_x FLOAT DEFAULT 0,"
    "respawn_y FLOAT DEFAULT -64"
    ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci").execute();

    // デフォルトのメインシティテンプレートの初期データ：新しい DB にはそのまま投入し、既存の DB も今回の 32x32 の目標値に合わせる
  sess.sql("INSERT INTO `" + db + "`.map_templates"
    "(map_id, map_name, map_mode, max_players, respawn_x, respawn_y) VALUES"
    "(1, '{プレイヤー名}のメインシティ', 0, 1, 0, -64) "
    "ON DUPLICATE KEY UPDATE "
    "respawn_x = VALUES(respawn_x), "
    "respawn_y = VALUES(respawn_y)").execute();

    // area_spawn_points（メインシティのマップのエリア別スポーン地点）
  sess.sql("CREATE TABLE IF NOT EXISTS `" + db + "`.area_spawn_points ("
    "id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT 'スポーン地点ID',"
    "map_id INT UNSIGNED NOT NULL COMMENT 'マップID',"
    "area_id TINYINT UNSIGNED NOT NULL COMMENT 'エリアID(1=メインシティ,2=南,3=西,4=北)',"
    "x FLOAT NOT NULL DEFAULT 0 COMMENT 'スポーン地点のX座標',"
    "y FLOAT NOT NULL DEFAULT 0 COMMENT 'スポーン地点のY座標',"
    "PRIMARY KEY (id),"
    "KEY idx_map_area(map_id, area_id)"
    ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4").execute();

    // areas（エリアの矩形境界）
    // (map_id, area_id) ごとに 1 行。area_spawn_points のスポーン地点は、この表の対応する行の
    // 矩形内になければならない。外れていると TemplateCache::loadAll の起動時チェックで失敗する。
  sess.sql("CREATE TABLE IF NOT EXISTS `" + db + "`.areas ("
    "map_id INT UNSIGNED NOT NULL COMMENT 'マップID',"
    "area_id TINYINT UNSIGNED NOT NULL COMMENT 'エリアID',"
    "min_x FLOAT NOT NULL COMMENT '矩形の左下 X',"
    "max_x FLOAT NOT NULL COMMENT '矩形の右上 X',"
    "min_y FLOAT NOT NULL COMMENT '矩形の左下 Y',"
    "max_y FLOAT NOT NULL COMMENT '矩形の右上 Y',"
    "PRIMARY KEY (map_id, area_id)"
    ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4").execute();

    // メインシティのマップの 4 エリア（十字型の配置。各エリア 800×800 で、メインシティが中央）
  sess.sql("INSERT IGNORE INTO `" + db + "`.areas"
    "(map_id, area_id, min_x, max_x, min_y, max_y) VALUES"
      "(1, 1,  -400,   400,  -400,   400),"
      "(1, 2,  -400,   400, -1200,  -400),"
      "(1, 3, -1200,  -400,  -400,   400),"
      "(1, 4,  -400,   400,   400,  1200)").execute();

    // monster_spawns_point（メインシティのモンスター出現設定）
  sess.sql("CREATE TABLE IF NOT EXISTS `" + db + "`.monster_spawns_point ("
    "id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT '設定ID',"
    "unit_id INT UNSIGNED NOT NULL COMMENT 'モンスターのテンプレートID',"
    "camp_id INT UNSIGNED NOT NULL COMMENT '陣営ID',"
    "spawn_point_id BIGINT UNSIGNED NOT NULL COMMENT 'スポーン地点ID(area_spawn_points.idを参照)',"
    "PRIMARY KEY (id),"
    "UNIQUE KEY uk_spawn_point_unit(spawn_point_id, unit_id),"
    "KEY idx_spawn_point_id(spawn_point_id),"
    "KEY idx_unit_id(unit_id)"
    ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4").execute();

    // エリアのスポーン地点の初期データ：既存の DB にも、マップ 1 のエリア 2 の 3 つの初期データが必ず入るようにする
  sess.sql("INSERT INTO `" + db + "`.area_spawn_points"
    "(id, map_id, area_id, x, y) VALUES"
      "(1, 1, 2, 0, -800),"
      "(2, 1, 2, 320, -1024),"
      "(3, 1, 2, -320, -1024) "
    "ON DUPLICATE KEY UPDATE "
    "map_id = VALUES(map_id), "
    "area_id = VALUES(area_id), "
    "x = VALUES(x), "
    "y = VALUES(y)").execute();

  sess.sql("INSERT IGNORE INTO `" + db + "`.monster_spawns_point"
    "(id, unit_id, camp_id, spawn_point_id) VALUES"
      "(1, 1, 1, 1),"
      "(2, 2, 1, 2),"
      "(3, 3, 1, 3)").execute();

    // role_cities（プレイヤーのメインシティのデータ）
  sess.sql("CREATE TABLE IF NOT EXISTS `" + db + "`.role_cities ("
    "role_id VARCHAR(32) NOT NULL PRIMARY KEY COMMENT 'キャラクターID(メインシティを所有するキャラクター)',"
    "city_level INT UNSIGNED NOT NULL DEFAULT 1 COMMENT 'メインシティのレベル',"
    "updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,"
    "KEY idx_role_id(role_id)"
    ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4").execute();

    // role_unit_area_assignments（プレイヤー船のエリア割り当て）
  sess.sql("CREATE TABLE IF NOT EXISTS `" + db + "`.role_unit_area_assignments ("
    "role_id VARCHAR(32) NOT NULL,"
    "unit_id INT UNSIGNED NOT NULL,"
    "area_id INT UNSIGNED NOT NULL DEFAULT 1,"
    "updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,"
    "PRIMARY KEY (role_id, unit_id)"
    ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4").execute();

    // デフォルトの精霊（テスト用データ）
  sess.sql("INSERT IGNORE INTO `" + db + "`.unit_templates"
    "(unit_id, unit_name, camp_id, camp_name, description, base_hp, base_atk, base_def, base_speed, attack_range) VALUES"
      "(1, '駆逐艦', 1, '戦艦', '駆逐艦', 100, 10, 5, 120, 1),"
      "(2, '巡洋艦', 1, '巡洋艦', '巡洋艦', 100, 10, 5, 120, 1),"
      "(3, '空母', 1, '空母', '空母', 100, 10, 5, 120, 1)").execute();

    // テスト期間中のメインシティ AI：デモの NPC_SPEED=120 に近づける。AOI を 100ms ごとに送る場合、1 スナップショットあたり約 12 world units。
    // 古いテスト用のデフォルト値のままになっているレコードだけを移行し、後から手で調整した企画データを上書きしないようにする。
  sess.sql("UPDATE `" + db + "`.unit_templates "
    "SET base_speed = 120 "
    "WHERE unit_id IN (1, 2, 3) AND base_speed IN (40, 200)").execute();
}

/**
 * @brief データベースのスキーマを初期化する
 *
 * @retval true スキーマが使える状態になった。
 * @retval false 接続、DDL、または初期データの書き込みに失敗した。
 */
bool mysql_worker::bootstrap(const zone::ZoneConfig &cfg) {
  const std::string db_name = cfg.mysqlDb;
  if (db_name.empty()) {
    zone_log::error("mysql bootstrap failed: no database configured");
    return false;
  }

  try {
    db::Session sess(cfg.mysqlHost, cfg.mysqlPort, cfg.mysqlUser,
                     cfg.mysqlPasswd, cfg.mysqlDb,
                     db::tlsRequiredFromConfig());
    ensureSchema(sess, db_name);
    return true;
  } catch (const db::Error &e) {
    zone_log::error("mysql bootstrap failed: %s", e.what());
    return false;
  } catch (const std::exception &e) {
    zone_log::error("mysql bootstrap std::exception: %s", e.what());
    return false;
  } catch (...) {
    zone_log::error("mysql bootstrap unknown exception");
    return false;
  }
}


/**
 * @brief 文字列の role_id を、BIGINT 列に使える数値に変換する
 *
 * @param roleId メモリ上のキャラクター ID。全体が 10 進数の数字列であること。
 * @param out 変換に成功したら数値を書き込む。
 *
 * @retval false 空文字列、数字以外を含む、または uint64 の範囲外。呼び出し側はそのタスクを破棄すること。
 *
 * @note
 * - role_id 列が BIGINT で作られているテーブル（role_items / unit_equips）専用。
 *   roles / role_units などの VARCHAR のテーブルは文字列のままバインドし、ここを通さないこと。
 */
static bool parseNumericRoleId(const std::string &roleId, uint64_t &out) {
  if (roleId.empty()) return false;
  uint64_t acc = 0;
  for (const char c : roleId) {
    if (c < '0' || c > '9') return false;
    const uint64_t digit = static_cast<uint64_t>(c - '0');
    if (acc > (std::numeric_limits<uint64_t>::max() - digit) / 10) return false;
    acc = acc * 10 + digit;
  }
  out = acc;
  return true;
}

/**
 * @brief MySQL の永続化タスクをまとめて書き込む
 *
 * @param sess 接続済みの MySQL セッション。
 * @param database データベース名。
 * @param batch 借用するタスクの一覧。参照は保持しない。
 *
 * @retval true トランザクションのコミットに成功した、または batch が空。
 * @retval false 書き込みに失敗し、可能な限りロールバックした。
 */
static bool flush(db::Session &sess, const std::string &database,
                  const std::vector<MySqlTask> &batch) {
  if (batch.empty())
    return true;

  try {
    sess.startTransaction();
    for (const auto &variant : batch) {
      std::visit([&](auto &&t) {
        using T = std::decay_t<decltype(t)>;

        if constexpr (std::is_same_v<T, MySqlSaveShipExp>) {
          auto res = sess
                       .sql("UPDATE `" + database +
                            "`.role_units SET level = ?, exp = ? "
                            "WHERE role_id = ? AND unit_id = ?")
                       .bind((unsigned)t.level, (unsigned long long)t.exp,
                             t.roleId, (unsigned)t.unitId)
                       .execute();
          if (res.getAffectedItemsCount() == 0) {
            zone_log::warning("mysql skip missing role_unit role=%s unit=%u",
                              t.roleId.c_str(), t.unitId);
          }
        } else if constexpr (std::is_same_v<T, MySqlSaveShipArea>) {
          sess
              .sql("INSERT INTO `" + database +
                   "`.role_unit_area_assignments(role_id, unit_id, area_id) "
                   "VALUES(?, ?, ?) "
                   "ON DUPLICATE KEY UPDATE area_id = VALUES(area_id)")
              .bind(t.roleId, (unsigned)t.unitId, (unsigned)t.areaId)
              .execute();
        } else if constexpr (std::is_same_v<T, MySqlSaveRoleItem>) {
                    // role_items.role_id は BIGINT で、メモリ上は文字列なので明示的に変換する。
                    // 失敗したらタスクを破棄して警告を出す。暗黙の変換で 0 が書き込まれる（＝別のキャラクターのデータに混ざる）ことは絶対に避ける。
          uint64_t numericRoleId = 0;
          if (!parseNumericRoleId(t.roleId, numericRoleId)) {
            zone_log::error(
                "mysql skip role_item: role_id=%s is not numeric, "
                "cannot write BIGINT role_items.role_id",
                t.roleId.c_str());
          } else if (t.count == 0) {
            sess
                .sql("DELETE FROM `" + database +
                     "`.role_items WHERE role_id = ? AND item_id = ?")
                .bind((uint64_t)numericRoleId, (unsigned)t.itemId)
                .execute();
          } else {
            sess
                .sql("INSERT INTO `" + database +
                     "`.role_items(role_id, item_id, count) VALUES(?, ?, ?) "
                     "ON DUPLICATE KEY UPDATE count = VALUES(count)")
                .bind((uint64_t)numericRoleId, (unsigned)t.itemId,
                      (unsigned)t.count)
                .execute();
          }
        } else if constexpr (std::is_same_v<T, MySqlSaveUnitEquip>) {
                    // unit_equips.role_id も BIGINT なので、role_items と同じように扱う。
          uint64_t numericRoleId = 0;
          if (!parseNumericRoleId(t.roleId, numericRoleId)) {
            zone_log::error(
                "mysql skip unit_equip: role_id=%s is not numeric, "
                "cannot write BIGINT unit_equips.role_id",
                t.roleId.c_str());
          } else if (t.itemId == 0) {
            sess
                .sql("DELETE FROM `" + database +
                     "`.unit_equips WHERE role_id = ? AND unit_id = ? "
                     "AND slot = ?")
                .bind((uint64_t)numericRoleId, (unsigned)t.unitId,
                      (unsigned)t.slot)
                .execute();
          } else {
            sess
                .sql("INSERT INTO `" + database +
                     "`.unit_equips(role_id, unit_id, slot, item_id) "
                     "VALUES(?, ?, ?, ?) "
                     "ON DUPLICATE KEY UPDATE item_id = VALUES(item_id)")
                .bind((uint64_t)numericRoleId, (unsigned)t.unitId,
                      (unsigned)t.slot, (unsigned)t.itemId)
                .execute();
          }
        } else if constexpr (std::is_same_v<T, MySqlInsertRoleUnit>) {
                    // INSERT IGNORE：重複して投入されても、同時に重複して募集されても 2 行目はできない。
          sess
              .sql("INSERT IGNORE INTO `" + database +
                   "`.role_units(role_id, unit_id, level, exp, base_hp, "
                   "base_atk, base_def, base_speed, attack_range) "
                   "VALUES(?, ?, 1, 0, ?, ?, ?, ?, ?)")
              .bind(t.roleId, (unsigned)t.unitId, (unsigned)t.baseHp,
                    (unsigned)t.baseAtk, (unsigned)t.baseDef,
                    (unsigned)t.baseSpeed, (double)t.attackRange)
              .execute();
        } else if constexpr (std::is_same_v<T, MySqlSaveTeamSlot>) {
          if (t.unitId == 0) {
            sess
                .sql("DELETE FROM `" + database +
                     "`.team_slots WHERE role_id = ? AND team_id = ? "
                     "AND slot_index = ?")
                .bind(t.roleId, (unsigned)t.teamId, (unsigned)t.slotIndex)
                .execute();
          } else {
            sess
                .sql("INSERT INTO `" + database +
                     "`.team_slots(role_id, team_id, slot_index, unit_id) "
                     "VALUES(?, ?, ?, ?) "
                     "ON DUPLICATE KEY UPDATE unit_id = VALUES(unit_id)")
                .bind(t.roleId, (unsigned)t.teamId, (unsigned)t.slotIndex,
                      (unsigned)t.unitId)
                .execute();
          }
        } else if constexpr (std::is_same_v<T, MySqlSaveSelectedTeam>) {
          sess
              .sql("UPDATE `" + database +
                   "`.roles SET selected_team_id = ? WHERE role_id = ?")
              .bind((unsigned)t.teamId, t.roleId)
              .execute();
        } else if constexpr (std::is_same_v<T, MySqlSaveRoleBuilding>) {
                    // role_buildings.role_id は VARCHAR なので、文字列のままバインドする。
          sess
              .sql("INSERT INTO `" + database +
                   "`.role_buildings(role_id, building_id, x, y, level, state) "
                   "VALUES(?, ?, ?, ?, ?, ?) "
                   "ON DUPLICATE KEY UPDATE x = VALUES(x), y = VALUES(y), "
                   "level = VALUES(level), state = VALUES(state)")
              .bind(t.roleId, (unsigned)t.buildingId, (double)t.x, (double)t.y,
                    (unsigned)t.level, (unsigned)t.state)
              .execute();
        } else if constexpr (std::is_same_v<T, MySqlSaveRoleGold>) {
          auto res = sess
                       .sql("UPDATE `" + database +
                            "`.roles SET gold = ? WHERE role_id = ?")
                       .bind((unsigned)t.gold, t.roleId)
                       .execute();
          if (res.getAffectedItemsCount() == 0) {
                        // 接続で CLIENT_FOUND_ROWS を有効にしているので、affected_rows は「一致した行数」になる。
                        // したがって 0 行なら本当にそのキャラクターが存在しないことを意味し、たまたまゴールドの値が変わらなかったわけではない。
            zone_log::warning("mysql gold update: role not found role=%s gold=%u",
                              t.roleId.c_str(), t.gold);
          }
        }
      }, variant);
    }
    sess.commit();
    zone_log::info("mysql flushed %zu records", batch.size());
    return true;
  } catch (const db::Error &e) {
    try { sess.rollback(); } catch (...) {}
    zone_log::error("mysql flush error: %s", e.what());
    return false;
  } catch (const std::exception &e) {
    try { sess.rollback(); } catch (...) {}
    zone_log::error("mysql flush std::exception: %s", e.what());
    return false;
  } catch (...) {
    try { sess.rollback(); } catch (...) {}
    zone_log::error("mysql flush unknown exception");
    return false;
  }
}

/**
 * @brief MySQL ワーカーのメインループ
 *
 * @note
 * - shutdown の条件を満たし、書き込み待ちのタスクがすべて片付くまで、呼び出したスレッドをブロックする。
 */
void mysql_worker::run(const zone::ZoneConfig &cfg, int flush_interval_sec) {
    // ── 接続を確立する ────────────────────────────────────────────────
  db::Session *sess = nullptr;
  const std::string db_name = cfg.mysqlDb;
  if (db_name.empty()) {
    zone_log::error("mysql no database configured");
    return;
  }

  // Dedup keys:
  //   exp  = role_id:unit_id
  //   area = role_id:unit_id
  auto makeShipKey = [](const std::string &roleId, uint32_t unitId) {
    return roleId + ":" + std::to_string(unitId);
  };

  std::unordered_map<std::string, MySqlSaveShipExp>  pendingExp;
  std::unordered_map<std::string, MySqlSaveShipArea> pendingArea;
    // バッグは role_id:item_id で、ゴールドは role_id で重複を排除する。どちらのタスクも絶対値を持つので、
    // 同じキーについては最後の状態だけを残せばよい。
  std::unordered_map<std::string, MySqlSaveRoleItem> pendingItem;
  std::unordered_map<std::string, MySqlSaveRoleGold> pendingGold;
    // 装備スロットは role_id:unit_id:slot で重複を排除する。
  std::unordered_map<std::string, MySqlSaveUnitEquip> pendingEquip;
    // 建物は role_id:building_id で重複を排除する。
  std::unordered_map<std::string, MySqlSaveRoleBuilding> pendingBuilding;
    // 募集は role_id:unit_id、編成スロットは role_id:team_id:slot、
    // 出撃中の編成は role_id で重複を排除する。3 つとも絶対値の状態を持つ。
  std::unordered_map<std::string, MySqlInsertRoleUnit> pendingRoleUnit;
  std::unordered_map<std::string, MySqlSaveTeamSlot> pendingTeamSlot;
  std::unordered_map<std::string, MySqlSaveSelectedTeam> pendingSelectedTeam;

  auto inboundQueuesEmpty = []() {
    return g_mysql_tasks.size_approx() == 0 &&
           g_ship_area_tasks.size_approx() == 0;
  };
  auto canStop = [&]() {
    return runtime_shutdown::isShutdownRequested() &&
           runtime_shutdown::activeProducerCount() == 0 &&
           inboundQueuesEmpty() &&
           pendingExp.empty() &&
           pendingArea.empty() &&
           pendingItem.empty() &&
           pendingGold.empty() &&
           pendingEquip.empty() &&
           pendingBuilding.empty() &&
           pendingRoleUnit.empty() &&
           pendingTeamSlot.empty() &&
           pendingSelectedTeam.empty();
  };

  auto connectAndBootstrap = [&]() -> bool {
    while (!sess) {
      if (canStop()) {
        return false;
      }
      try {
        sess = new db::Session(cfg.mysqlHost, cfg.mysqlPort, cfg.mysqlUser,
                               cfg.mysqlPasswd, cfg.mysqlDb,
                               db::tlsRequiredFromConfig());
        zone_log::info("mysql connected to %s:%d db=%s",
                       cfg.mysqlHost, cfg.mysqlPort, cfg.mysqlDb);
        ensureSchema(*sess, db_name);
        return true;
      } catch (const db::Error &e) {
        delete sess;
        sess = nullptr;
        zone_log::warning("mysql bootstrap failed: %s, retrying...",
                          e.what());
        std::this_thread::sleep_for(std::chrono::seconds(2));
      } catch (const std::exception &e) {
        delete sess;
        sess = nullptr;
        zone_log::warning("mysql bootstrap std::exception: %s, retrying...",
                          e.what());
        std::this_thread::sleep_for(std::chrono::seconds(2));
      } catch (...) {
        delete sess;
        sess = nullptr;
        zone_log::warning("mysql bootstrap unknown exception, retrying...");
        std::this_thread::sleep_for(std::chrono::seconds(2));
      }
    }
    return true;
  };
  if (!connectAndBootstrap()) {
    zone_log::info("mysql worker stopped");
    return;
  }

  uint64_t next_flush_ms = timeutil::steady_now_ms() + static_cast<uint64_t>(flush_interval_sec) * 1000;

  while (true) {
        // ── g_mysql_tasks（経験値 + エリア）を取り出す ─────────────────────
    MySqlTask variant;
    while (g_mysql_tasks.try_dequeue(variant)) {
      std::visit([&](auto &&t) {
        using T = std::decay_t<decltype(t)>;
        if constexpr (std::is_same_v<T, MySqlSaveShipExp>) {
          pendingExp[makeShipKey(t.roleId, t.unitId)] = t;
        } else if constexpr (std::is_same_v<T, MySqlSaveShipArea>) {
          pendingArea[makeShipKey(t.roleId, t.unitId)] = t;
        } else if constexpr (std::is_same_v<T, MySqlSaveRoleItem>) {
          pendingItem[makeShipKey(t.roleId, t.itemId)] = t;
        } else if constexpr (std::is_same_v<T, MySqlSaveRoleGold>) {
          pendingGold[t.roleId] = t;
        } else if constexpr (std::is_same_v<T, MySqlSaveUnitEquip>) {
          pendingEquip[makeShipKey(t.roleId, t.unitId) + ":" +
                       std::to_string(t.slot)] = t;
        } else if constexpr (std::is_same_v<T, MySqlSaveRoleBuilding>) {
          pendingBuilding[makeShipKey(t.roleId, t.buildingId)] = t;
        } else if constexpr (std::is_same_v<T, MySqlInsertRoleUnit>) {
          pendingRoleUnit[makeShipKey(t.roleId, t.unitId)] = t;
        } else if constexpr (std::is_same_v<T, MySqlSaveTeamSlot>) {
          pendingTeamSlot[makeShipKey(t.roleId, t.teamId) + ":" +
                          std::to_string(t.slotIndex)] = t;
        } else if constexpr (std::is_same_v<T, MySqlSaveSelectedTeam>) {
          pendingSelectedTeam[t.roleId] = t;
        }
      }, variant);
    }

        // ── プレイヤー船のエリアタスクを直接取り出す ─────────────────────────
    ShipAreaTask areaTask;
    while (g_ship_area_tasks.try_dequeue(areaTask)) {
      auto key = makeShipKey(areaTask.roleId, areaTask.unitId);
      pendingArea[key] = MySqlSaveShipArea{
          areaTask.roleId, areaTask.unitId, areaTask.areaId};
    }

    const bool hasPending = !pendingExp.empty() || !pendingArea.empty() ||
                            !pendingItem.empty() || !pendingGold.empty() ||
                            !pendingEquip.empty() || !pendingBuilding.empty() ||
                            !pendingRoleUnit.empty() ||
                            !pendingTeamSlot.empty() ||
                            !pendingSelectedTeam.empty();
    const bool forceShutdownFlush =
        runtime_shutdown::isShutdownRequested() &&
        runtime_shutdown::activeProducerCount() == 0 &&
        inboundQueuesEmpty();
    if (hasPending &&
        (timeutil::steady_now_ms() >= next_flush_ms || forceShutdownFlush)) {
      std::vector<MySqlTask> batch;
      batch.reserve(pendingExp.size() + pendingArea.size() +
                    pendingItem.size() + pendingGold.size() +
                    pendingEquip.size() + pendingBuilding.size() +
                    pendingRoleUnit.size() + pendingTeamSlot.size() +
                    pendingSelectedTeam.size());
      for (auto &[id, t] : pendingExp)  batch.emplace_back(t);
      for (auto &[id, t] : pendingArea) batch.emplace_back(t);
      for (auto &[id, t] : pendingItem) batch.emplace_back(t);
      for (auto &[id, t] : pendingGold) batch.emplace_back(t);
      for (auto &[id, t] : pendingEquip) batch.emplace_back(t);
      for (auto &[id, t] : pendingBuilding) batch.emplace_back(t);
      for (auto &[id, t] : pendingRoleUnit) batch.emplace_back(t);
      for (auto &[id, t] : pendingTeamSlot) batch.emplace_back(t);
      for (auto &[id, t] : pendingSelectedTeam) batch.emplace_back(t);
      pendingExp.clear();
      pendingArea.clear();
      pendingItem.clear();
      pendingGold.clear();
      pendingEquip.clear();
      pendingBuilding.clear();
      pendingRoleUnit.clear();
      pendingTeamSlot.clear();
      pendingSelectedTeam.clear();

      if (!flush(*sess, db_name, batch)) {
                // キューに戻す（リトライ）
        for (const auto &v : batch) {
          std::visit([&](auto &&t) {
            using T = std::decay_t<decltype(t)>;
            if constexpr (std::is_same_v<T, MySqlSaveShipExp>) {
              pendingExp[makeShipKey(t.roleId, t.unitId)] = t;
            } else if constexpr (std::is_same_v<T, MySqlSaveShipArea>) {
              pendingArea[makeShipKey(t.roleId, t.unitId)] = t;
            } else if constexpr (std::is_same_v<T, MySqlSaveRoleItem>) {
              pendingItem[makeShipKey(t.roleId, t.itemId)] = t;
            } else if constexpr (std::is_same_v<T, MySqlSaveRoleGold>) {
              pendingGold[t.roleId] = t;
            } else if constexpr (std::is_same_v<T, MySqlSaveUnitEquip>) {
              pendingEquip[makeShipKey(t.roleId, t.unitId) + ":" +
                           std::to_string(t.slot)] = t;
            } else if constexpr (std::is_same_v<T, MySqlSaveRoleBuilding>) {
              pendingBuilding[makeShipKey(t.roleId, t.buildingId)] = t;
            } else if constexpr (std::is_same_v<T, MySqlInsertRoleUnit>) {
              pendingRoleUnit[makeShipKey(t.roleId, t.unitId)] = t;
            } else if constexpr (std::is_same_v<T, MySqlSaveTeamSlot>) {
              pendingTeamSlot[makeShipKey(t.roleId, t.teamId) + ":" +
                              std::to_string(t.slotIndex)] = t;
            } else if constexpr (std::is_same_v<T, MySqlSaveSelectedTeam>) {
              pendingSelectedTeam[t.roleId] = t;
            }
          }, v);
        }
        delete sess;
        sess = nullptr;
        if (!connectAndBootstrap()) {
          break;
        }
      }
      next_flush_ms = timeutil::steady_now_ms() +
                      static_cast<uint64_t>(flush_interval_sec) * 1000;
    }

    if (canStop()) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  delete sess;
  sess = nullptr;
  zone_log::info("mysql worker stopped");
}
