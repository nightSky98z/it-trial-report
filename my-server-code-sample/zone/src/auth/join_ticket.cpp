#include "auth/join_ticket.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <openssl/evp.h>

namespace zone_auth {
namespace {

struct EvpCipherCtxDeleter {
  void operator()(EVP_CIPHER_CTX *ctx) const {
    EVP_CIPHER_CTX_free(ctx);
  }
};

/**
 * @brief 16 進数 1 文字を 4 ビット値（nibble）に変換する
 *
 * @retval 0..15 有効な 16 進数文字。
 * @retval -1 16 進数ではない文字。
 */
int hexNibble(unsigned char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}

/**
 * @brief methodId に応じて復号に使う鍵スロットを選ぶ
 *
 * @retval 非nullptr methodId に対応する鍵。
 * @retval nullptr methodId が 1..slot_count の範囲外。
 */
const TicketCryptoKey *keyForMethodId(const TicketCryptoKeySet &keySet,
                                      uint8_t methodId) {
  if (methodId < 1 || methodId > keySet.slots.size()) {
    return nullptr;
  }
  return &keySet.slots[methodId - 1];
}

/**
 * @brief リトルエンディアンの i64 を読み取る
 *
 * @note
 * - bytes は少なくとも 8 バイトの有効なメモリを指していること。
 */
int64_t readI64Le(const uint8_t *bytes) {
  uint64_t value = 0;
  for (size_t i = 0; i < 8; ++i) {
    value |= static_cast<uint64_t>(bytes[i]) << (i * 8);
  }
  return static_cast<int64_t>(value);
}

/**
 * @brief 復号した join チケットの平文を解析する
 *
 * @retval Ok out にチケット全体を書き込んだ。
 * @retval BadTokenPayload 平文の長さ、フィールド長、または zoneId がプロトコルの制約を満たさない。
 */
JoinTicketError decodePlaintext(std::span<const uint8_t> plaintext,
                                DecryptedJoinTicket &out) {
  size_t pos = 0;
  if (plaintext.size() < 1 + 1 + 1 + 8) {
    return JoinTicketError::BadTokenPayload;
  }

  const size_t ticketKeyLen = plaintext[pos++];
  if (ticketKeyLen == 0 || ticketKeyLen > kTicketKeyMaxBytes) {
    return JoinTicketError::BadTokenPayload;
  }
  if (pos + ticketKeyLen + 1 + 1 + 8 > plaintext.size()) {
    return JoinTicketError::BadTokenPayload;
  }

  DecryptedJoinTicket next{};
  next.ticketKeyLen = static_cast<uint8_t>(ticketKeyLen);
  std::copy_n(plaintext.data() + pos, ticketKeyLen, next.ticketKey.data());
  pos += ticketKeyLen;

  const size_t uidLen = plaintext[pos++];
  if (uidLen == 0 || uidLen > kUidMaxBytes) {
    return JoinTicketError::BadTokenPayload;
  }
  if (pos + uidLen + 1 + 8 != plaintext.size()) {
    return JoinTicketError::BadTokenPayload;
  }

  next.uidLen = static_cast<uint8_t>(uidLen);
  std::copy_n(plaintext.data() + pos, uidLen, next.uid.data());
  pos += uidLen;

  next.zoneId = plaintext[pos++];
  if (next.zoneId == 0) {
    return JoinTicketError::BadTokenPayload;
  }

  next.expiresAtMs = readI64Le(plaintext.data() + pos);
  out = next;
  return JoinTicketError::Ok;
}

} // namespace

/**
 * @brief AES-256-GCM 鍵の 16 進数テキストを解析する
 *
 * @retval true out に 32 バイトの鍵を書き込んだ。
 * @retval false 16 進数として不正、または長さが鍵長と一致しない。
 */
bool parseTicketCryptoKeyHex(std::string_view hex, TicketCryptoKey &out) {
  size_t outLen = 0;
  if (!decodeHexBytes(hex, std::span<uint8_t>(out.data(), out.size()), outLen)) {
    return false;
  }
  return outLen == out.size();
}

/**
 * @brief 16 進数のバイト列をデコードする
 *
 * @param out 呼び出し側が所有する出力バッファ。
 * @param outLen 成功時は実際のバイト数を書き込む。失敗時は 0 または途中までの長さ。
 *
 * @retval true 16 進数を最後までデコードでき、out の容量も超えなかった。
 * @retval false 長さが奇数、不正な文字がある、または out の容量が足りない。
 */
bool decodeHexBytes(std::string_view hex, std::span<uint8_t> out, size_t &outLen) {
  outLen = 0;
  if ((hex.size() % 2) != 0) {
    return false;
  }

  const size_t need = hex.size() / 2;
  if (need > out.size()) {
    return false;
  }

  for (size_t i = 0; i < need; ++i) {
    const int hi = hexNibble(static_cast<unsigned char>(hex[i * 2]));
    const int lo = hexNibble(static_cast<unsigned char>(hex[i * 2 + 1]));
    if (hi < 0 || lo < 0) {
      outLen = 0;
      return false;
    }
    out[i] = static_cast<uint8_t>((hi << 4) | lo);
  }

  outLen = need;
  return true;
}

JoinTicketError decryptEncryptedJoinTicket(
    uint8_t methodId,
    const TicketCryptoKeySet &keySet,
    std::span<const uint8_t> encryptedToken,
    DecryptedJoinTicket &out) {
  const TicketCryptoKey *key = keyForMethodId(keySet, methodId);
  if (!key) {
    return JoinTicketError::BadMethodId;
  }

  if (encryptedToken.size() <= kTicketCryptoNonceBytes + kTicketCryptoTagBytes) {
    return JoinTicketError::TokenTooShort;
  }
  if (encryptedToken.size() > kEncryptedTicketMaxBytes) {
    return JoinTicketError::TokenTooLong;
  }

  const uint8_t *nonce = encryptedToken.data();
  const uint8_t *ciphertext = encryptedToken.data() + kTicketCryptoNonceBytes;
  const size_t ciphertextLen =
      encryptedToken.size() - kTicketCryptoNonceBytes - kTicketCryptoTagBytes;
  const uint8_t *tag =
      encryptedToken.data() + encryptedToken.size() - kTicketCryptoTagBytes;
  if (ciphertextLen > kTicketPlaintextMaxBytes) {
    return JoinTicketError::TokenTooLong;
  }

  EVP_CIPHER_CTX *rawCtx = EVP_CIPHER_CTX_new();
  if (!rawCtx) {
    return JoinTicketError::AuthenticationFailed;
  }
  EvpCipherCtxDeleter ctxDeleter;
  std::unique_ptr<EVP_CIPHER_CTX, EvpCipherCtxDeleter> ctx(rawCtx, ctxDeleter);

  if (EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) {
    return JoinTicketError::AuthenticationFailed;
  }
  if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN,
                          static_cast<int>(kTicketCryptoNonceBytes), nullptr) != 1) {
    return JoinTicketError::AuthenticationFailed;
  }
  if (EVP_DecryptInit_ex(ctx.get(), nullptr, nullptr, key->data(), nonce) != 1) {
    return JoinTicketError::AuthenticationFailed;
  }

  const uint8_t aad[1] = {methodId};
  int written = 0;
  if (EVP_DecryptUpdate(ctx.get(), nullptr, &written, aad, sizeof(aad)) != 1) {
    return JoinTicketError::AuthenticationFailed;
  }

  std::array<uint8_t, kTicketPlaintextMaxBytes> plaintext{};
  if (EVP_DecryptUpdate(ctx.get(), plaintext.data(), &written, ciphertext,
                        static_cast<int>(ciphertextLen)) != 1) {
    return JoinTicketError::AuthenticationFailed;
  }
  const int plaintextLen = written;

  if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG,
                          static_cast<int>(kTicketCryptoTagBytes),
                          const_cast<uint8_t *>(tag)) != 1) {
    return JoinTicketError::AuthenticationFailed;
  }
  if (EVP_DecryptFinal_ex(ctx.get(), plaintext.data() + plaintextLen, &written) != 1) {
    return JoinTicketError::AuthenticationFailed;
  }

  return decodePlaintext(
      std::span<const uint8_t>(plaintext.data(), static_cast<size_t>(plaintextLen)),
      out);
}

JoinTicketError decodeRedisJoinTicketValue(
    std::span<const uint8_t> value,
    RedisJoinTicketValue &out) {
  if (value.size() < 1 + 1) {
    return JoinTicketError::BadRedisValue;
  }

  const size_t uidLen = value[0];
  if (uidLen == 0 || uidLen > kUidMaxBytes) {
    return JoinTicketError::BadRedisValue;
  }
  if (value.size() != 1 + uidLen + 1) {
    return JoinTicketError::BadRedisValue;
  }

  RedisJoinTicketValue next{};
  next.uidLen = static_cast<uint8_t>(uidLen);
  std::copy_n(value.data() + 1, uidLen, next.uid.data());
  next.zoneId = value[1 + uidLen];
  if (next.zoneId == 0) {
    return JoinTicketError::BadRedisValue;
  }

  out = next;
  return JoinTicketError::Ok;
}

/**
 * @brief 復号したチケットと Redis のチケット値の uid を比較する
 *
 * @retval true uid の長さとバイト列が完全に一致する。
 * @retval false uid が一致しない。
 */
bool uidEquals(const DecryptedJoinTicket &ticket,
               const RedisJoinTicketValue &redisValue) {
  if (ticket.uidLen != redisValue.uidLen) {
    return false;
  }
  return std::equal(ticket.uid.begin(), ticket.uid.begin() + ticket.uidLen,
                    redisValue.uid.begin());
}

/**
 * @brief JoinTicketError のログ用の固定名称を返す
 *
 * @return エラー列挙値に対応する静的文字列。
 */
const char *joinTicketErrorName(JoinTicketError err) {
  switch (err) {
  case JoinTicketError::Ok: return "Ok";
  case JoinTicketError::BadMethodId: return "BadMethodId";
  case JoinTicketError::BadHex: return "BadHex";
  case JoinTicketError::TokenTooShort: return "TokenTooShort";
  case JoinTicketError::TokenTooLong: return "TokenTooLong";
  case JoinTicketError::AuthenticationFailed: return "AuthenticationFailed";
  case JoinTicketError::BadTokenPayload: return "BadTokenPayload";
  case JoinTicketError::TicketExpired: return "TicketExpired";
  case JoinTicketError::ZoneMismatch: return "ZoneMismatch";
  case JoinTicketError::TicketNotFound: return "TicketNotFound";
  case JoinTicketError::BadRedisValue: return "BadRedisValue";
  case JoinTicketError::UidMismatch: return "UidMismatch";
  }
  return "Unknown";
}

} // namespace zone_auth
