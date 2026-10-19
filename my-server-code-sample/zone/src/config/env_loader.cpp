#include "config/env_loader.h"

#include <cctype>
#include <fstream>
#include <limits>

namespace config {

/**
 * @brief env の行の前後の空白を取り除く
 *
 * @return 前後の ASCII 空白を除いた文字列（所有権あり）。
 */
std::string EnvLoader::Trim(const std::string &s) {
  size_t start = 0;
  while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
    ++start;
  }

  size_t end = s.size();
  while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
    --end;
  }

  return s.substr(start, end - start);
}

/**
 * @brief env ファイルを読み込み、内部の key/value テーブルに格納する
 *
 * @retval true ファイルを開いて正常に解析できた。
 * @retval false ファイルを開けない、または不正な行がある。err に理由を書き込む。
 */
bool EnvLoader::LoadFile(const std::string &path, std::string &err) {
  std::ifstream ifs(path);
  if (!ifs.is_open()) {
    err = "failed to open env file: " + path;
    return false;
  }

  std::unordered_map<std::string, std::string> parsed;
  std::string line;
  int line_no = 0;
  while (std::getline(ifs, line)) {
    ++line_no;
    std::string trimmed = Trim(line);
    if (trimmed.empty() || trimmed[0] == '#') {
      continue;
    }

    const auto pos = trimmed.find('=');
    if (pos == std::string::npos) {
      err = "invalid env line " + std::to_string(line_no) + ": missing '='";
      return false;
    }

    std::string key = Trim(trimmed.substr(0, pos));
    std::string value = Trim(trimmed.substr(pos + 1));
    if (key.empty()) {
      err = "invalid env line " + std::to_string(line_no) + ": empty key";
      return false;
    }

    if (value.size() >= 2) {
      const char first = value.front();
      const char last = value.back();
      if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
        value = value.substr(1, value.size() - 2);
      }
    }

    parsed[std::move(key)] = std::move(value);
  }

  if (ifs.bad()) {
    err = "failed while reading env file: " + path;
    return false;
  }

  values_ = std::move(parsed);
  return true;
}

/**
 * @brief 文字列の設定値を取得する
 *
 * @return string: key が存在する。
 * @retval nullopt key が存在しない。
 */
std::optional<std::string> EnvLoader::GetString(const std::string &key) const {
  auto it = values_.find(key);
  if (it == values_.end()) {
    return std::nullopt;
  }
  return it->second;
}

/**
 * @brief 0 以上の 10 進整数の設定値を取得する
 *
 * @return int: key が存在し、全体を数値として解析できた。
 * @retval nullopt key がない・空・数字以外を含む・オーバーフローした。
 */
std::optional<int> EnvLoader::GetInt(const std::string &key) const {
  auto v = GetString(key);
  if (!v.has_value()) {
    return std::nullopt;
  }

  if (v->empty()) {
    return std::nullopt;
  }

  int parsed = 0;
  for (char ch : *v) {
    const auto uch = static_cast<unsigned char>(ch);
    if (!std::isdigit(uch)) {
      return std::nullopt;
    }
    const int digit = static_cast<int>(uch - static_cast<unsigned char>('0'));
    if (parsed > (std::numeric_limits<int>::max() - digit) / 10) {
      return std::nullopt;
    }
    parsed = parsed * 10 + digit;
  }

  return parsed;
}

} // namespace config
