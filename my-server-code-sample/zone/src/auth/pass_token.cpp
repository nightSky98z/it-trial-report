#include "auth/pass_token.h"

#include <array>
#include <charconv>
#include <openssl/rand.h>
#include <string_view>
#include <vector>

namespace zone_auth {
namespace {

/**
 * @brief uint64 の 10 進数テキストを厳密に解析する
 *
 * @retval true 文字列全体を解析でき、オーバーフローもしていない。
 * @retval false 空文字列、不正な文字、またはオーバーフロー。
 */
bool parseU64Strict(std::string_view text, uint64_t &out) {
  if (text.empty()) return false;
  uint64_t value = 0;
  const char *begin = text.data();
  const char *end = text.data() + text.size();
  auto result = std::from_chars(begin, end, value);
  if (result.ec != std::errc{} || result.ptr != end) return false;
  out = value;
  return true;
}

/**
 * @brief uint32 の 10 進数テキストを厳密に解析する
 *
 * @retval true 文字列全体を解析でき、uint32 の範囲に収まっている。
 * @retval false テキストが不正、または uint32 の上限を超えている。
 */
bool parseU32Strict(std::string_view text, uint32_t &out) {
  uint64_t value = 0;
  if (!parseU64Strict(text, value)) return false;
  if (value > UINT32_MAX) return false;
  out = static_cast<uint32_t>(value);
  return true;
}

/**
 * @brief テキストに passToken レコードの区切り文字が含まれるかを判定する
 *
 * @retval true '|' が含まれる。
 */
bool hasSeparator(std::string_view text) {
  return text.find('|') != std::string_view::npos;
}

/**
 * @brief レコードを '|' で分割し、借用した断片として返す
 *
 * @return 各フィールドの string_view。断片の寿命は data を超えない。
 */
std::vector<std::string_view> splitRecord(std::string_view data) {
  std::vector<std::string_view> parts;
  size_t pos = 0;
  while (pos <= data.size()) {
    const size_t sep = data.find('|', pos);
    if (sep == std::string_view::npos) {
      parts.push_back(data.substr(pos));
      break;
    }
    parts.push_back(data.substr(pos, sep - pos));
    pos = sep + 1;
  }
  return parts;
}

} // namespace

/**
 * @brief 新しい passToken を生成する
 *
 * @param out 呼び出し側が所有する出力先。失敗時はクリアされる。
 *
 * @retval true out に固定長の 16 進数トークンを書き込んだ。
 * @retval false OpenSSL の RAND_bytes が失敗した。
 */
bool generatePassToken(std::string &out) {
  std::array<uint8_t, kPassTokenRandomBytes> randomBytes{};
  if (RAND_bytes(randomBytes.data(), static_cast<int>(randomBytes.size())) != 1) {
    out.clear();
    return false;
  }

  static constexpr char kHex[] = "0123456789abcdef";
  out.resize(kPassTokenHexBytes);
  for (size_t i = 0; i < randomBytes.size(); ++i) {
    out[i * 2] = kHex[randomBytes[i] >> 4];
    out[i * 2 + 1] = kHex[randomBytes[i] & 0x0f];
  }
  return true;
}

/**
 * @brief passToken レコードをシリアライズする
 *
 * @return '|' 区切りの文字列（所有権あり）。
 */
std::string serializePassTokenRecord(const PassTokenRecord &rec) {
  std::string data;
  data.reserve(rec.uid.size() + rec.roleId.size() + 80);
  data += std::to_string(rec.zoneId);
  data.push_back('|');
  data += rec.uid;
  data.push_back('|');
  data += rec.roleId;
  data.push_back('|');
  data += std::to_string(rec.mapId);
  data.push_back('|');
  data += std::to_string(rec.issuedAtMs);
  data.push_back('|');
  data += std::to_string(rec.expireAtMs);
  data.push_back('|');
  data += std::to_string(rec.version);
  return data;
}

/**
 * @brief Redis 上の passToken レコードを解析・検証する
 *
 * @retval true out に有効なレコードを書き込んだ。
 * @retval false フィールド数・zoneId・mapId・バージョン・有効期限のいずれかが不正。
 */
bool parsePassTokenRecord(std::string_view data,
                          uint32_t expectedZoneId,
                          uint64_t nowMs,
                          PassTokenRecord &out) {
  auto parts = splitRecord(data);
  if (parts.size() != 7) return false;

  PassTokenRecord rec{};
  if (!parseU32Strict(parts[0], rec.zoneId)) return false;
  if (rec.zoneId == 0 || rec.zoneId != expectedZoneId) return false;
  if (parts[1].empty() || parts[2].empty()) return false;
  if (hasSeparator(parts[1]) || hasSeparator(parts[2])) return false;
  rec.uid.assign(parts[1].data(), parts[1].size());
  rec.roleId.assign(parts[2].data(), parts[2].size());
  if (!parseU32Strict(parts[3], rec.mapId) || rec.mapId == 0 || rec.mapId > 255) {
    return false;
  }
  if (!parseU64Strict(parts[4], rec.issuedAtMs)) return false;
  if (!parseU64Strict(parts[5], rec.expireAtMs)) return false;
  if (!parseU32Strict(parts[6], rec.version) || rec.version == 0) return false;
  if (nowMs >= rec.expireAtMs) return false;

  out = std::move(rec);
  return true;
}

/**
 * @brief passToken テキストの形式を検証する
 *
 * @retval true トークンが固定長の小文字 16 進数である。
 * @retval false 長さまたは文字が不正。
 */
bool isValidPassTokenText(std::string_view token) {
  if (token.size() != kPassTokenHexBytes) return false;
  for (char ch : token) {
    const bool digit = ch >= '0' && ch <= '9';
    const bool lower = ch >= 'a' && ch <= 'f';
    if (!digit && !lower) return false;
  }
  return true;
}

} // namespace zone_auth
