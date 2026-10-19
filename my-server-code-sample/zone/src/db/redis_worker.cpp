#include "db/redis_worker.h"
#include "db/db_types.h"
#include "config/zone_config.h"
#include "runtime/shutdown.h"
#include "utils/log.h"

#include <hiredis.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

/**
 * @brief Redis にキャッシュするプレイヤー船の経験値の TTL（秒）
 *
 * @note
 * - Redis は書き戻し用のキャッシュにすぎず、最終的な永続化先はあくまで MySQL。
 */
static constexpr int kPlayerTTLSec = 24 * 3600;

/**
 * @brief Redis ワーカーを終了してよいかを判定する
 *
 * @retval true shutdown が要求済みで、このワーカー以外に producer がおらず、経験値キューが空になった。
 */
static bool redis_worker_can_stop() {
    // Redis ワーカー自身も ProducerGuard を持っている。active==1 は、経験値を投入するゲームワーカーがもういないことを意味する。
  return runtime_shutdown::isShutdownRequested() &&
         runtime_shutdown::activeProducerCount() <= 1 &&
         g_ship_exp_tasks.size_approx() == 0;
}

/**
 * @brief Redis の STATUS 応答を確認して解放する
 *
 * @retval true 応答が STATUS である。
 * @retval false 応答が空、または STATUS ではない。
 */
static bool redis_status_ok(redisReply *reply) {
  if (!reply) return false;
  const bool ok = reply->type == REDIS_REPLY_STATUS;
  freeReplyObject(reply);
  return ok;
}

/**
 * @brief Redis の AUTH と SELECT を行う
 *
 * @retval true 接続が目的の DB に切り替わった。
 * @retval false 認証または SELECT に失敗した。
 */
static bool configure_redis_connection(redisContext *ctx,
                                       const zone::ZoneConfig &cfg) {
  if (cfg.redisPassword[0] != '\0') {
    redisReply *auth = nullptr;
    if (cfg.redisUser[0] != '\0') {
      auth = (redisReply *)redisCommand(ctx, "AUTH %s %s",
                                        cfg.redisUser, cfg.redisPassword);
    } else {
      auth = (redisReply *)redisCommand(ctx, "AUTH %s", cfg.redisPassword);
    }
    if (!redis_status_ok(auth)) {
      zone_log::error("redis_worker AUTH failed");
      return false;
    }
  }

  auto *select = (redisReply *)redisCommand(ctx, "SELECT %d", cfg.redisDb);
  if (!redis_status_ok(select)) {
    zone_log::error("redis_worker SELECT %d failed", cfg.redisDb);
    return false;
  }
  return true;
}

/**
 * @brief Redis への接続を 1 回試みる
 *
 * @retval 非nullptr 認証と DB の選択まで済んだ接続。
 * @retval nullptr 今回の接続に失敗した。待機（バックオフ）は呼び出し側が行う。
 */
static redisContext *try_connect_redis_once(const zone::ZoneConfig &cfg) {
  redisContext *ctx = redisConnect(cfg.redisHost, cfg.redisPort);
  if (ctx && !ctx->err) {
    if (configure_redis_connection(ctx, cfg)) {
      zone_log::info("redis_worker connected to %s:%d/%d",
                     cfg.redisHost, cfg.redisPort, cfg.redisDb);
      return ctx;
    }
    redisFree(ctx);
    return nullptr;
  }

  if (ctx) {
    zone_log::warning("redis_worker connect failed: %s", ctx->errstr);
    redisFree(ctx);
  } else {
    zone_log::warning("redis_worker connect failed");
  }
  return nullptr;
}

/**
 * @brief Redis 接続が使える状態であることを保証する
 *
 * @param ctx 呼び出し側が持つ Redis 接続のポインタ。無効になっていれば解放して再接続する。
 *
 * @retval true ctx が nullptr でなく、使用できる。
 * @retval false 今回は使用可能な接続を確立できなかった。
 */
static bool ensure_redis(redisContext *&ctx, const zone::ZoneConfig &cfg) {
  if (ctx) {
    auto *r = (redisReply *)redisCommand(ctx, "PING");
    if (r) {
      bool ok = (r->type == REDIS_REPLY_STATUS);
      freeReplyObject(r);
      if (ok) return true;
    }
    redisFree(ctx);
    ctx = nullptr;
  }
  ctx = try_connect_redis_once(cfg);
  return ctx != nullptr;
}

/**
 * @brief プレイヤー船の経験値タスクを MySQL の最終永続化キューへ転送する
 *
 * @param task 経験値タスク。呼び出し中だけ借用する。
 *
 * @note
 * - Redis はキャッシュ層なので、MySQL への転送は Redis が使えるかどうかに依存してはならない。
 */
static void forward_ship_exp_to_mysql(const ShipExpTask &task) {
  g_mysql_tasks.enqueue(MySqlTask{
      MySqlSaveShipExp{task.roleId, task.unitId, task.level, task.exp}});
}

/**
 * @brief プレイヤー船の経験値を Redis キャッシュに書き込んでみる
 *
 * @param redis 接続済みで DB も選択済みの Redis コンテキスト。
 * @param task 経験値タスク。呼び出し中だけ借用する。
 *
 * @retval true Redis の HSET/EXPIRE がエラーを返さなかった。
 * @retval false Redis への書き込みに失敗した。呼び出し側は切断し、バックオフしながら再接続すること。
 */
static bool try_write_ship_exp_cache(redisContext *redis,
                                     const ShipExpTask &task) {
    // Redis のキーの規則：ship_exp:{role_id}:{unit_id} の HASH、フィールドは level,exp
  std::string key("ship_exp:");
  key += task.roleId;
  key += ":";
  key += std::to_string(task.unitId);

  bool redisOk = true;
  auto *r = (redisReply *)redisCommand(
      redis, "HSET %s level %u exp %llu", key.c_str(),
      (unsigned)task.level, (unsigned long long)task.exp);
  if (!r || r->type == REDIS_REPLY_ERROR) {
    zone_log::error("redis_worker HSET %s failed", key.c_str());
    redisOk = false;
  }
  if (r) freeReplyObject(r);

  r = (redisReply *)redisCommand(redis, "EXPIRE %s %d", key.c_str(), kPlayerTTLSec);
  if (!r || r->type == REDIS_REPLY_ERROR) {
    zone_log::error("redis_worker EXPIRE %s failed", key.c_str());
    redisOk = false;
  }
  if (r) freeReplyObject(r);

  return redisOk;
}

/**
 * @brief Redis ワーカーのメインループ
 *
 * @note
 * - shutdown の条件を満たすまで、呼び出したスレッドをブロックする。
 */
void redis_worker::run(const zone::ZoneConfig &cfg) {
  {
    runtime_shutdown::ProducerGuard producerGuard;
    redisContext *redis = nullptr;
    auto nextRedisConnectAt = std::chrono::steady_clock::time_point{};

    while (true) {
      if (redis_worker_can_stop()) {
        break;
      }

      ShipExpTask expTask;
      bool got = false;
      while (g_ship_exp_tasks.try_dequeue(expTask)) {
        forward_ship_exp_to_mysql(expTask);
        if (redis && !try_write_ship_exp_cache(redis, expTask)) {
          redisFree(redis);
          redis = nullptr;
          nextRedisConnectAt = std::chrono::steady_clock::now();
        }
        got = true;
      }

      if (!redis && std::chrono::steady_clock::now() >= nextRedisConnectAt) {
        if (!ensure_redis(redis, cfg)) {
          nextRedisConnectAt =
              std::chrono::steady_clock::now() + std::chrono::seconds(2);
        }
      }

      if (!got) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    }

    if (redis) {
      redisFree(redis);
      redis = nullptr;
    }
  }
  zone_log::info("redis_worker stopped");
}
