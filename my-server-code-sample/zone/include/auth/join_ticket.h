#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace zone_auth {

/**
 * @brief AES-256-GCM で暗号化した join チケットの固定的なプロトコル上の制約
 *
 * @note
 * - これらの定数は gateway/src/ticket_crypto.zig と一致させること。
 * - 暗号化チケットの通信上のレイアウトは nonce || ciphertext || tag。
 */
inline constexpr size_t kTicketCryptoKeyBytes = 32;
inline constexpr size_t kTicketCryptoNonceBytes = 12;
inline constexpr size_t kTicketCryptoTagBytes = 16;
inline constexpr size_t kTicketKeyMaxBytes = 64;
inline constexpr size_t kUidMaxBytes = 32;
inline constexpr size_t kTicketPlaintextMaxBytes =
    1 + kTicketKeyMaxBytes + 1 + kUidMaxBytes + 1 + 8;
inline constexpr size_t kEncryptedTicketMaxBytes =
    kTicketCryptoNonceBytes + kTicketPlaintextMaxBytes + kTicketCryptoTagBytes;

using TicketCryptoKey = std::array<uint8_t, kTicketCryptoKeyBytes>;

/**
 * @brief チケット暗号化の鍵セット
 *
 * @note
 * - スロットの添字 0..3 は、通信上の methodId 1..4 に対応する。
 * - gateway の暗号化とゾーンサーバーの復号は同じ鍵セットを使うこと。
 */
struct TicketCryptoKeySet {
  std::array<TicketCryptoKey, 4> slots{};
};

/**
 * @brief 復号した join チケットのペイロード
 *
 * @note
 * - ticketKey[0..ticketKeyLen] は Redis のチケットキーの平文。
 * - uid[0..uidLen] は users.uid の固定長配列へのコピー。
 * - zoneId は、このゾーンサーバーの env.zone に設定された ZONE_ID と一致している必要がある。
 */
struct DecryptedJoinTicket {
  std::array<uint8_t, kTicketKeyMaxBytes> ticketKey{};
  uint8_t ticketKeyLen = 0;
  std::array<uint8_t, kUidMaxBytes> uid{};
  uint8_t uidLen = 0;
  uint8_t zoneId = 0;
  int64_t expiresAtMs = 0;
};

/**
 * @brief Redis に保存された join チケットの値
 *
 * @note
 * - 値のレイアウトは uid_len:u8 + uid のバイト列 + zone_id:u8。
 * - このレイアウトは gateway/src/redis.zig の encodeJoinTicketValue と一致させること。
 */
struct RedisJoinTicketValue {
  std::array<uint8_t, kUidMaxBytes> uid{};
  uint8_t uidLen = 0;
  uint8_t zoneId = 0;
};

/**
 * @brief join チケットの復号・検証エラー
 *
 * @note
 * - 成功を表すのは Ok だけ。それ以外はすべて、クライアントが送ったチケットではセッションを確立できないことを表す。
 * - AuthenticationFailed は暗号文の改ざん・鍵の誤り・タグの誤りを区別しない。認証の詳細を漏らさないため。
 */
enum class JoinTicketError : uint8_t {
  Ok = 0,
  BadMethodId,
  BadHex,
  TokenTooShort,
  TokenTooLong,
  AuthenticationFailed,
  BadTokenPayload,
  TicketExpired,
  ZoneMismatch,
  TicketNotFound,
  BadRedisValue,
  UidMismatch,
};

/**
 * @brief 固定長の 16 進数 AES 鍵を解析する
 *
 * @param hex 64 文字の 16 進数テキスト。呼び出し中だけ借用する。
 * @param out 呼び出し側が所有する鍵の出力先。
 *
 * @retval true out に 32 バイトの鍵を書き込んだ。
 * @retval false 長さまたは文字が 16 進数の鍵として不正。
 */
bool parseTicketCryptoKeyHex(std::string_view hex, TicketCryptoKey &out);

/**
 * @brief 16 進数テキストを呼び出し側のバッファにデコードする
 *
 * @param hex 偶数文字の 16 進数テキスト。呼び出し中だけ借用する。
 * @param out 呼び出し側が所有する出力バッファ。
 * @param outLen 成功時に有効なバイト数を書き込む。
 *
 * @retval true out[0..outLen] が有効。
 * @retval false 16 進数の長さ・文字、または出力容量が条件を満たさない。
 */
bool decodeHexBytes(std::string_view hex, std::span<uint8_t> out, size_t &outLen);

/**
 * @brief gateway が発行した join チケットを復号する
 *
 * @param methodId gateway の /join が返した methodId。通信上の値は 1..4。
 * @param keySet このゾーンサーバーが持つ AES 鍵スロット。
 * @param encryptedToken nonce || ciphertext || tag。呼び出し中だけ借用する。
 * @param out 呼び出し側が所有するペイロードの出力先。
 *
 * @retval Ok 認証を通過したペイロードを out に書き込んだ。
 * @return それ以外の値はトークンを信頼できないことを表す。呼び出し側はハンドシェイクを拒否すること。
 *
 * @note
 * - 本関数が行うのは AES-GCM の認証と平文レイアウトの検証だけ。
 * - 呼び出し側は、これに加えて expiresAtMs と zoneId を確認し、Redis のチケットを消費する必要がある。
 */
JoinTicketError decryptEncryptedJoinTicket(
    uint8_t methodId,
    const TicketCryptoKeySet &keySet,
    std::span<const uint8_t> encryptedToken,
    DecryptedJoinTicket &out);

/**
 * @brief Redis の join チケットの値を解析する
 *
 * @param value Redis の GETDEL の戻り値。呼び出し中だけ借用する。
 * @param out 呼び出し側が所有する値の出力先。
 *
 * @retval Ok out に uid/zoneId を書き込んだ。
 * @retval BadRedisValue Redis の値が gateway のプロトコルと一致しない。
 */
JoinTicketError decodeRedisJoinTicketValue(
    std::span<const uint8_t> value,
    RedisJoinTicketValue &out);

/**
 * @brief join チケットのペイロードと Redis の値に含まれる uid を比較する
 *
 * @param ticket AES-GCM の認証を通過したチケットのペイロード。
 * @param redisValue Redis のチケットの値。
 *
 * @retval true uid のバイト列が完全に一致する。
 */
bool uidEquals(const DecryptedJoinTicket &ticket, const RedisJoinTicketValue &redisValue);

/**
 * @brief join チケットのエラー名を返す
 *
 * @param err 検証エラーの列挙値。
 *
 * @return 静的な文字列。プロセスが終了するまで有効。
 */
const char *joinTicketErrorName(JoinTicketError err);

} // namespace zone_auth
