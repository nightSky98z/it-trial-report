#include "network/zone_http.h"
#include "auth/join_ticket.h"
#include "auth/pass_token.h"
#include "config/zone_config.h"
#include "game/game_session.h"
#include "config/config_reload.h"
#include "game/building_cache.h"
#include "game/item_cache.h"
#include "network/msg_types.h"
#include "network/zone_redis_scripts.h"
#include "protocol/codec.h"
#include "runtime/shutdown.h"
#include "utils/log.h"
#include "utils/safe_utils.h"
#include "utils/time_utils.h"
#include <App.h>
#include <cstdint>
#include <hiredis.h>
#include "db/mysql_client.h"
#include "db/mysql_x_session.h"
#include "config/runtime_config.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <limits>
#include <memory>
#include <mutex>
#include <random>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include <algorithm>

namespace {

enum class ConnState {
  Connected,        // WebSocket が確立した直後
  Authenticating,   // enter_token を検証中
  Authed,           // トークンの検証を通過し、本人確認済み
  LoadingPlayer,    // プレイヤーデータを取得中
  LoadingScene,     // シーン／インスタンスを割り当て中
  InGame,           // ゲームに入場済み
  Closing
};
static std::string extractQueryParam(std::string_view query, const char *key) {
  if (!key || *key == '\0' || query.empty()) return "";
  const std::string wanted(key);
  size_t pos = 0;
  while (pos < query.size()) {
    size_t amp = query.find('&', pos);
    if (amp == std::string_view::npos) amp = query.size();
    auto kv = query.substr(pos, amp - pos);
    size_t eq = kv.find('=');
    auto k = (eq == std::string_view::npos) ? kv : kv.substr(0, eq);
    if (k == wanted) {
      return eq == std::string_view::npos ? "" : std::string(kv.substr(eq + 1));
    }
    pos = amp + 1;
  }
  return "";
}

static std::string extractJsonString(std::string_view body, const char *key) {
  std::string k = std::string("\"") + key + "\"";
  size_t p = body.find(k);
  if (p == std::string_view::npos) return "";
  p = body.find(':', p + k.size());
  if (p == std::string_view::npos) return "";
  ++p;
  while (p < body.size() && std::isspace(static_cast<unsigned char>(body[p]))) ++p;
  if (p >= body.size() || body[p] != '"') return "";
  ++p;
  std::string out;
  while (p < body.size()) {
    char c = body[p++];
    if (c == '\\' && p < body.size()) { out.push_back(body[p++]); continue; }
    if (c == '"') break;
    out.push_back(c);
  }
  return out;
}

static std::string jsonEscape(const std::string &s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
    case '\\': out += "\\\\"; break;
    case '"':  out += "\\\""; break;
    case '\n': out += "\\n"; break;
    case '\r': out += "\\r"; break;
    case '\t': out += "\\t"; break;
    default: out.push_back(c);
    }
  }
  return out;
}

static uint64_t nowEpochMs() {
  return timeutil::wall_now_ms();
}

// ── キャラクター ID／名前の生成（auth_worker のロジックと同じ）────────────────────

// role_id を生成する：3 桁の乱数 + タイムスタンプ（秒）。英字を含まない数字のみ
// 例：48717... → "487" + "1743072345"
static std::string generateRoleIdDatetime() {
  static thread_local std::mt19937 rng(std::random_device{}());
    // 3 桁の乱数
  std::uniform_int_distribution<int> dist(100, 999);
  int prefix = dist(rng);
  uint64_t ts = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
  return std::to_string(prefix) + std::to_string(ts);
}

static std::string generateDefaultRoleName() {
  static thread_local std::mt19937 rng(std::random_device{}());
    // 6 桁の乱数
  std::uniform_int_distribution<int> dist(100000, 999999);
  return "Player" + std::to_string(dist(rng)) + "MT";
}

// ── MySQL 接続の管理 ──────────────────────────────────────────────

struct ZoneDbRuntime {
  std::unique_ptr<db::Session> sess;
  std::string host, user, password, dbName;
  int port = 3306;
};
static thread_local ZoneDbRuntime zoneDbRt;

static bool ensureDbConnected(ZoneDbRuntime &rt) {
  if (rt.sess) {
    try {
      rt.sess->sql("SELECT 1").execute();
      return true;
    } catch (...) {
      rt.sess.reset();
    }
  }
  while (!rt.sess && !runtime_shutdown::isShutdownRequested()) {
    try {
      rt.sess = std::make_unique<db::Session>(
          rt.host.c_str(), rt.port, rt.user.c_str(), rt.password.c_str(),
          rt.dbName.c_str(), db::tlsRequiredFromConfig());
      zone_log::info("zone_http mysql connected %s db=%s",
                     rt.host.c_str(), rt.dbName.c_str());
    } catch (const db::Error &e) {
      rt.sess.reset();
      zone_log::warning("zone_http mysql connect failed: %s", e.what());
      std::this_thread::sleep_for(std::chrono::seconds(2));
    }
  }
  return rt.sess != nullptr;
}

// ── Redis による join チケット／passToken の検証 ──────────────────────────

static thread_local redisContext *gZoneRedis = nullptr;
static thread_local std::string gRedisHost;
static thread_local int gRedisPort = 6379;
static thread_local int gRedisDb = 0;
static thread_local std::string gRedisUser;
static thread_local std::string gRedisPassword;

enum class RedisTicketConsumeResult {
  Consumed,
  TicketNotFound,
  ValueMismatch,
  RedisError,
};

enum class RedisPassTokenConsumeResult {
  Consumed,
  TokenNotFound,
  BadValue,
  RedisError,
};

static void ensureRedis() {
  if (gZoneRedis) {
    auto *reply = (redisReply *)redisCommand(gZoneRedis, "PING");
    if (reply && reply->type == REDIS_REPLY_STATUS) {
      freeReplyObject(reply);
      return;
    }
    if (reply) freeReplyObject(reply);
    redisFree(gZoneRedis);
    gZoneRedis = nullptr;
  }
  gZoneRedis = redisConnect(gRedisHost.c_str(), gRedisPort);
  if (!gZoneRedis || gZoneRedis->err) {
    if (gZoneRedis) {
      zone_log::warning("zone_http redis connect failed: %s", gZoneRedis->errstr);
      redisFree(gZoneRedis);
      gZoneRedis = nullptr;
    }
    return;
  }
  if (!gRedisPassword.empty()) {
    redisReply *reply = nullptr;
    if (!gRedisUser.empty()) {
      reply = (redisReply *)redisCommand(gZoneRedis, "AUTH %s %s",
                                         gRedisUser.c_str(),
                                         gRedisPassword.c_str());
    } else {
      reply = (redisReply *)redisCommand(gZoneRedis, "AUTH %s",
                                         gRedisPassword.c_str());
    }
    if (!reply) {
      zone_log::error("zone_http redis AUTH failed: no reply");
      redisFree(gZoneRedis);
      gZoneRedis = nullptr;
      return;
    }
    if (reply->type == REDIS_REPLY_ERROR) {
      zone_log::error("zone_http redis AUTH failed: %.*s",
                      static_cast<int>(reply->len), reply->str ? reply->str : "");
      freeReplyObject(reply);
      redisFree(gZoneRedis);
      gZoneRedis = nullptr;
      return;
    }
    const bool authed = reply->type == REDIS_REPLY_STATUS;
    freeReplyObject(reply);
    if (!authed) {
      zone_log::error("zone_http redis AUTH failed: bad response");
      redisFree(gZoneRedis);
      gZoneRedis = nullptr;
      return;
    }
  }
  // SELECT db
  if (gRedisDb != 0) {
    auto *reply = (redisReply *)redisCommand(gZoneRedis, "SELECT %d", gRedisDb);
    if (!reply) {
      zone_log::error("zone_http redis SELECT %d failed: no reply", gRedisDb);
      redisFree(gZoneRedis);
      gZoneRedis = nullptr;
      return;
    }
    if (reply->type == REDIS_REPLY_ERROR) {
      zone_log::error("zone_http redis SELECT %d failed: %.*s",
                      gRedisDb, static_cast<int>(reply->len), reply->str ? reply->str : "");
      freeReplyObject(reply);
      redisFree(gZoneRedis);
      gZoneRedis = nullptr;
      return;
    }
    const bool selected = reply->type == REDIS_REPLY_STATUS;
    freeReplyObject(reply);
    if (!selected) {
      zone_log::error("zone_http redis SELECT %d failed: bad response", gRedisDb);
      redisFree(gZoneRedis);
      gZoneRedis = nullptr;
      return;
    }
  }
  zone_log::info("zone_http redis connected %s:%d", gRedisHost.c_str(), gRedisPort);
}

static std::string makeExpectedJoinTicketRedisValue(
    const zone_auth::DecryptedJoinTicket &ticket) {
  std::string value;
  value.reserve(1 + ticket.uidLen + 1);
  value.push_back(static_cast<char>(ticket.uidLen));
  value.append(reinterpret_cast<const char *>(ticket.uid.data()), ticket.uidLen);
  value.push_back(static_cast<char>(ticket.zoneId));
  return value;
}

/**
 * @brief gateway の join チケットをアトミックに消費し、収容数の状態を更新する
 *
 * @param ticket AES-GCM の認証を通過したチケットのペイロード。
 * @param expectedRedisValue uid_len + uid + zone_id。gateway が Redis に書き込んだ値と完全に一致していること。
 *
 * @retval Consumed チケットは GETDEL、予約は ZREM 済みで、zone:<id>:online をインクリメントした。
 * @retval TicketNotFound チケットが存在しない。期限切れ、使用済み、または Redis の DB の不一致が考えられる。
 * @retval ValueMismatch Redis の値とチケットのペイロードが一致しない。チケットと予約は削除済みだが、online は増やしていない。
 * @retval RedisError Redis の接続、コマンド、または応答の型でエラーが起きた。
 *
 * @note
 * - 成功した場合、現在の WebSocket セッションは切断時に zone:<id>:online を必ずデクリメントすること。
 */
static RedisTicketConsumeResult consumeJoinTicketAndMarkOnline(
    const zone_auth::DecryptedJoinTicket &ticket,
    const std::string &expectedRedisValue) {
  ensureRedis();
  if (!gZoneRedis) {
    return RedisTicketConsumeResult::RedisError;
  }

  const std::string reservationKey = zone_redis::makeZoneReservationsKey(ticket.zoneId);
  const std::string onlineKey = zone_redis::makeZoneOnlineKey(ticket.zoneId);
  auto *reply = (redisReply *)redisCommand(
      gZoneRedis, "EVAL %s 3 %b %s %s %b",
      zone_redis::kConsumeJoinTicketScript,
      reinterpret_cast<const char *>(ticket.ticketKey.data()),
      static_cast<size_t>(ticket.ticketKeyLen),
      reservationKey.c_str(),
      onlineKey.c_str(),
      expectedRedisValue.data(),
      expectedRedisValue.size());

  if (!reply) {
    if (gZoneRedis) {
      redisFree(gZoneRedis);
      gZoneRedis = nullptr;
    }
    zone_log::error("zone_http join ticket redis consume failed: no reply");
    return RedisTicketConsumeResult::RedisError;
  }

  if (reply->type == REDIS_REPLY_ERROR) {
    zone_log::error("zone_http join ticket redis consume failed: %.*s",
                    static_cast<int>(reply->len), reply->str ? reply->str : "");
    freeReplyObject(reply);
    return RedisTicketConsumeResult::RedisError;
  }

  if (reply->type != REDIS_REPLY_ARRAY || reply->elements != 2 ||
      !reply->element || !reply->element[0] ||
      reply->element[0]->type != REDIS_REPLY_INTEGER) {
    zone_log::error("zone_http join ticket redis consume failed: bad response");
    freeReplyObject(reply);
    return RedisTicketConsumeResult::RedisError;
  }

  const long long status = reply->element[0]->integer;
  freeReplyObject(reply);
  if (status == 2) return RedisTicketConsumeResult::Consumed;
  if (status == 0) return RedisTicketConsumeResult::TicketNotFound;
  if (status == 1) return RedisTicketConsumeResult::ValueMismatch;

  zone_log::error("zone_http join ticket redis consume failed: status=%lld", status);
  return RedisTicketConsumeResult::RedisError;
}

/**
 * @brief gateway の online から現在の WebSocket セッションを外す
 *
 * @param zoneId 現在の接続が計上されている zones.id。
 *
 * @note
 * - countedOnline=true の接続に対してのみ呼ぶ。
 * - この後始末はベストエフォート。失敗した場合は、運用や後続のヘルスチェックで online を修正する必要がある。
 */
static void unmarkZoneOnline(uint32_t zoneId) {
  if (zoneId == 0) return;
  ensureRedis();
  if (!gZoneRedis) return;

  const std::string onlineKey = zone_redis::makeZoneOnlineKey(zoneId);
  auto *reply = (redisReply *)redisCommand(
      gZoneRedis, "EVAL %s 1 %s", zone_redis::kDecrementZoneOnlineScript,
      onlineKey.c_str());
  if (!reply) {
    redisFree(gZoneRedis);
    gZoneRedis = nullptr;
    zone_log::warning("zone_http zone online decrement failed: no reply");
    return;
  }
  if (reply->type == REDIS_REPLY_ERROR) {
    zone_log::warning("zone_http zone online decrement failed: %.*s",
                      static_cast<int>(reply->len), reply->str ? reply->str : "");
  }
  freeReplyObject(reply);
}

/**
 * @brief gateway の /join が発行した暗号化チケットを検証する
 *
 * @param methodIdText クライアントが /join から受け取ってそのまま転送した methodId のクエリ文字列。
 * @param ticketHex クライアントが /join から受け取ってそのまま転送した、暗号化チケットの 16 進数テキスト。
 * @param cfg このゾーンサーバーの設定。zoneId と ticketCryptoKeys が初期化済みであること。
 * @param outUid 成功時に users.uid を書き込む。
 *
 * @retval true チケットの認証、有効期限、zoneId、Redis の 1 回限りの値のすべてを通過した。
 * @retval false ハンドシェイクを拒否しなければならない。
 *
 * @note
 * - 成功時は Redis のチケットキーを GETDEL するので、同じチケットは 1 回しか使えない。
 * - 失敗時のログにはチケットの平文も暗号文も出力しない。短期間有効な資格情報を漏らさないため。
 */
static bool validateJoinTicket(const std::string &methodIdText,
                               const std::string &ticketHex,
                               const zone::ZoneConfig &cfg,
                               std::string &outUid) {
  if (cfg.zoneId == 0 || methodIdText.empty() || ticketHex.empty()) {
    return false;
  }

  uint32_t methodId32 = 0;
  for (char ch : methodIdText) {
    if (!std::isdigit(static_cast<unsigned char>(ch))) {
      zone_log::error("zone_http join ticket failed: BadMethodId");
      return false;
    }
    methodId32 = methodId32 * 10u + static_cast<uint32_t>(ch - '0');
    if (methodId32 > 255) {
      zone_log::error("zone_http join ticket failed: BadMethodId");
      return false;
    }
  }
  if (methodId32 == 0) {
    zone_log::error("zone_http join ticket failed: BadMethodId");
    return false;
  }

  std::array<uint8_t, zone_auth::kEncryptedTicketMaxBytes> encrypted{};
  size_t encryptedLen = 0;
  if (!zone_auth::decodeHexBytes(ticketHex,
                                 std::span<uint8_t>(encrypted.data(), encrypted.size()),
                                 encryptedLen)) {
    zone_log::error("zone_http join ticket failed: BadHex");
    return false;
  }

  zone_auth::DecryptedJoinTicket ticket{};
  zone_auth::JoinTicketError err = zone_auth::decryptEncryptedJoinTicket(
      static_cast<uint8_t>(methodId32),
      cfg.ticketCryptoKeys,
      std::span<const uint8_t>(encrypted.data(), encryptedLen),
      ticket);
  if (err != zone_auth::JoinTicketError::Ok) {
    zone_log::error("zone_http join ticket failed: %s",
                    zone_auth::joinTicketErrorName(err));
    return false;
  }

  if (ticket.expiresAtMs <= 0 ||
      static_cast<uint64_t>(ticket.expiresAtMs) <= nowEpochMs()) {
    zone_log::error("zone_http join ticket failed: TicketExpired");
    return false;
  }
  if (ticket.zoneId != cfg.zoneId) {
    zone_log::error("zone_http join ticket failed: ZoneMismatch self_zone=%u",
                    cfg.zoneId);
    return false;
  }

  const std::string expectedRedisValue = makeExpectedJoinTicketRedisValue(ticket);
  const RedisTicketConsumeResult consumeResult =
      consumeJoinTicketAndMarkOnline(ticket, expectedRedisValue);
  if (consumeResult == RedisTicketConsumeResult::TicketNotFound) {
    zone_log::error("zone_http join ticket failed: TicketNotFound");
    return false;
  }
  if (consumeResult == RedisTicketConsumeResult::ValueMismatch) {
    zone_log::error("zone_http join ticket failed: RedisValueMismatch");
    return false;
  }
  if (consumeResult != RedisTicketConsumeResult::Consumed) {
    return false;
  }

  outUid.assign(reinterpret_cast<const char *>(ticket.uid.data()), ticket.uidLen);
  return !outUid.empty();
}

// ── Redis による passToken（セッション用チケット）の管理 ───────────────────────────────

/**
 * @brief passToken を消費せずに読み取る
 *
 * @note
 * - WebSocket の upgrade 時の事前チェック専用。本当の 1 回限りの消費は、RequestEnterZone で
 *   新しいトークンを書き込んだ後に consumePassTokenAndMarkOnline を呼んで行うこと。
 */
static bool readPassTokenRecord(const std::string &token,
                                uint32_t zoneId,
                                zone_auth::PassTokenRecord &out) {
  if (!zone_auth::isValidPassTokenText(token) || zoneId == 0) return false;
  ensureRedis();
  if (!gZoneRedis) return false;

  const std::string key = zone_redis::makePassTokenKey(zoneId, token);
  auto *reply = (redisReply *)redisCommand(gZoneRedis, "GET %s", key.c_str());
  if (!reply) {
    redisFree(gZoneRedis);
    gZoneRedis = nullptr;
    return false;
  }
  if (reply->type != REDIS_REPLY_STRING) {
    freeReplyObject(reply);
    return false;
  }

  std::string data(reply->str, reply->len);
  freeReplyObject(reply);
  return zone_auth::parsePassTokenRecord(data, zoneId, nowEpochMs(), out);
}

/**
 * @brief passToken を書き込む
 *
 * @note
 * - SET EX NX を使う。トークンに固定の TTL を持たせ、ごく低い確率の乱数の衝突で既存のセッションの資格情報を上書きしないようにする。
 */
static bool writePassToken(const std::string &token,
                           const zone_auth::PassTokenRecord &rec) {
  if (!zone_auth::isValidPassTokenText(token) || rec.zoneId == 0) return false;
  ensureRedis();
  if (!gZoneRedis) return false;

  const std::string value = zone_auth::serializePassTokenRecord(rec);
  const std::string key = zone_redis::makePassTokenKey(rec.zoneId, token);
  auto *reply = (redisReply *)redisCommand(gZoneRedis,
      "SET %s %b EX %u NX", key.c_str(),
      value.data(), value.size(),
      static_cast<unsigned int>(runtime_config::PASS_TOKEN_SEC));
  if (!reply) {
    redisFree(gZoneRedis);
    gZoneRedis = nullptr;
    return false;
  }
  const bool ok = reply->type == REDIS_REPLY_STATUS &&
                  reply->str && std::string_view(reply->str, reply->len) == "OK";
  freeReplyObject(reply);
  return ok;
}

/**
 * @brief 新しい passToken を発行する
 *
 * @param rec Redis のレコード。zoneId/uid/role/map/expireAtMs が設定済みであること。
 * @param outToken 成功時に、クライアントに渡すトークンを書き込む。
 *
 * @retval true トークンを Redis に書き込んだ。
 * @retval false CSPRNG、Redis、または衝突時のリトライに失敗した。
 */
static bool issuePassToken(const zone_auth::PassTokenRecord &rec,
                           std::string &outToken) {
  for (int attempt = 0; attempt < 4; ++attempt) {
    std::string token;
    if (!zone_auth::generatePassToken(token)) {
      return false;
    }
    if (writePassToken(token, rec)) {
      outToken = std::move(token);
      return true;
    }
  }
  outToken.clear();
  return false;
}

/**
 * @brief passToken をアトミックに消費し、online に計上する
 *
 * @param token クライアントの再接続クエリに含まれる passToken。呼び出し中だけ借用する。
 * @param zoneId このゾーンサーバーに対応する zones.id。
 * @param out 成功時に、セッションの復元に必要なフィールドを書き込む。
 *
 * @retval Consumed passToken を GETDEL し、zone:<id>:online をインクリメントした。out は有効。
 * @retval TokenNotFound トークンが存在しない、期限切れ、または別の接続が消費済み。
 * @retval BadValue Redis の値の形式が不正。online はベストエフォートでロールバック済み。
 * @retval RedisError Redis の接続、コマンド、または応答の型でエラーが起きた。
 *
 * @note
 * - 成功した場合、現在の WebSocket セッションは切断時に zone:<id>:online を必ずデクリメントすること。
 */
static RedisPassTokenConsumeResult consumePassTokenAndMarkOnline(
    const std::string &token,
    uint32_t zoneId,
    zone_auth::PassTokenRecord &out) {
  if (!zone_auth::isValidPassTokenText(token) || zoneId == 0) {
    return RedisPassTokenConsumeResult::TokenNotFound;
  }
  ensureRedis();
  if (!gZoneRedis) {
    return RedisPassTokenConsumeResult::RedisError;
  }

  const std::string key = zone_redis::makePassTokenKey(zoneId, token);
  const std::string onlineKey = zone_redis::makeZoneOnlineKey(zoneId);
  auto *reply = (redisReply *)redisCommand(
      gZoneRedis, "EVAL %s 2 %s %s",
      zone_redis::kConsumePassTokenScript,
      key.c_str(),
      onlineKey.c_str());

  if (!reply) {
    redisFree(gZoneRedis);
    gZoneRedis = nullptr;
    zone_log::error("zone_http passToken redis consume failed: no reply");
    return RedisPassTokenConsumeResult::RedisError;
  }

  if (reply->type == REDIS_REPLY_ERROR) {
    zone_log::error("zone_http passToken redis consume failed: %.*s",
                    static_cast<int>(reply->len), reply->str ? reply->str : "");
    freeReplyObject(reply);
    return RedisPassTokenConsumeResult::RedisError;
  }

  if (reply->type != REDIS_REPLY_ARRAY || reply->elements != 2 ||
      !reply->element || !reply->element[0] ||
      reply->element[0]->type != REDIS_REPLY_INTEGER) {
    zone_log::error("zone_http passToken redis consume failed: bad response");
    freeReplyObject(reply);
    return RedisPassTokenConsumeResult::RedisError;
  }

  const long long status = reply->element[0]->integer;
  if (status == 0) {
    freeReplyObject(reply);
    return RedisPassTokenConsumeResult::TokenNotFound;
  }
  if (status != 1 || !reply->element[1] ||
      reply->element[1]->type != REDIS_REPLY_STRING) {
    zone_log::error("zone_http passToken redis consume failed: status=%lld",
                    status);
    freeReplyObject(reply);
    return RedisPassTokenConsumeResult::RedisError;
  }

  // Redis reply owns reply->str; copy before free so parsing never reads
  // hiredis memory after freeReplyObject().
  std::string data(reply->element[1]->str, reply->element[1]->len);
  freeReplyObject(reply);

  if (!zone_auth::parsePassTokenRecord(data, zoneId, nowEpochMs(), out)) {
    unmarkZoneOnline(zoneId);
    return RedisPassTokenConsumeResult::BadValue;
  }
  return RedisPassTokenConsumeResult::Consumed;
}

/**
 * @brief 現在のセッションが持つ passToken を無効化する
 *
 * @param zoneId 現在のセッションが online に計上されている gateway の zones.id。
 * @param token 現在のセッションが EnterZone に成功した後に保存した新しい passToken。
 *
 * @retval true Redis の DEL が整数の応答を返した。トークンは削除済み、またはもともと存在しない。
 * @retval false 引数が不正、Redis が使えない、または Redis がエラーを返した。呼び出し側はそのまま切断時の後始末を続けること。
 *
 * @note
 * - online に計上済みのセッションが正常に切断されたときだけ呼ぶ。
 * - 再接続の upgrade の事前チェック段階で保存しているのは古いトークン。EnterZone 前に切断された場合は無効化してはならない。
 */
static bool revokePassToken(uint32_t zoneId, const std::string &token) {
  if (zoneId == 0 || !zone_auth::isValidPassTokenText(token)) return false;
  ensureRedis();
  if (!gZoneRedis) return false;
  std::string key = zone_redis::makePassTokenKey(zoneId, token);
  auto *reply = (redisReply *)redisCommand(gZoneRedis, "DEL %s", key.c_str());
  if (!reply) {
    redisFree(gZoneRedis);
    gZoneRedis = nullptr;
    return false;
  }
  if (reply->type == REDIS_REPLY_INTEGER) {
    freeReplyObject(reply);
    return true;
  }
  if (reply->type == REDIS_REPLY_ERROR) {
    zone_log::warning("zone_http passToken revoke failed: %.*s",
                      static_cast<int>(reply->len), reply->str ? reply->str : "");
  } else {
    zone_log::warning("zone_http passToken revoke failed: bad response");
  }
  freeReplyObject(reply);
  return false;
}

static void insertDefaultRoleChildren(db::Session &sess,
                                      const std::string &db,
                                      const std::string &roleId) {
  for (uint32_t unitId = 1; unitId <= 3; ++unitId) {
    sess.sql("INSERT IGNORE INTO `" + db +
             "`.role_units(role_id, unit_id, level) VALUES(?, ?, 1)")
        .bind(roleId, unitId).execute();
  }

  for (uint32_t slot = 0; slot <= 2; ++slot) {
    sess.sql("INSERT IGNORE INTO `" + db +
             "`.team_slots(role_id, team_id, slot_index, unit_id) "
             "VALUES(?, 1, ?, ?)")
        .bind(roleId, slot, slot + 1).execute();
  }
}

/**
 * @brief キャラクターのバッグを読み込む
 *
 * @param sess 接続済みの MySQL セッション。借用して検索する。
 * @param db データベース名。
 * @param roleId キャラクター ID。role_items.role_id は BIGINT だが、バインド時に MySQL 側で
 *               文字列と数値の比較変換が行われるので、ここでは文字列のままバインドする。
 * @param[out] out 呼び出し側が所有するバッグの状態。内容は上書きされる。
 *
 * @note
 * - ログイン時に 1 回だけ呼び、結果は SessionRegister と一緒にゲームワーカーへムーブする。
 * - count <= 0 の古い不正データは捨て、不正な数量を実行時の状態に持ち込まない。
 */
static void loadRoleInventory(db::Session &sess, const std::string &db,
                              const std::string &roleId,
                              inventory::RoleInventory &out) {
  out.stacks.clear();
  auto res = sess.sql(
      "SELECT item_id, count FROM `" + db + "`.role_items "
      "WHERE role_id = ? ORDER BY item_id")
      .bind(roleId).execute();

  for (auto row = res.fetchOne(); row; row = res.fetchOne()) {
    const uint32_t itemId = utils::safeToU32(row[0], 0);
    const int32_t  count  = utils::safeToI32(row[1], 0);
    if (itemId == 0 || count <= 0) continue;
    out.stacks.push_back(
        inventory::ItemStack{itemId, static_cast<uint32_t>(count)});
  }
}

/**
 * @brief キャラクターが所持する全精霊の装備スロットを読み込む
 *
 * @param sess 接続済みの MySQL セッション。借用して検索する。
 * @param db データベース名。
 * @param roleId キャラクター ID。unit_equips.role_id は BIGINT だが、文字列でバインドすれば MySQL 側で変換される。
 * @param[out] out 呼び出し側が所有する装備の状態。内容は上書きされる。
 *
 * @note
 * - ログイン時に 1 回だけ呼び、結果は SessionRegister と一緒にゲームワーカーへムーブする。
 * - スロットが範囲外、または item_id が 0 の不正な行は捨て、実行時の状態には入れない。
 */
static void loadRoleEquipment(db::Session &sess, const std::string &db,
                              const std::string &roleId,
                              equipment::RoleEquipment &out) {
  out.entries.clear();
  auto res = sess.sql(
      "SELECT unit_id, slot, item_id FROM `" + db + "`.unit_equips "
      "WHERE role_id = ? ORDER BY unit_id, slot")
      .bind(roleId).execute();

  for (auto row = res.fetchOne(); row; row = res.fetchOne()) {
    const uint32_t unitId = utils::safeToU32(row[0], 0);
    const uint32_t slot   = utils::safeToU32(row[1], 0);
    const uint32_t itemId = utils::safeToU32(row[2], 0);
    if (unitId == 0 || itemId == 0) continue;
    if (slot < cache::kMinEquipSlot || slot > cache::kMaxEquipSlot) continue;
    out.entries.push_back(equipment::EquipSlotEntry{
        unitId, static_cast<uint8_t>(slot), itemId});
  }
}

/**
 * @brief キャラクターのメインシティの建物を読み込む。新規キャラクターにはデフォルトの建物を補う
 *
 * @param sess 接続済みの MySQL セッション。借用して検索と挿入を行う。
 * @param db データベース名。
 * @param roleId キャラクター ID。role_buildings.role_id は VARCHAR なので文字列のままバインドする。
 * @param[out] out 呼び出し側が所有する建物の状態。内容は上書きされる。
 *
 * @note
 * - ログイン時に 1 回だけ呼ぶ。
 * - キャラクターの建物レコードが 1 件もない場合は、buildings.is_default_built に従ってデフォルトの建物を補って DB に保存する。
 *   座標は buildings.default_x/default_y を使う。既にレコードがあるキャラクターには補わない。
 *   プレイヤーが配置済みの位置を上書きしないため。
 */
static void loadRoleBuildings(db::Session &sess, const std::string &db,
                              const std::string &roleId,
                              buildings::RoleBuildings &out) {
  out.entries.clear();

  auto load = [&]() {
    auto res = sess.sql(
        "SELECT building_id, x, y, COALESCE(level, 1), COALESCE(state, 0) "
        "FROM `" + db + "`.role_buildings WHERE role_id = ? "
        "ORDER BY building_id")
        .bind(roleId).execute();
    for (auto row = res.fetchOne(); row; row = res.fetchOne()) {
      const uint32_t buildingId = utils::safeToU32(row[0], 0);
      if (buildingId == 0) continue;
      buildings::RoleBuilding e;
      e.buildingId = buildingId;
      e.x     = utils::safeToF32(row[1], 0.f);
      e.y     = utils::safeToF32(row[2], 0.f);
      e.level = utils::safeToU32(row[3], 1);
      e.state = static_cast<uint8_t>(utils::safeToU32(row[4], 0));
      out.entries.push_back(e);
    }
  };

  load();
  if (!out.entries.empty()) return;

    // 新規キャラクター：テンプレートに従ってデフォルトの建物を補う。INSERT IGNORE なので、同時に重複ログインしても互いに壊さない。
  try {
    sess.sql(
        "INSERT IGNORE INTO `" + db + "`.role_buildings"
        "(role_id, building_id, x, y, level, state) "
        "SELECT ?, id, COALESCE(default_x, 0), COALESCE(default_y, 0), "
        "       COALESCE(base_level, 1), 1 "
        "FROM `" + db + "`.buildings WHERE COALESCE(is_default_built, 0) = 1")
        .bind(roleId).execute();
  } catch (const db::Error &e) {
    zone_log::warning("zone_http default role_buildings init failed: %s",
                      e.what());
    return;
  }
  load();
}

/**
 * @brief キャラクターが所持するすべての精霊を読み込む
 *
 * @param sess 接続済みの MySQL セッション。借用して検索する。
 * @param db データベース名。
 * @param roleId キャラクター ID。role_units.role_id は VARCHAR なので文字列のままバインドする。
 * @param[out] out 呼び出し側が所有するリスト。内容は上書きされ、unit_id の昇順に並ぶ。
 *
 * @note
 * - ログイン時に 1 回だけ呼ぶ。
 * - 結果は次の用途に使う：募集の重複チェック、募集リストの owned フラグ、そしてマップ上に
 *   所持している全精霊を生成すること（待機中の精霊もその場にいないと、フロントエンドで選んで出撃させられない）。
 * - ここでの slotIndex には意味がない。実際のスロットは roleDto.teams を正とする。
 */
static void loadOwnedUnits(db::Session &sess, const std::string &db,
                           const std::string &roleId,
                           std::vector<codec::TeamUnitInfoDto> &out) {
  out.clear();
  auto res = sess.sql(
      "SELECT ru.unit_id, COALESCE(ru.level, 1), COALESCE(ru.exp, 0), "
      "       COALESCE(rua.area_id, 1), COALESCE(ut.unit_name, ''), "
      "       COALESCE(ut.base_hp, 0), COALESCE(ut.base_atk, 0) "
      "FROM `" + db + "`.role_units ru "
      "LEFT JOIN `" + db + "`.role_unit_area_assignments rua "
      "  ON rua.role_id = ru.role_id AND rua.unit_id = ru.unit_id "
      "LEFT JOIN `" + db + "`.unit_templates ut ON ut.unit_id = ru.unit_id "
      "WHERE ru.role_id = ? ORDER BY ru.unit_id")
      .bind(roleId).execute();

  for (auto row = res.fetchOne(); row; row = res.fetchOne()) {
    const uint32_t unitId = utils::safeToU32(row[0], 0);
    if (unitId == 0) continue;
    codec::TeamUnitInfoDto u{};
    u.unitId = unitId;
    u.runtimeLevel = utils::safeToU32(row[1], 1);
    u.runtimeExp = utils::safeToU64(row[2], 0);
    u.runtimeAreaId = utils::safeToU32(row[3], 1);
    u.hasRuntimePersistence = true;
    u.unitName = utils::safeToStr(row[4]);
    u.baseHp = utils::safeToU32(row[5], 0);
    u.baseAtk = utils::safeToU32(row[6], 0);
    out.push_back(u);
  }
}

static bool loadRoleDtoByUserId(db::Session &sess,
                                const std::string &db,
                                const std::string &uid,
                                codec::RoleInfoDto &out) {
  codec::RoleInfoDto next{};
  auto res = sess.sql(
      "SELECT role_id, role_name, "
      "COALESCE(gold, 0), COALESCE(stamina, 0), "
      "COALESCE(crystal, 0), COALESCE(combat_power, 0), "
      "COALESCE(current_map_id, 1), COALESCE(selected_team_id, 1) "
      "FROM `" + db + "`.roles WHERE user_id = ? LIMIT 1")
      .bind(uid).execute();
  auto row = res.fetchOne();
  if (!row) {
    return false;
  }

  next.roleId = std::string(row[0]);
  next.roleName = std::string(row[1]);
  next.gold = utils::safeToU32(row[2], 0);
  next.stamina = utils::safeToU32(row[3], 0);
  next.crystal = utils::safeToU32(row[4], 0);
  next.combatPower = utils::safeToU32(row[5], 0);
  next.currentMapId = static_cast<uint8_t>(utils::safeToU32(row[6], 1));
  next.selectedTeamId = static_cast<uint8_t>(utils::safeToU32(row[7], 1));
  next.selectedUnitId = 0;

  auto teamRes = sess.sql(
      "SELECT ts.team_id, ts.slot_index, ts.unit_id, "
      "COALESCE(ru.level, 1), COALESCE(ru.exp, 0), "
      "COALESCE(rua.area_id, 1), "
      "COALESCE(ut.unit_name, ''), COALESCE(ut.base_hp, 0), "
      "COALESCE(ut.base_atk, 0) "
      "FROM `" + db + "`.team_slots ts "
      "LEFT JOIN `" + db + "`.role_units ru "
      "  ON ru.role_id = ts.role_id AND ru.unit_id = ts.unit_id "
      "LEFT JOIN `" + db + "`.role_unit_area_assignments rua "
      "  ON rua.role_id = ts.role_id AND rua.unit_id = ts.unit_id "
      "LEFT JOIN `" + db + "`.unit_templates ut ON ut.unit_id = ts.unit_id "
      "WHERE ts.role_id = ? ORDER BY ts.team_id, ts.slot_index")
      .bind(next.roleId).execute();

  std::unordered_map<uint8_t, codec::TeamInfoDto> teamMap;
  for (auto trow = teamRes.fetchOne(); trow; trow = teamRes.fetchOne()) {
    uint8_t tid = static_cast<uint8_t>(utils::safeToU32(trow[0], 1));
    uint8_t slot = static_cast<uint8_t>(utils::safeToU32(trow[1], 0));
    uint32_t unitId = utils::safeToU32(trow[2], 0);
    auto &team = teamMap[tid];
    team.teamId = tid;
    codec::TeamUnitInfoDto unit{};
    unit.unitId = unitId;
    unit.slotIndex = slot;
    unit.runtimeLevel = utils::safeToU32(trow[3], 1);
    unit.runtimeExp = utils::safeToU64(trow[4], 0);
    unit.runtimeAreaId = utils::safeToU32(trow[5], 1);
    unit.hasRuntimePersistence = true;
    unit.unitName = utils::safeToStr(trow[6]);
    unit.baseHp = utils::safeToU32(trow[7], 0);
    unit.baseAtk = utils::safeToU32(trow[8], 0);
    team.units.push_back(unit);
  }
  for (auto &[_, team] : teamMap) {
    next.teams.push_back(std::move(team));
  }

  for (auto &team : next.teams) {
    std::sort(team.units.begin(), team.units.end(),
              [](auto &a, auto &b) { return a.slotIndex < b.slotIndex; });
    if (team.teamId == next.selectedTeamId && !team.units.empty()) {
      next.selectedUnitId = team.units[0].unitId;
    }
  }

  out = std::move(next);
  return true;
}

static bool ensureRoleInitializedForUser(db::Session &sess,
                                         const std::string &db,
                                         const std::string &uid) {
  std::string lastError;
  for (int attempt = 0; attempt < 3; ++attempt) {
    try {
      sess.startTransaction();

      auto res = sess.sql(
          "SELECT role_id FROM `" + db +
          "`.roles WHERE user_id = ? LIMIT 1 FOR UPDATE")
          .bind(uid).execute();
      auto row = res.fetchOne();

      std::string roleId;
      if (row) {
        roleId = std::string(row[0]);
      } else {
        roleId = generateRoleIdDatetime();
        const std::string roleName = generateDefaultRoleName();
        sess.sql("INSERT INTO `" + db +
                 "`.roles(user_id, role_id, role_name, current_map_id) "
                 "VALUES(?, ?, ?, 1)")
            .bind(uid, roleId, roleName).execute();
      }

      insertDefaultRoleChildren(sess, db, roleId);
      sess.commit();
      return true;
    } catch (const db::Error &e) {
      lastError = e.what();
      try { sess.rollback(); } catch (...) {}

            // 同時に作成した場合、別のリクエストが先にユニーク制約を満たしてコミットしていることがある。キャラクターを読み取れれば初期化は完了したとみなしてよい。
      try {
        codec::RoleInfoDto ignored;
        if (loadRoleDtoByUserId(sess, db, uid, ignored)) {
          return true;
        }
      } catch (...) {}
    }
  }

  zone_log::error("zone_http role init failed uid=%s error=%s",
                  uid.c_str(), lastError.c_str());
  return false;
}

// ── uid でキャラクターを検索／作成する（roles.user_id VARCHAR(64) 列）─────────────────
/**
 * @brief キャラクターを検索または初期化し、そのバッグを読み込む
 *
 * @param uid 外部のユーザー ID。
 * @param[out] out キャラクター情報の DTO。成功時に上書きされる。
 * @param[out] outInv キャラクターのバッグ。成功時に上書きされ、失敗時の内容は未定義。
 *
 * @retval false キャラクターの検索または初期化に失敗した。呼び出し側は今回のゾーン入場を拒否すること。
 */
static bool findOrCreateRoleDto(const std::string &uid, codec::RoleInfoDto &out,
                                inventory::RoleInventory &outInv,
                                equipment::RoleEquipment &outEquip,
                                buildings::RoleBuildings &outBuildings,
                                std::vector<codec::TeamUnitInfoDto> &outOwnedUnits) {
  if (!ensureDbConnected(zoneDbRt)) return false;
  auto &sess = *zoneDbRt.sess;
  const auto &db = zoneDbRt.dbName;

  try {
    if (!(loadRoleDtoByUserId(sess, db, uid, out) && !out.teams.empty())) {
      if (!ensureRoleInitializedForUser(sess, db, uid)) {
        return false;
      }
      if (!loadRoleDtoByUserId(sess, db, uid, out)) {
        return false;
      }
    }
    loadRoleInventory(sess, db, out.roleId, outInv);
    loadRoleEquipment(sess, db, out.roleId, outEquip);
    loadRoleBuildings(sess, db, out.roleId, outBuildings);
    loadOwnedUnits(sess, db, out.roleId, outOwnedUnits);
    return true;
  } catch (const db::Error &e) {
    zone_log::error("zone_http findOrCreateRoleDto error: %s", e.what());
    return false;
  }
}

// ── HTTP API のハンドラ ───────────────────────────────────────────

// GET /api/role?user_id=<uid>
static std::string handleApiRole(const std::string &user_id) {
  if (!ensureDbConnected(zoneDbRt)) {
    return R"({"status":"error","message":"database unavailable"})";
  }
  auto &sess = *zoneDbRt.sess;
  const auto &db = zoneDbRt.dbName;

  try {
    auto res = sess.sql(
        "SELECT COALESCE(role_id, '') AS role_id, "
        "COALESCE(role_name, '') AS role_name, "
        "CAST(COALESCE(current_map_id, 1) AS UNSIGNED) AS current_map_id, "
        "CAST(COALESCE(gold, 0) AS SIGNED) AS gold, "
        "CAST(COALESCE(stamina, 0) AS SIGNED) AS stamina, "
        "CAST(COALESCE(crystal, 0) AS SIGNED) AS crystal, "
        "CAST(COALESCE(combat_power, 0) AS SIGNED) AS combat_power "
        "FROM `" + db + "`.roles "
        "WHERE user_id = ? "
        "ORDER BY created_at ASC, role_id ASC LIMIT 1")
        .bind(user_id).execute();
    auto row = res.fetchOne();
    if (!row) {
      return R"({"status":"not_found"})";
    }
    std::string roleId   = std::string(row[0]);
    std::string roleName = std::string(row[1]);
    uint32_t currentMapId       = utils::safeToU32(row[2], 1);
        // これらの列はすべて INT UNSIGNED なので、safeToU32 で欠損なく読める。
        // int64_t に広げているのは、下で JSON を組み立てやすくするためだけ。
    const int64_t gold        = utils::safeToU32(row[3], 0);
    const int64_t stamina     = utils::safeToU32(row[4], 0);
    const int64_t crystal     = utils::safeToU32(row[5], 0);
    const int64_t combatPower = utils::safeToU32(row[6], 0);

    return std::string("{\"status\":\"ok\",\"data\":{") +
           "\"roleId\":\"" + jsonEscape(roleId) +
           "\",\"roleName\":\"" + jsonEscape(roleName) +
           "\",\"currentMapId\":" + std::to_string(currentMapId) +
           ",\"gold\":" + std::to_string(gold) +
           ",\"stamina\":" + std::to_string(stamina) +
           ",\"crystal\":" + std::to_string(crystal) +
           ",\"combatPower\":" + std::to_string(combatPower) + "}}";
  } catch (const db::Error &e) {
    return std::string("{\"status\":\"error\",\"message\":\"") +
           jsonEscape(e.what()) + "\"}";
  }
}

// POST /api/role/create { "user_id": "<uid>" }
static std::string handleApiRoleCreate(const std::string &user_id) {
  if (!ensureDbConnected(zoneDbRt)) {
    return R"({"status":"error","message":"database unavailable"})";
  }
  auto &sess = *zoneDbRt.sess;
  const auto &db = zoneDbRt.dbName;

  try {
    if (!ensureRoleInitializedForUser(sess, db, user_id)) {
      return R"({"status":"error","message":"role init failed"})";
    }
    return handleApiRole(user_id);
  } catch (const db::Error &e) {
    std::string retry = handleApiRole(user_id);
    if (retry.find("\"status\":\"ok\"") != std::string::npos) return retry;
    return std::string("{\"status\":\"error\",\"message\":\"") +
           jsonEscape(e.what()) + "\"}";
  }
}

// GET /api/map-respawn?map_id=1
static std::string handle_api_map_respawn(uint32_t map_id) {
  if (!ensureDbConnected(zoneDbRt)) {
    return R"({"status":"error","message":"database unavailable"})";
  }
  auto &sess = *zoneDbRt.sess;
  const auto &db = zoneDbRt.dbName;

  try {
    auto res = sess.sql(
        "SELECT CAST(COALESCE(respawn_x, 0) AS DOUBLE), "
        "CAST(COALESCE(respawn_y, 0) AS DOUBLE) "
        "FROM `" + db + "`.map_templates WHERE map_id = ? LIMIT 1")
        .bind(map_id).execute();
    auto row = res.fetchOne();
    if (!row) {
      return R"({"status":"not_found"})";
    }
    float rx = utils::safeToF32(row[0], 0.f);
    float ry = utils::safeToF32(row[1], 0.f);
    return std::string("{\"status\":\"ok\",\"data\":{") +
           "\"respawnX\":" + std::to_string(rx) +
           ",\"respawnY\":" + std::to_string(ry) + "}}";
  } catch (const db::Error &e) {
    return std::string("{\"status\":\"error\",\"message\":\"") +
           jsonEscape(e.what()) + "\"}";
  }
}

// ── WebSocket のヘルパー ────────────────────────────────────────────────────

using WS = uWS::WebSocket<false, true, SessionData>;
static std::atomic<uint64_t> gZoneWsSeq{0};
static constexpr size_t kRoleCreateBodyLimit = size_t{4} * size_t{1024};

struct ZoneNetWorkerState {
  std::atomic<uWS::Loop *> loop{nullptr};
  std::atomic_bool drainScheduled{false};
  moodycamel::ConcurrentQueue<OutboundMsg> outbound;
  std::unordered_map<uint64_t, WS *> connMap;
  std::shared_mutex connMtx;
};

static std::array<ZoneNetWorkerState, runtime_config::NUM_NET_WORKERS>
    gZoneNetWorkers;

static void drainWorkerOutbound(uint8_t workerId);

/**
 * @brief zone_http の shutdown 用タイマーのポーリング周期
 *
 * @note
 * - このタイマーは fallthrough しないポーリングで、shutdown が完了するまで uSockets のループを動かし続ける役割を持つ。
 */
static constexpr int kHttpShutdownPollMs = 100;

static ZoneNetWorkerState *zoneNetWorkerState(uint8_t workerId) {
  if (workerId >= runtime_config::NUM_NET_WORKERS) {
    return nullptr;
  }
  return &gZoneNetWorkers[workerId];
}

/**
 * @brief zone_http のループスレッドが持つ shutdown 制御用の状態
 *
 * @note
 * - listenSocket と timer はどちらも現在の uSockets のループに属するので、別スレッドから閉じてはならない。
 * - state はプロセス全体のワーカー枠を指す。run() から戻る前に、この構造体のアドレスを保存しないこと。
 */
struct HttpShutdownRuntime {
  uint8_t workerId = 0;
  ZoneNetWorkerState *state = nullptr;
  struct us_listen_socket_t *listenSocket = nullptr;
  struct us_timer_t *timer = nullptr;
  bool started = false;
};

/**
 * @brief 現在のワーカーの WebSocket ポインタのスナップショットをコピーする
 *
 * @param state ワーカーのネットワーク状態。null でもよい。
 *
 * @return 現在の connMap にある WebSocket ポインタの一覧。ポインタは zone_http のループの現在のコールバック内でだけ借用できる。
 *
 * @note
 * - 戻り値を非同期タスクに保存したり、uSockets のコールバックの寿命を超えて保持したりしないこと。
 */
static std::vector<WS *> snapshotHttpSockets(ZoneNetWorkerState *state) {
  std::vector<WS *> sockets;
  if (!state) return sockets;

  std::shared_lock lk(state->connMtx);
  sockets.reserve(state->connMap.size());
  for (auto &[_, ws] : state->connMap) {
    sockets.push_back(ws);
  }
  return sockets;
}

/**
 * @brief プロセスの shutdown 要求をポーリングし、HTTP ループを終了させる
 *
 * @param timer 現在の uSockets の shutdown 用タイマー。キューの処理が終わったら本関数が閉じる。
 *
 * @note
 * - uSockets から zone_http のループスレッド上でのみ呼ばれる。
 * - shutdown の開始後、少なくとも次のコールバックまではタイマーを残す。listen ソケットの close-list が
 *   ループの post 段階で確実に解放されるようにするため。
 */
static void pollHttpShutdown(struct us_timer_t *timer) {
  auto **runtimeSlot =
      reinterpret_cast<HttpShutdownRuntime **>(us_timer_ext(timer));
  HttpShutdownRuntime *runtime = runtimeSlot ? *runtimeSlot : nullptr;
  if (!runtime || !runtime_shutdown::isShutdownRequested()) {
    return;
  }

  auto sockets = snapshotHttpSockets(runtime->state);
  if (!runtime->started) {
    runtime->started = true;
    if (runtime->listenSocket) {
      us_listen_socket_close(0, runtime->listenSocket);
      runtime->listenSocket = nullptr;
    }

    for (WS *ws : sockets) {
      ws->end(1012, "server restart");
    }

    zone_log::info("zone_http:%u shutdown requested, closed listener and %zu ws",
                   runtime->workerId, sockets.size());
    return;
  }

  if (!sockets.empty()) {
    return;
  }

  runtime->timer = nullptr;
  us_timer_close(timer);
  zone_log::info("zone_http:%u shutdown drain completed", runtime->workerId);
}

static void scheduleWorkerOutboundDrain(uint8_t workerId) {
  auto *state = zoneNetWorkerState(workerId);
  if (!state) return;

  if (state->drainScheduled.exchange(true, std::memory_order_acq_rel)) {
    return;
  }

  uWS::Loop *loop = state->loop.load(std::memory_order_acquire);
  if (!loop) {
    state->drainScheduled.store(false, std::memory_order_release);
    return;
  }

  loop->defer([workerId]() {
    drainWorkerOutbound(workerId);
  });
}

static void drainWorkerOutbound(uint8_t workerId) {
  auto *state = zoneNetWorkerState(workerId);
  if (!state) return;

  OutboundMsg msg;
  while (state->outbound.try_dequeue(msg)) {
    std::shared_lock lk(state->connMtx);
    auto it = state->connMap.find(msg.connId);
    if (it != state->connMap.end()) {
      it->second->send(msg.payload, uWS::OpCode::BINARY);
    }
  }

  state->drainScheduled.store(false, std::memory_order_release);
  if (state->outbound.size_approx() > 0) {
    scheduleWorkerOutboundDrain(workerId);
  }
}

struct RoleCreateBodyState {
  std::string body;
  bool aborted = false;
  bool rejected = false;
};

static uint32_t parse_u32(std::string_view s, uint32_t fallback = 0) {
  if (s.empty()) return fallback;
  uint32_t parsed = 0;
  for (char c : s) {
    if (!std::isdigit(static_cast<unsigned char>(c))) return fallback;
    const uint32_t digit =
        static_cast<uint32_t>(static_cast<unsigned char>(c) -
                              static_cast<unsigned char>('0'));
    if (parsed > (std::numeric_limits<uint32_t>::max() - digit) / 10u) {
      return fallback;
    }
    parsed = parsed * 10u + digit;
  }
  return parsed;
}

template <typename ResT>
static void write_json(ResT *res, std::string_view json) {
  res->writeHeader("Access-Control-Allow-Origin", "*")
     ->writeHeader("Content-Type", "application/json")
     ->end(json);
}

template <typename ResT>
static void write_json_status(ResT *res, std::string_view status, std::string_view json) {
  res->writeStatus(status)
     ->writeHeader("Access-Control-Allow-Origin", "*")
     ->writeHeader("Content-Type", "application/json")
     ->end(json);
}

template <typename ResT, typename ReqT>
static bool requireInternalApiAuth(ResT *res, ReqT *req,
                                   std::string_view expectedToken) {
  if (expectedToken.empty()) {
    write_json_status(res, "503 Service Unavailable",
                      R"({"status":"error","message":"internal api token not configured"})");
    return false;
  }
  const std::string_view token = req->getHeader("x-zone-internal-token");
  if (token != expectedToken) {
    write_json_status(res, "403 Forbidden",
                      R"({"status":"error","message":"forbidden"})");
    return false;
  }
  return true;
}

} // namespace

// ── network::send() のゾーンサーバー版の実装（gw_bridge 版を置き換える）──────────
// zone が zone_http（直接接続モード）を使う場合、send は connId が属するネットワークワーカーに投入する。
void network::send(uint64_t conn_id, std::string payload) {
  const uint8_t workerId = netWorkerOf(conn_id);
  auto *state = zoneNetWorkerState(workerId);
  if (!state) {
    return;
  }

  state->outbound.enqueue(OutboundMsg{conn_id, 0, std::move(payload)});
  scheduleWorkerOutboundDrain(workerId);
}

// ── メイン関数 ──────────────────────────────────────────────────────

void zone_http::run(uint8_t workerId, const zone::ZoneConfig &cfg) {
    // スレッドローカルの DB を初期化する
  zoneDbRt.host = cfg.mysqlHost;
  zoneDbRt.port = cfg.mysqlPort;
  zoneDbRt.user = cfg.mysqlUser;
  zoneDbRt.password = cfg.mysqlPasswd;
  zoneDbRt.dbName = cfg.mysqlDb;
  if (!ensureDbConnected(zoneDbRt)) {
    zone_log::info("zone_http:%u stopped before listen", workerId);
    return;
  }

    // Redis を初期化する
  gRedisHost = cfg.redisHost;
  gRedisPort = cfg.redisPort;
  gRedisDb = cfg.redisDb;
  gRedisUser = cfg.redisUser;
  gRedisPassword = cfg.redisPassword;
  ensureRedis();

  int port = cfg.listenPort;
  const char* host = cfg.listenHost;
    // ゲームワーカーの受信キュー用の producer トークン
  std::vector<moodycamel::ProducerToken> ptokArr;
  ptokArr.reserve(runtime_config::NUM_GAME_WORKERS);
  for (uint8_t i = 0; i < runtime_config::NUM_GAME_WORKERS; i++) {
    ptokArr.emplace_back(gInbound[i]);
  }

  auto app = uWS::App();

    // ── CORS ──
  app.options("/*", [](auto *res, auto *) {
    res->writeHeader("Access-Control-Allow-Origin", "*")
       ->writeHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
       ->writeHeader("Access-Control-Allow-Headers", "Content-Type, X-Zone-Internal-Token")
       ->writeStatus("204 No Content")
       ->end();
  });

  const std::string internalApiToken = cfg.internalApiToken;

    // ── HTTP API：キャラクター照会 ──
  app.get("/api/role", [internalApiToken](auto *res, auto *req) {
    if (!requireInternalApiAuth(res, req, internalApiToken)) return;
    std::string user_id = extractQueryParam(req->getQuery(), "user_id");
    if (user_id.empty()) {
      write_json(res, R"({"status":"error","message":"user_id required"})");
      return;
    }
    write_json(res, handleApiRole(user_id));
  });

    // ── HTTP API：キャラクター作成 ──
  app.post("/api/role/create", [internalApiToken](auto *res, auto *req) {
    if (!requireInternalApiAuth(res, req, internalApiToken)) return;
    auto state = std::make_shared<RoleCreateBodyState>();
    res->onAborted([state]() {
      state->aborted = true;
    });
    res->onData([res, state](std::string_view chunk, bool last) {
      if (state->aborted || state->rejected) return;

      if (state->body.size() + chunk.size() > kRoleCreateBodyLimit) {
        state->rejected = true;
        write_json_status(res, "413 Payload Too Large",
                          R"({"status":"error","message":"request body too large"})");
        return;
      }

      state->body.append(chunk.data(), chunk.size());
      if (!last) return;
      std::string userId = extractJsonString(state->body, "user_id");
      if (userId.empty()) {
        write_json(res, R"({"status":"error","message":"user_id required"})");
        return;
      }
      write_json(res, handleApiRoleCreate(userId));
    });
  });

    // ── HTTP API：企画用設定テーブルのホットリロード ──
    // 読み込みと検証はすべてこのスレッドで行い、成功してからゲームワーカーへ渡す。
    // そのため失敗しても本番環境は古い設定のまま動き、中途半端な設定や壊れた設定が有効になることはない。
  app.post("/api/reload-config", [internalApiToken, &cfg](auto *res, auto *req) {
    if (!requireInternalApiAuth(res, req, internalApiToken)) return;
    res->onAborted([]() {});

    if (config_reload::requestReload(cfg)) {
      write_json(res,
                 R"({"status":"ok","message":"config reloaded; takes effect next tick"})");
    } else {
      write_json_status(res, "409 Conflict",
                        R"({"status":"error","message":"config validation failed; old config still active, see server log"})");
    }
  });

    // ── HTTP API：マップのリスポーン地点照会 ──
  app.get("/api/map-respawn", [internalApiToken](auto *res, auto *req) {
    if (!requireInternalApiAuth(res, req, internalApiToken)) return;
    std::string mapIdStr = extractQueryParam(req->getQuery(), "map_id");
    if (mapIdStr.empty()) {
      write_json(res, R"({"status":"error","message":"map_id required"})");
      return;
    }
    uint32_t mapId = parse_u32(mapIdStr, 0);
    if (mapId == 0) {
      write_json(res, R"({"status":"error","message":"invalid map_id"})");
      return;
    }
    write_json(res, handle_api_map_respawn(mapId));
  });

    // ── WebSocket：ゲームクライアントの直接接続（join チケット／passToken の検証）──
  auto doUpgrade = [&](auto *res, auto *req, auto *context) {
    std::string methodId = extractQueryParam(req->getQuery(), "methodId");
    std::string joinTicket = extractQueryParam(req->getQuery(), "ticket");
    std::string reconnectPassToken = extractQueryParam(req->getQuery(), "passToken");
    SessionData session{};

    const bool hasJoinTicket = !methodId.empty() || !joinTicket.empty();
    const bool hasPassToken = !reconnectPassToken.empty();
    if (hasJoinTicket && hasPassToken) {
      res->writeStatus("400 Bad Request")->end("ticket and passToken are mutually exclusive");
      return;
    }

    if (hasJoinTicket) {
      if (!validateJoinTicket(methodId, joinTicket, cfg, session.uid)) {
        res->writeStatus("401 Unauthorized")->end("invalid or expired join ticket");
        return;
      }
      session.zoneId = cfg.zoneId;
      session.countedOnline = true;
    } else if (hasPassToken) {
      zone_auth::PassTokenRecord rec;
      if (!readPassTokenRecord(reconnectPassToken, cfg.zoneId, rec)) {
        res->writeStatus("401 Unauthorized")->end("invalid or expired passToken");
        return;
      }
      session.uid = rec.uid;
      session.roleId = rec.roleId;
      session.passToken = reconnectPassToken;
      session.currentMapId = rec.mapId;
      session.zoneId = cfg.zoneId;
      session.isReconnect = true;
    } else {
      res->writeStatus("401 Unauthorized")->end("ticket or passToken required");
      return;
    }

    res->template upgrade<SessionData>(
        std::move(session),
        req->getHeader("sec-websocket-key"),
        req->getHeader("sec-websocket-protocol"),
        req->getHeader("sec-websocket-extensions"), context);
  };

  auto on_open = [&](WS *ws) {
    auto seq = gZoneWsSeq.fetch_add(1, std::memory_order_relaxed);
    uint64_t connId = makeConnId(workerId, seq);
    ws->getUserData()->connId = connId;
    auto *state = zoneNetWorkerState(workerId);
    if (state) {
      std::unique_lock lk(state->connMtx);
      state->connMap[connId] = ws;
    }
    zone_log::debug("zone_http ws connected conn_id=%llu",
                    (unsigned long long)connId);
  };

  auto on_message = [&ptokArr, zoneId = cfg.zoneId](WS *ws, std::string_view msg, uWS::OpCode) {
    auto *ud = ws->getUserData();
    std::string payload(msg);

        // Envelope をデコードする
    const Game::Envelope *env = codec::decode(payload);
    if (!env) return;

    switch (env->msg_type()) {

    case Game::MsgType_C2S_RequestEnterZone: {
      auto *m = env->body_as_C2S_RequestEnterZone();
      if (!m) return;

            // uid は upgrade 時にハンドシェイクのチケットから取り出し、SessionData に格納済み。
            // RequestEnterZone はゾーンに入るという業務上の意味だけを持ち、プロトコルのフィールドは client_version などの情報に使える。
      std::string uid = ud->uid;
      if (uid.empty()) {
        ws->send(codec::encodeErrorMessage(env->seq(),
            Game::ErrorType_Net_AuthFailed, "uid not found"),
            uWS::OpCode::BINARY);
        return;
      }

            // キャラクターを検索／作成する（同時にバッグも読み込み、SessionRegister と一緒にゲームワーカーへ引き渡す）
      codec::RoleInfoDto roleDto;
      inventory::RoleInventory roleInventory;
      equipment::RoleEquipment roleEquipment;
      buildings::RoleBuildings roleBuildings;
      std::vector<codec::TeamUnitInfoDto> ownedUnits;
      if (!findOrCreateRoleDto(uid, roleDto, roleInventory, roleEquipment,
                               roleBuildings, ownedUnits)) {
        ws->send(codec::encodeErrorMessage(env->seq(),
            Game::ErrorType_Net_LoginGameServerFailed, "role lookup failed"),
            uWS::OpCode::BINARY);
        return;
      }

      const bool isReconnect = ud->isReconnect;
      const std::string previousPassToken = ud->passToken;

            // 新しい pass_token を生成して Redis に書き込む
      std::string passToken;
      zone_auth::PassTokenRecord rec;
      rec.zoneId = zoneId;
      rec.uid = uid;
      rec.roleId = roleDto.roleId;
      rec.mapId = roleDto.currentMapId;
      rec.issuedAtMs = nowEpochMs();
      rec.expireAtMs =
          rec.issuedAtMs + static_cast<uint64_t>(runtime_config::PASS_TOKEN_SEC) * 1000;
      rec.version = 1;
      if (!issuePassToken(rec, passToken)) {
        ws->send(codec::encodeErrorMessage(env->seq(),
            Game::ErrorType_Net_LoginGameServerFailed, "session init failed"),
            uWS::OpCode::BINARY);
        return;
      }

      if (isReconnect) {
        zone_auth::PassTokenRecord consumed{};
        const RedisPassTokenConsumeResult consumeResult =
            consumePassTokenAndMarkOnline(previousPassToken, zoneId, consumed);
        if (consumeResult == RedisPassTokenConsumeResult::RedisError) {
          revokePassToken(zoneId, passToken);
          ws->send(codec::encodeErrorMessage(env->seq(),
              Game::ErrorType_Net_LoginGameServerFailed, "session restore failed"),
              uWS::OpCode::BINARY);
          return;
        }
        if (consumeResult != RedisPassTokenConsumeResult::Consumed ||
            consumed.uid != uid || consumed.roleId != roleDto.roleId) {
          if (consumeResult == RedisPassTokenConsumeResult::Consumed) {
            unmarkZoneOnline(zoneId);
          }
          revokePassToken(zoneId, passToken);
          ws->send(codec::encodeErrorMessage(env->seq(),
              Game::ErrorType_Net_AuthFailed, "passToken already used"),
              uWS::OpCode::BINARY);
          return;
        }
        ud->zoneId = zoneId;
        ud->countedOnline = true;
      }

      ud->passToken = passToken;
      ud->roleId = roleDto.roleId;
      ud->currentMapId = roleDto.currentMapId;
      ud->isReconnect = false;
      ud->enteredZone = true;

            // 重要な順序：必ず先にセッションを game_worker に登録してから、EnterZoneAccepted を返すこと。
            // 逆にすると、クライアントが Accepted を受け取ってすぐに送る後続のメッセージ（RequestEnterMap / EnterMapReady）が、
            // game_worker が SessionRegister を処理する前に gInbound に入ってしまうことがある。
            // その場合 game_loop の `sessions.find(connId)` がセッションを見つけられず、メッセージは黙って捨てられる。
      {
        uint8_t zone = zoneOfMap(ud->currentMapId);
        gSessionRegister[zone].enqueue(SessionRegister{
            ud->connId, roleDto.roleId, ud->passToken,
            roleDto.currentMapId, roleDto, isReconnect,
            std::move(roleInventory), std::move(roleEquipment),
            std::move(roleBuildings), std::move(ownedUnits)});
      }

            // S2C_EnterZoneAccepted を送る
      ws->send(codec::encodeEnterZoneAccepted(env->seq(), roleDto, ud->passToken.c_str(), true),
               uWS::OpCode::BINARY);

      zone_log::debug("zone_http EnterZoneAccepted uid=%s role_id=%s reconnect=%d",
                      uid.c_str(), roleDto.roleId.c_str(), isReconnect ? 1 : 0);
      break;
    }

    default:
      if (!ud->enteredZone || ud->roleId.empty() || ud->currentMapId == 0) return;
      uint8_t zone = zoneOfMap(ud->currentMapId);
      gInbound[zone].enqueue(ptokArr[zone],
          InboundMsg{ud->connId, env->seq(), MsgType::Data, std::move(payload)});
      break;
    }
  };

  auto on_close = [&](WS *ws, int code, std::string_view) {
    auto *ud = ws->getUserData();
    uint64_t connId = ud->connId;
    auto *state = zoneNetWorkerState(workerId);
    if (state) {
      std::unique_lock lk(state->connMtx);
      state->connMap.erase(connId);
    }

    const bool wasCountedOnline = ud->countedOnline;
    const uint32_t countedZoneId = ud->zoneId;

        // WebSocket の close コード（RFC 6455）：
        // code 1000 = 正常な切断（サーバーによるキック、明示的なログアウト）
        // code 1001 = 端点の離脱（ブラウザのタブを閉じた／ページを移動・再読み込みした）
        // code 1006 = 異常な切断（ネットワーク断、クラッシュ）
        // code 1012 = サーバーの再起動／停止。クライアントは復旧後に passToken で再接続を試みてよい
        //
        // passToken の扱い：
        // - EnterZone 済みで code 1000/1001：現在のセッションの新しい passToken を無効化する
        // - サーバーの shutdown による code 1012：passToken を残す。ローリング再起動で再接続用の資格情報を失わないため
        // - EnterZone 前の再接続の事前チェック中の接続：古い passToken を残す。upgrade 後に切断されてもチケットを失わないため
        // - code 1006／その他：passToken を残し、TTL で自然に期限切れになるのを待つ
    const bool gracefulDisconnect = (code == 1000 || code == 1001);
    if (gracefulDisconnect && wasCountedOnline && !ud->passToken.empty()) {
            // 正常な切断で無効化するのは、online に計上済みのセッションのトークンだけ。事前チェック段階では古いトークンを消してはならない。
      revokePassToken(countedZoneId, ud->passToken);
      zone_log::debug("zone_http session closed normally conn=%llu passToken revoked",
                      (unsigned long long)connId);
    } else {
            // 異常な切断、または再接続の事前チェックが未完了：passToken は Redis に残し、TTL（5 時間）で期限切れになるのを待つ。
      zone_log::debug("zone_http session closed abnormally code=%d conn=%llu passToken kept",
                      code, (unsigned long long)connId);
    }

    if (wasCountedOnline) {
      unmarkZoneOnline(countedZoneId);
      ud->countedOnline = false;
      ud->zoneId = 0;
    }

    if (ud->enteredZone && ud->currentMapId > 0) {
      uint8_t zone = zoneOfMap(ud->currentMapId);
      gInbound[zone].enqueue(
        InboundMsg{connId, 0, MsgType::Disconnect, "", gracefulDisconnect});
    }
  };

  HttpShutdownRuntime shutdownRuntime{workerId, zoneNetWorkerState(workerId)};

  app.ws<SessionData>("/game/ws",
                      {.upgrade = [&](auto *res, auto *req, auto *context) {
                         doUpgrade(res, req, context);
                       },
                       .open = on_open,
                       .message = on_message,
                       .close = on_close})
     .get("/*", [](auto *res, auto *) { res->end("Zone Server"); })
     .listen(host, port, [workerId, port, host, &shutdownRuntime](auto *token) {
       if (token) {
         shutdownRuntime.listenSocket = token;
         zone_log::info("zone_http:%u listening on %s:%d", workerId, host, port);
       } else {
         zone_log::error("zone_http:%u FAILED to listen on %s:%d", workerId, host, port);
       }
     });

  if (!shutdownRuntime.listenSocket) {
    runtime_shutdown::requestShutdown();
    zone_log::info("zone_http:%u stopped before listen", workerId);
    return;
  }

    // postHandler：ループが動いている間もキューを処理させる。スレッドをまたぐ send は defer で能動的にループを起こす。
  auto *loop = uWS::Loop::get();
  if (auto *state = zoneNetWorkerState(workerId)) {
    state->loop.store(loop, std::memory_order_release);
  }
  loop->addPostHandler(nullptr, [workerId](uWS::Loop *) {
    drainWorkerOutbound(workerId);
  });

  shutdownRuntime.timer =
      us_create_timer(reinterpret_cast<struct us_loop_t *>(loop), 0,
                      sizeof(HttpShutdownRuntime *));
  if (shutdownRuntime.timer) {
    *reinterpret_cast<HttpShutdownRuntime **>(
        us_timer_ext(shutdownRuntime.timer)) = &shutdownRuntime;
        // fallthrough しない：shutdown 後もループを起こし続ける必要がある。listen ソケットの
        // 遅延 close-list が解放されたら、コールバック内で自身を閉じて app.run() から戻らせる。
    us_timer_set(shutdownRuntime.timer, pollHttpShutdown, kHttpShutdownPollMs,
                 kHttpShutdownPollMs);
  } else {
    zone_log::error("zone_http:%u failed to create shutdown timer", workerId);
        // uWS のループが正常終了の要求を受け取る入口はこのタイマーだけ。タイマーなしで動かし続けると、q を入力した後に止まらなくなる。
    runtime_shutdown::requestShutdown();
    app.close();
    if (auto *state = zoneNetWorkerState(workerId)) {
      state->loop.store(nullptr, std::memory_order_release);
    }
    return;
  }

  scheduleWorkerOutboundDrain(workerId);

  app.run();
  if (shutdownRuntime.timer) {
    us_timer_close(shutdownRuntime.timer);
    shutdownRuntime.timer = nullptr;
  }
  if (auto *state = zoneNetWorkerState(workerId)) {
    state->loop.store(nullptr, std::memory_order_release);
  }
  zone_log::info("zone_http:%u stopped", workerId);
}
