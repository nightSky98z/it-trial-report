#pragma once

#include <optional>
#include <string>
#include <unordered_map>

namespace config {

/**
 * @brief シンプルな env ファイルローダー
 *
 * @note
 * - LoadFile の後、values_ が key/value 文字列のコピーを所有する。
 * - シェル展開やコマンド実行はしない。このプロジェクトの env.zone 形式だけを解析する。
 */
class EnvLoader {
public:
    /**
     * @brief env ファイルを読み込む
     *
     * @param path env ファイルのパス。呼び出し中だけ借用する。
     * @param err 失敗時にエラーメッセージを書き込む。
     *
     * @retval true ファイルを解析して内部の values_ に格納した。
     * @retval false ファイルを読めない、または形式が条件を満たさない。
     */
  bool LoadFile(const std::string &path, std::string &err);

    /**
     * @brief 文字列の設定値を取得する
     *
     * @param key env のキー名。呼び出し中だけ借用する。
     *
     * @return string 値があれば key が存在する。
     * @retval nullopt key が存在しない。
     */
  std::optional<std::string> GetString(const std::string &key) const;

    /**
     * @brief 全体が 10 進数として解釈できる 0 以上の int を取得する
     *
     * @param key env のキー名。呼び出し中だけ借用する。
     *
     * @return int 値があれば、key が存在し、空でなく、全文字が数字で、オーバーフローしていない。
     * @retval nullopt key がない、または文字列全体を 0 以上の int として解析できない。
     *
     * @note
     * - 値の範囲（ポート番号・Redis DB・tick rate など）のチェックは上位の設定処理が担当する。
     */
  std::optional<int> GetInt(const std::string &key) const;

private:
    /**
     * @brief 文字列の前後の ASCII 空白を取り除く
     *
     * @param s 入力文字列。呼び出し中だけ借用する。
     *
     * @return 前後の空白を除いた文字列（所有権あり）。
     */
  static std::string Trim(const std::string &s);
  std::unordered_map<std::string, std::string> values_;
};

} // namespace config
