#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace zone_auth {

/**
 * @brief passToken の乱数部分のバイト数
 *
 * @note
 * - この値を変えると Redis のキーとクライアントに渡すトークン文字列の長さが変わる。
 *   プロトコルのテストもあわせて更新すること。
 */
inline constexpr size_t kPassTokenRandomBytes = 32;

/**
 * @brief 通信上の passToken 文字列の長さ
 *
 * @note
 * - トークンは小文字の 16 進数で表す。乱数 1 バイトにつき 2 文字。
 */
inline constexpr size_t kPassTokenHexBytes = kPassTokenRandomBytes * 2;

/**
 * @brief Redis に保存する再接続用の認証情報レコード
 *
 * @note
 * - zoneId はこのトークンを発行した zones.id。消費する側の ZONE_ID と一致していなければならない。
 * - uid / roleId は、join チケットまたは以前の passToken で認証済みの ID 情報。
 * - mapId は現在のプロトコルで表現できるマップ ID。0 は無効。
 */
struct PassTokenRecord {
  uint32_t zoneId = 0;
  std::string uid;
  std::string roleId;
  uint32_t mapId = 0;
  uint64_t issuedAtMs = 0;
  uint64_t expireAtMs = 0;
  uint32_t version = 1;
};

/**
 * @brief 固定長の再接続トークンを生成する
 *
 * @param out 呼び出し側が所有する出力先。成功時は 64 文字の小文字 16 進数を書き込む。
 *
 * @retval true トークンを書き込んだ。
 * @retval false OS の安全な乱数源が使えない。呼び出し側はトークンの発行を中止すること。
 */
bool generatePassToken(std::string &out);

/**
 * @brief passToken レコードを Redis の値にエンコードする
 *
 * @param rec 呼び出し側が持つレコード。呼び出し中だけ借用する。
 *
 * @return 固定のフィールド順のテキスト：zone|uid|role|map|issued|expire|version。
 *
 * @note
 * - uid / roleId に `|` を含めてはならない。Redis へ書き込む前に、値の出所が信頼できることを
 *   呼び出し側で保証すること。
 */
std::string serializePassTokenRecord(const PassTokenRecord &rec);

/**
 * @brief Redis の passToken レコードを解析・検証する
 *
 * @param data Redis の値。呼び出し中だけ借用する。
 * @param expectedZoneId このゾーンサーバーの env.zone に設定された ZONE_ID。
 * @param nowMs 現在の WallClock（ミリ秒）。
 * @param out 成功時に解析したレコードを書き込む。
 *
 * @retval true レコードがこのゾーンのもので、期限内で、各フィールドが妥当。
 * @retval false トークンを信頼できない。呼び出し側は再接続を拒否すること。
 */
bool parsePassTokenRecord(std::string_view data,
                          uint32_t expectedZoneId,
                          uint64_t nowMs,
                          PassTokenRecord &out);

/**
 * @brief クライアントから受け取ったトークン文字列の形式を検証する
 *
 * @param token クライアントが送ったトークン。呼び出し中だけ借用する。
 *
 * @retval true トークンが 64 文字の小文字 16 進数である。
 */
bool isValidPassTokenText(std::string_view token);

} // namespace zone_auth
