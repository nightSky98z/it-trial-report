#pragma once
#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <vector>

#include <mysql.h>

/**
 * @file mysql_client.h
 * @brief MySQL 従来プロトコル（3306）のクライアントラッパー
 *
 * 以前は X DevAPI（33060）を使っていた。X Protocol は 33060 でしか提供されないため、
 * 3306 を使うには従来プロトコルに切り替える必要がある。そこで libmysqlclient の上に薄いラッパーを用意し、
 * sql().bind().execute() / fetchOne() という呼び出しの形を保っている。
 *
 * この層が行うのは次の 3 つだけで、それ以外のコストは隠さない。
 * - 接続と TLS のオプション
 * - プレースホルダ `?` のエスケープと置換（従来プロトコルのテキストクエリ）
 * - 結果セットの行／列の借用ビュー
 *
 * @note
 * - 従来プロトコルのテキスト結果セットではすべての列が文字列になる。数値としての解釈は utils::safeTo* が行う。
 * - Session はスレッドセーフではない。各スレッドが自分専用の Session を持つこと。
 */
namespace db {

/**
 * @brief データベース操作の失敗
 *
 * @note
 * - 接続・クエリ・トランザクションの失敗はすべてこの型で投げる。呼び出し側は DB との境界で必ず捕捉し、
 *   tick ループやプロトコル処理の境界を越えて伝播させないこと。
 */
class Error : public std::exception {
public:
  explicit Error(std::string message) : message_(std::move(message)) {}
  const char *what() const noexcept override { return message_.c_str(); }

private:
  std::string message_;
};

/**
 * @brief 結果セット内の 1 フィールド（借用ビュー）
 *
 * @note
 * - 所属する Result のメモリを借用するだけで、リソースは所有しない。
 *   その Result より長く保持してはならない。
 * - 従来プロトコルのテキスト結果では、NULL 以外の値はすべて文字列。
 */
class Value {
public:
  Value() = default;
  Value(const char *data, std::size_t length)
      : data_(data), length_(length) {}

    /// @retval true このフィールドは SQL の NULL。呼び出し側は代替値を使うこと。
  bool isNull() const { return data_ == nullptr; }

    /// @return フィールドのテキスト。NULL のフィールドは空文字列を返す。
  std::string str() const {
    return data_ ? std::string(data_, length_) : std::string();
  }

    /// 既存の呼び出し箇所の `std::string(row[i])` と互換にするための明示的な変換。
  explicit operator std::string() const { return str(); }

private:
  const char *data_ = nullptr;
  std::size_t length_ = 0;
};

/**
 * @brief 結果セット内の 1 行（借用ビュー）
 *
 * @note
 * - 行を取得できたかどうかは operator bool で判定する。fetchOne() で最後まで読むと空の行を返す。
 * - 範囲外の添字はクラッシュせずに NULL 値を返す。設定と schema の不一致を
 *   safeTo* の代替値の経路にまとめられるようにするため。
 */
class Row {
public:
  Row() = default;
  Row(MYSQL_ROW row, const unsigned long *lengths, unsigned int fieldCount)
      : row_(row), lengths_(lengths), fieldCount_(fieldCount) {}

  explicit operator bool() const { return row_ != nullptr; }

  Value operator[](std::size_t index) const {
    if (!row_ || index >= fieldCount_) return Value{};
    return Value{row_[index], static_cast<std::size_t>(lengths_[index])};
  }

private:
  MYSQL_ROW row_ = nullptr;
  const unsigned long *lengths_ = nullptr;
  unsigned int fieldCount_ = 0;
};

/**
 * @brief 1 回のクエリの結果セット
 *
 * @note
 * - 内部の MYSQL_RES を所有し、デストラクタで解放する。ムーブのみ可能でコピーはできない。
 * - fetchOne() による 1 行ずつの読み取りと、range-for による走査の両方に対応するが、
 *   両者は同じカーソルを共有するので混在させないこと。
 */
class Result {
public:
  Result() = default;
  Result(MYSQL_RES *res, uint64_t affectedRows)
      : res_(res), affectedRows_(affectedRows) {
    if (res_) fieldCount_ = mysql_num_fields(res_);
  }

  Result(const Result &) = delete;
  Result &operator=(const Result &) = delete;

  Result(Result &&other) noexcept { moveFrom(other); }
  Result &operator=(Result &&other) noexcept {
    if (this != &other) {
      reset();
      moveFrom(other);
    }
    return *this;
  }

  ~Result() { reset(); }

    /**
     * @brief 次の行を取得する
     *
     * @return 行のビュー。最後まで読んだ場合や、この文に結果セットがない場合は空の行（operator bool が false）を返す。
     */
  Row fetchOne() {
    if (!res_) return Row{};
    MYSQL_ROW row = mysql_fetch_row(res_);
    if (!row) return Row{};
    return Row{row, mysql_fetch_lengths(res_), fieldCount_};
  }

    /// @return INSERT/UPDATE/DELETE で影響を受けた行数。SELECT では意味を持たない。
  uint64_t getAffectedItemsCount() const { return affectedRows_; }

    /**
     * @brief range-for 用の入力イテレータ
     *
     * @note
     * - 1 回限りの走査で、間接参照すると現在の行が得られる。fetchOne() とカーソルを共有する。
     */
  class Iterator {
  public:
    Iterator() = default;
    explicit Iterator(Result *owner) : owner_(owner) { advance(); }

    const Row &operator*() const { return current_; }
    Iterator &operator++() {
      advance();
      return *this;
    }
    bool operator!=(const Iterator &other) const {
      return owner_ != other.owner_;
    }

  private:
    void advance() {
      if (!owner_) return;
      current_ = owner_->fetchOne();
      if (!current_) owner_ = nullptr;  // 末尾に達した。end() と同等
    }

    Result *owner_ = nullptr;
    Row current_;
  };

  Iterator begin() { return Iterator{this}; }
  Iterator end() { return Iterator{}; }

private:
  void reset() {
    if (res_) {
      mysql_free_result(res_);
      res_ = nullptr;
    }
  }
  void moveFrom(Result &other) {
    res_ = other.res_;
    affectedRows_ = other.affectedRows_;
    fieldCount_ = other.fieldCount_;
    other.res_ = nullptr;
    other.affectedRows_ = 0;
    other.fieldCount_ = 0;
  }

  MYSQL_RES *res_ = nullptr;
  uint64_t affectedRows_ = 0;
  unsigned int fieldCount_ = 0;
};

class Session;

/**
 * @brief 実行待ちの文：バインドされたパラメータを集め、プレースホルダを置き換える
 *
 * @note
 * - Session を借用するだけなので、その Session が生きている間に execute() すること。
 * - bind() するパラメータの数は SQL 中の `?` の数と一致させること。一致しないと execute() が Error を投げる。
 * - 文字列のパラメータは mysql_real_escape_string でエスケープし、数値は 10 進数でそのまま書式化する。
 */
class Statement {
public:
  Statement(Session &session, std::string sql)
      : session_(&session), sql_(std::move(sql)) {}

    /// 任意個のパラメータをバインドする。メソッドチェーンが可能で、複数回 bind すると順に追加される。
  template <typename... Args>
  Statement &bind(Args &&...args) {
    (appendParam(std::forward<Args>(args)), ...);
    return *this;
  }

    /**
     * @brief 文を実行する
     *
     * @return 結果セット。SELECT 以外の文では結果セットは空だが、影響を受けた行数は読み取れる。
     *
     * @throws Error プレースホルダの数が合わない、接続が使えない、またはサーバーがエラーを返した場合。
     */
  Result execute();

private:
  void appendParam(const std::string &v) { params_.push_back(quoted(v)); }
  void appendParam(const char *v) {
    params_.push_back(v ? quoted(std::string(v)) : std::string("NULL"));
  }
  void appendParam(bool v) { params_.push_back(v ? "1" : "0"); }
  void appendParam(int v) { params_.push_back(std::to_string(v)); }
  void appendParam(unsigned v) { params_.push_back(std::to_string(v)); }
  void appendParam(long v) { params_.push_back(std::to_string(v)); }
  void appendParam(unsigned long v) { params_.push_back(std::to_string(v)); }
  void appendParam(long long v) { params_.push_back(std::to_string(v)); }
  void appendParam(unsigned long long v) { params_.push_back(std::to_string(v)); }
  void appendParam(float v) { appendFloating(static_cast<double>(v), 9); }
  void appendParam(double v) { appendFloating(v, 17); }

    /// 往復しても値が変わらない精度で浮動小数点を書式化する。実装は .cpp にあり、有限でない値は Error を投げる。
  void appendFloating(double v, int significantDigits);

    /// 接続の文字セットでエスケープして引用符で囲む。実装は .cpp にあり、Session の MYSQL* が必要。
  std::string quoted(const std::string &raw) const;

  Session *session_ = nullptr;
  std::string sql_;
  std::vector<std::string> params_;
};

/**
 * @brief MySQL 従来プロトコルの接続 1 本
 *
 * @note
 * - 構築と同時に接続し、失敗したら Error を投げる。デストラクタで接続を閉じる。
 * - ムーブのみ可能でコピーはできない。スレッドセーフではないので、スレッドごとに専用のものを使う。
 */
class Session {
public:
    /**
     * @brief 接続を確立する
     *
     * @param host サーバーのアドレス。
     * @param port 従来プロトコルのポート（3306）。
     * @param user ユーザー名。
     * @param password パスワード。
     * @param database デフォルトのデータベース。空文字列でもよい。
     * @param useTls true なら TLS を必須にし、false なら無効にする。
     *
     * @throws Error 接続または初期化に失敗した場合。
     */
  Session(const char *host, int port, const char *user, const char *password,
          const char *database, bool useTls);

  Session(const Session &) = delete;
  Session &operator=(const Session &) = delete;
  Session(Session &&other) noexcept : conn_(other.conn_) { other.conn_ = nullptr; }
  Session &operator=(Session &&other) noexcept {
    if (this != &other) {
      close();
      conn_ = other.conn_;
      other.conn_ = nullptr;
    }
    return *this;
  }
  ~Session() { close(); }

    /// 実行待ちの文を作る。SQL 中の `?` がパラメータの位置を表す。
  Statement sql(std::string statement) {
    return Statement{*this, std::move(statement)};
  }

    /// @throws Error トランザクションの開始に失敗した場合。
  void startTransaction();
    /// @throws Error コミットに失敗した場合。
  void commit();
    /// @throws Error ロールバックに失敗した場合。
  void rollback();

    /// 内部のハンドル。同じ層の Statement だけが使う。業務コードから直接触らないこと。
  MYSQL *native() const { return conn_; }

private:
  void close();

  MYSQL *conn_ = nullptr;
};

} // namespace db
