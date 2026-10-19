#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace zone_redis {

/**
 * @brief gateway の join チケットをアトミックに消費し、収容数の状態を更新する
 *
 * @note
 * - KEYS[1] は Redis のチケットキー。
 * - KEYS[2] は zone:<id>:reservations。
 * - KEYS[3] は zone:<id>:online。
 * - ARGV[1] は uid_len + uid + zone_id 形式の Redis 値。
 *
 * @retval {0,''} チケットが存在しない、または期限切れ。online は変化しない。
 * @retval {1,value} チケットは GETDEL、予約は ZREM 済みだが値が一致しない。online は変化しない。
 * @retval {2,value} チケットを消費して予約を削除し、online をインクリメントした。
 */
inline constexpr const char *kConsumeJoinTicketScript =
    "local value=redis.call('GETDEL',KEYS[1]);"
    "if not value then return {0,''}; end;"
    "redis.call('ZREM',KEYS[2],KEYS[1]);"
    "if value~=ARGV[1] then return {1,value}; end;"
    "redis.call('INCR',KEYS[3]);"
    "return {2,value};";

/**
 * @brief passToken をアトミックに消費し、online に加算する
 *
 * @note
 * - KEYS[1] は zone:<id>:passtoken:<token>。
 * - KEYS[2] は zone:<id>:online。
 *
 * @retval {0,''} トークンが存在しない・期限切れ・消費済み。online は変化しない。
 * @retval {1,value} トークンを GETDEL して online をインクリメントした。value は passToken のレコード。
 */
inline constexpr const char *kConsumePassTokenScript =
    "local value=redis.call('GETDEL',KEYS[1]);"
    "if not value then return {0,''}; end;"
    "redis.call('INCR',KEYS[2]);"
    "return {1,value};";

/**
 * @brief 切断時に online をデクリメントし、異常な負数は 0 に戻す
 *
 * @note
 * - KEYS[1] は zone:<id>:online。
 * - online に計上済みの WebSocket セッションに対してのみ呼ぶこと。
 */
inline constexpr const char *kDecrementZoneOnlineScript =
    "local n=redis.call('DECR',KEYS[1]);"
    "if n<0 then redis.call('SET',KEYS[1],0); return 0; end;"
    "return n;";

inline std::string makeZoneReservationsKey(uint32_t zoneId) {
  return "zone:" + std::to_string(zoneId) + ":reservations";
}

inline std::string makeZoneOnlineKey(uint32_t zoneId) {
  return "zone:" + std::to_string(zoneId) + ":online";
}

inline std::string makePassTokenKey(uint32_t zoneId, std::string_view token) {
  std::string key("zone:");
  key += std::to_string(zoneId);
  key += ":passtoken:";
  key.append(token.data(), token.size());
  return key;
}

} // namespace zone_redis
