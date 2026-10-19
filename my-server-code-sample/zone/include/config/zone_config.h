#pragma once
#include "auth/join_ticket.h"
#include "env_loader.h"
#include "utils/log.h"
#include "utils/str_utils.h"
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace zone {
    /**
     * @brief zone プロセスの実行時設定
     *
     * @note
     * - loadFromEnvFile が env.zone から値を設定する。各フィールドは固定長の char バッファで、上限を明確にしている。
     * - zoneId は gateway のデータベースの zones.id に対応する。0 は無効な設定で、起動は許可しない。
     * - ticketCryptoKeys は、gateway の /join が発行する暗号化チケットを復号するための鍵の所有者。
     */
  struct ZoneConfig {
    char envFile[12] = "env.zone";
    char appEnv[12] = "dev";

    // MySQL X Protocol
    char mysqlHost[32] = {};
    int  mysqlPort = 3306;
    char mysqlUser[32] = {};
    char mysqlPasswd[64] = {};
    char mysqlDb[32] = {};

    // Redis
    char redisHost[32] = {};
    int  redisPort = 6379;
    int  redisDb = 0;
    char redisUser[64] = {};
    char redisPassword[128] = {};

    // Zone Server
    char listenHost[32] = "0.0.0.0";
    int  listenPort = 9002;
    int  tickRate = 30;
        /**
         * 精霊のレベル上限。
         *
         * 企画がいつでも調整できる。env.zone を変更して POST /api/reload-config を呼べば反映され、
         * 再起動は不要（tick の境界でまとめて差し替える）。
         * 上限を下げても、既に上限を超えている保存済みのレベルは戻さない。マップ入場時に上限へ丸めるだけ。
         */
    int  maxUnitLevel = 60;
    uint32_t zoneId = 0;
    char internalApiToken[128] = {};

    // Gateway join ticket crypto
    zone_auth::TicketCryptoKeySet ticketCryptoKeys{};
    bool ticketCryptoUsesDevDefaults = false;
  };

/**
 * @brief env.zone の ZONE_ID を解析する
 *
 * @param value env のテキスト値。呼び出し中だけ借用する。
 * @param out 成功時に zones.id を書き込む。現在の gateway の通信上の範囲は 1..255。
 *
 * @retval true out が有効。
 * @retval false 未設定・0・数字以外・現在の gateway の通信上の上限超えのいずれか。
 *
 * @note
 * - ZONE_ID はデータベースの `zones.id` であり、ゾーン一覧のインデックスではない。
 */
inline bool parseZoneIdEnvValue(std::string_view value, uint32_t &out) {
  out = 0;
  if (value.empty()) return false;

  uint32_t parsed = 0;
  for (char ch : value) {
    const auto uch = static_cast<unsigned char>(ch);
    if (!std::isdigit(uch)) return false;
    const uint32_t digit =
        static_cast<uint32_t>(uch - static_cast<unsigned char>('0'));
    if (parsed > (255u - digit) / 10u) return false;
    parsed = parsed * 10u + digit;
    if (parsed > 255u) return false;
  }

  if (parsed == 0) return false;
  out = parsed;
  return true;
}

/**
 * @brief 0 以上の 10 進整数の設定値を解析する
 *
 * @param value env のテキスト値。呼び出し中だけ借用する。すべて数字で構成されていること。
 * @param out 成功時に解析した int を書き込む。
 *
 * @retval true value が 10 進数として最後まで解析でき、int の上限を超えない。
 * @retval false 空、数字以外を含む、またはオーバーフロー。
 */
inline bool parseNonNegativeIntEnvValue(std::string_view value, int &out) {
  out = 0;
  if (value.empty()) return false;

  int parsed = 0;
  for (char ch : value) {
    const auto uch = static_cast<unsigned char>(ch);
    if (!std::isdigit(uch)) return false;
    const int digit = static_cast<int>(uch - static_cast<unsigned char>('0'));
    if (parsed > (std::numeric_limits<int>::max() - digit) / 10) return false;
    parsed = parsed * 10 + digit;
  }

  out = parsed;
  return true;
}

/**
 * @brief int の env 設定値を閉区間で解析する
 *
 * @param value env のテキスト値。呼び出し中だけ借用する。10 進数として最後まで解析できること。
 * @param minValue 許可する最小値。
 * @param maxValue 許可する最大値。
 * @param out 成功時に解析した int を書き込む。
 *
 * @retval true value を最後まで解析でき、[minValue, maxValue] に収まっている。
 * @retval false 解析失敗または範囲外。
 */
inline bool parseIntRangeEnvValue(std::string_view value, int minValue,
                                  int maxValue, int &out) {
  int parsed = 0;
  if (!parseNonNegativeIntEnvValue(value, parsed)) return false;
  if (parsed < minValue || parsed > maxValue) return false;
  out = parsed;
  return true;
}

/**
 * @brief 省略可能な int の env 設定を読み込み、範囲を確認する
 *
 * @param loader env.zone を読み込み済みの設定リーダー。呼び出し中だけ借用する。
 * @param key env のキー名。呼び出し中は有効な文字列であること。
 * @param minValue 許可する最小値。
 * @param maxValue 許可する最大値。
 * @param out key が存在して値が正しければ書き込む。key がなければ元の値のまま。
 *
 * @retval true key が存在しない、または値が正しい。
 * @retval false key は存在するが、数字として最後まで解析できない、または範囲外。
 */
inline bool loadOptionalIntRange(config::EnvLoader &loader, const char *key,
                                 int minValue, int maxValue, int &out) {
  auto v = loader.GetString(key);
  if (!v.has_value()) return true;

  int parsed = 0;
  if (!parseIntRangeEnvValue(*v, minValue, maxValue, parsed)) {
    zone_log::error("invalid %s: expected full decimal integer in range %d..%d",
                    key, minValue, maxValue);
    return false;
  }
  out = parsed;
  return true;
}

/**
 * @brief 空でもよい文字列設定をコピーする
 *
 * @param dst cfg 内の固定長文字配列。呼び出し側が所有する。
 * @param src env のテキスト値。呼び出し中だけ借用する。空文字列でもよい。
 * @param key env のキー名。呼び出し中だけ借用し、長さエラーのログに使う。
 *
 * @retval true src を終端の 0 まで含めてコピーした。
 * @retval false src が dst に保存できる最大長を超えている。
 */
template <size_t N>
inline bool copyOptionalConfigString(char (&dst)[N], const char *src, const char *key) {
  const size_t len = std::strlen(src);
  if (len >= N) {
    zone_log::error("config value too long: %s (len=%zu, max=%zu)",
                    key, len, N - 1);
    return false;
  }
  std::memcpy(dst, src, len + 1);
  return true;
}

/**
 * @brief 必須の文字列設定を読み込む
 *
 * @param loader env.zone を読み込み済みの設定リーダー。呼び出し中だけ借用する。
 * @param key env のキー名。呼び出し中だけ借用する。
 * @param out cfg 内の固定長文字配列。呼び出し側が所有する。
 *
 * @retval true key が存在して空でなく、out にコピーした。
 * @retval false key がない、空、または out の容量を超える。
 */
template <size_t N>
inline bool loadRequiredConfigString(config::EnvLoader &loader, const char *key,
                                     char (&out)[N]) {
  auto v = loader.GetString(key);
  if (!v.has_value()) {
    zone_log::error("missing required config: %s", key);
    return false;
  }
  return utils::copy_checked(out, v->c_str(), key);
}

/**
 * @brief gateway の開発環境用に組み込まれたチケット鍵を読み込む
 *
 * @param cfg 呼び出し側が所有する実行時設定。成功すると ticketCryptoKeys に書き込む。
 *
 * @retval true 開発用の鍵を書き込んだ。
 *
 * @note
 * - APP_ENV=dev で、かつ鍵が明示的に設定されていない場合にだけ呼ぶこと。
 */
inline bool loadDevTicketCryptoKeys(ZoneConfig &cfg) {
  static constexpr std::array<std::string_view, 4> kDevKeys = {
      "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff",
      "102132435465768798a9babbdcddfeff102132435465768798a9babbdcddfeff",
      "2031425364758697a8b9cacbdcedfe0f2031425364758697a8b9cacbdcedfe0f",
      "30415263748596a7b8c9dadbecfd0e1f30415263748596a7b8c9dadbecfd0e1f",
  };

  for (size_t i = 0; i < kDevKeys.size(); ++i) {
    if (!zone_auth::parseTicketCryptoKeyHex(kDevKeys[i], cfg.ticketCryptoKeys.slots[i])) {
      return false;
    }
  }
  cfg.ticketCryptoUsesDevDefaults = true;
  return true;
}

/**
 * @brief env からチケット暗号化の鍵セットを読み込む
 *
 * @param loader env.zone を読み込み済みの設定リーダー。呼び出し中だけ借用する。
 * @param cfg 呼び出し側が所有する実行時設定。成功すると ticketCryptoKeys に書き込む。
 *
 * @retval true 鍵セットが揃っていて使用できる。
 * @retval false dev 以外で鍵がない、一部の鍵しか設定されていない、または鍵のテキストが 32 バイトの 16 進数でない。
 *
 * @note
 * - dev 以外の環境では、4 つの TICKET_AES256_GCM_V1_SLOT*_KEY_HEX をすべて明示的に設定すること。
 */
inline bool loadTicketCryptoKeys(config::EnvLoader &loader, ZoneConfig &cfg) {
  static constexpr std::array<const char *, 4> kKeyNames = {
      "TICKET_AES256_GCM_V1_SLOT1_KEY_HEX",
      "TICKET_AES256_GCM_V1_SLOT2_KEY_HEX",
      "TICKET_AES256_GCM_V1_SLOT3_KEY_HEX",
      "TICKET_AES256_GCM_V1_SLOT4_KEY_HEX",
  };

  std::array<std::optional<std::string>, 4> values{};
  bool anyKeyConfigured = false;
  bool allKeysConfigured = true;
  for (size_t i = 0; i < kKeyNames.size(); ++i) {
    values[i] = loader.GetString(kKeyNames[i]);
    anyKeyConfigured = anyKeyConfigured || values[i].has_value();
    allKeysConfigured = allKeysConfigured && values[i].has_value();
  }

  if (!anyKeyConfigured) {
    if (std::string_view(cfg.appEnv) != "dev") {
      zone_log::error(
          "missing required ticket crypto keys; only APP_ENV=dev may use built-in dev keys");
      return false;
    }
    zone_log::warning("using built-in dev ticket crypto keys; configure explicit keys outside local development");
    return loadDevTicketCryptoKeys(cfg);
  }

  if (!allKeysConfigured) {
    zone_log::error("partial ticket crypto key config is not allowed");
    return false;
  }

  for (size_t i = 0; i < kKeyNames.size(); ++i) {
    if (!zone_auth::parseTicketCryptoKeyHex(*values[i], cfg.ticketCryptoKeys.slots[i])) {
      zone_log::error("invalid ticket crypto key config: %s", kKeyNames[i]);
      return false;
    }
  }
  cfg.ticketCryptoUsesDevDefaults = false;
  return true;
}

/**
 * @brief zone の実行時設定を読み込む
 *
 * @param envFile env ファイルのパス。呼び出し中だけ借用する。
 * @param cfg 呼び出し側が所有する出力先の設定。
 *
 * @retval true env とチケット暗号化の設定をどちらも読み込んだ。
 * @retval false ファイル、フィールドの長さ、または必須設定が条件を満たさない。
 *
 * @note
 * - ZONE_ID は env.zone に必ず設定すること。0 は無効または未設定を表す。
 * - cfg.zoneId は gateway のデータベースの `zones.id` に対応する。現在の通信上の範囲は 1..255。
 */
inline bool loadFromEnvFile(const char* envFile, ZoneConfig &cfg) {
  const bool isInValidFile = (!envFile || envFile[0] == '\0');
  if (isInValidFile) return false;

  config::EnvLoader loader;
  std::string err;
  if (!loader.LoadFile(envFile, err)) {
    zone_log::error("%s", err.c_str());
    return false;
  }

  if (!utils::copy_checked(cfg.envFile, envFile)) return false;

  if (auto v = loader.GetString("APP_ENV"))
    if (!utils::copy_checked(cfg.appEnv, v->c_str(), "APP_ENV")) return false;

  if (auto v = loader.GetString("ZONE_ID")) {
    if (!parseZoneIdEnvValue(*v, cfg.zoneId)) {
      zone_log::error(
          "invalid or missing ZONE_ID; zoneId is an unsigned zones.id value starting from 1, current gateway wire range is 1..255.");
      return false;
    }
  } else {
    zone_log::error(
        "invalid or missing ZONE_ID; zoneId is an unsigned zones.id value starting from 1, current gateway wire range is 1..255.");
    return false;
  }

  if (!loadRequiredConfigString(loader, "MYSQL_HOST", cfg.mysqlHost)) return false;
  if (!loadOptionalIntRange(loader, "MYSQL_PORT", 1, 65535, cfg.mysqlPort)) return false;
  if (!loadRequiredConfigString(loader, "MYSQL_USER", cfg.mysqlUser)) return false;
  if (auto v = loader.GetString("MYSQL_PASSWORD"))
    if (!utils::copy_checked(cfg.mysqlPasswd, v->c_str(), "MYSQL_PASSWORD")) return false;
  if (!loadRequiredConfigString(loader, "MYSQL_DB", cfg.mysqlDb)) return false;

  if (!loadRequiredConfigString(loader, "REDIS_HOST", cfg.redisHost)) return false;
  if (!loadOptionalIntRange(loader, "REDIS_PORT", 1, 65535, cfg.redisPort)) return false;
  if (!loadOptionalIntRange(loader, "REDIS_DB", 0, std::numeric_limits<int>::max(), cfg.redisDb)) return false;
  if (auto v = loader.GetString("REDIS_USER"))
    if (!copyOptionalConfigString(cfg.redisUser, v->c_str(), "REDIS_USER")) return false;
  if (auto v = loader.GetString("REDIS_PASSWORD"))
    if (!copyOptionalConfigString(cfg.redisPassword, v->c_str(), "REDIS_PASSWORD")) return false;
  if (cfg.redisUser[0] != '\0' && cfg.redisPassword[0] == '\0') {
    zone_log::error("invalid REDIS_USER: REDIS_PASSWORD is required for ACL auth");
    return false;
  }

  if (auto v = loader.GetString("LISTEN_HOST"))
    if (!utils::copy_checked(cfg.listenHost, v->c_str(), "LISTEN_HOST")) return false;
  if (!loadOptionalIntRange(loader, "LISTEN_PORT", 1, 65535, cfg.listenPort)) return false;
  if (!loadOptionalIntRange(loader, "TICK_RATE", 1, 240, cfg.tickRate)) return false;
  if (!loadOptionalIntRange(loader, "MAX_UNIT_LEVEL", 1, 1000, cfg.maxUnitLevel)) return false;
  if (auto v = loader.GetString("ZONE_INTERNAL_API_TOKEN"))
    if (!utils::copy_checked(cfg.internalApiToken, v->c_str(), "ZONE_INTERNAL_API_TOKEN")) return false;

  if (!loadTicketCryptoKeys(loader, cfg)) return false;

  return true;
}
}
