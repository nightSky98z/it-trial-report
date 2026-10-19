#pragma once
#include "concurrentqueue.h"
#include <cstdint>
#include <string>
#include <variant>

/**
 * @brief プレイヤー船の経験値・レベルの永続化タスク
 *
 * @note
 * - タスクは値でキューに入れる。投入後、生産側は外部の文字列や ECS コンポーネントの寿命に依存しないこと。
 * - Redis ワーカーは MySqlSaveShipExp を先に MySQL ワーカーへ転送し、その後ベストエフォートで Redis キャッシュに書く。
 * - roleId + unitId が重複排除のキー。後から投入したタスクが、同じ船の古い経験値タスクを上書きする。
 */
struct ShipExpTask {
  std::string roleId;
  uint32_t    unitId = 0;
  uint32_t    level  = 1;
  uint64_t    exp    = 0;
};

/**
 * @brief プレイヤー船のエリア割り当ての永続化タスク
 *
 * @note
 * - タスクは値でキューに入れる。areaId は業務上のエリア表の値であること。0 は無効で、投入してはならない。
 * - ロジックスレッドから MySQL ワーカーへ直接投入し、Redis キャッシュは経由しない。
 * - roleId + unitId が重複排除のキー。後から投入したタスクが、同じ船の古いエリアタスクを上書きする。
 */
struct ShipAreaTask {
  std::string roleId;
  uint32_t    unitId = 0;
  uint32_t    areaId = 1;
};

/**
 * @brief MySQL ワーカーがプレイヤー船の経験値を保存する最終タスク
 *
 * @note
 * - Redis ワーカーまたはロジックスレッドが生成する。MySQL ワーカーは既存の role_units だけを更新する。
 * - role_units が存在しないのはキャラクターの所有関係の異常を意味するので、消費側は警告を出してこの経験値タスクをスキップする。
 * - 文字列はタスクオブジェクトが所有しており、スレッド間で受け渡しても外部のメモリは借用しない。
 */
struct MySqlSaveShipExp {
  std::string roleId;
  uint32_t    unitId = 0;
  uint32_t    level  = 1;
  uint64_t    exp    = 0;
};

/**
 * @brief MySQL ワーカーがプレイヤー船のエリアを保存する最終タスク
 *
 * @note
 * - MySQL ワーカーが消費して role_unit_area_assignments に upsert する。
 * - areaId はデータテーブルのエリア ID（1 始まり）。0 を有効な業務値として書き込んではならない。
 */
struct MySqlSaveShipArea {
  std::string roleId;
  uint32_t    unitId = 0;
  uint32_t    areaId = 1;
};

/**
 * @brief MySQL ワーカーがバッグのアイテム 1 種類の数量を保存する最終タスク
 *
 * @note
 * - count は差分ではなく変更後の数量（絶対値）。消費側は roleId+itemId で重複を排除し、後から来たものが先のものを上書きする。
 *   絶対値で扱うことで、タスクの欠落や順序の入れ替わりがあっても数量がずれない。
 * - count == 0 はそのアイテムがなくなったことを表す。消費側は 0 を書き込むのではなく、該当する role_items の行を削除すること。
 * - role_items.role_id は BIGINT だが、メモリ上の roleId は文字列。
 *   消費側で明示的に変換し、失敗したらタスクを破棄すること。黙って 0 を書き込んではならない。
 */
struct MySqlSaveRoleItem {
  std::string roleId;
  uint32_t    itemId = 0;
  uint32_t    count  = 0;
};

/**
 * @brief MySQL ワーカーがキャラクターのゴールドを保存する最終タスク
 *
 * @note
 * - gold は変更後の絶対値。roleId で重複を排除し、後から来たものが先のものを上書きする。
 * - roles.role_id は VARCHAR なので、数値に変換せず文字列のままバインドする。
 */
struct MySqlSaveRoleGold {
  std::string roleId;
  uint32_t    gold = 0;
};

/**
 * @brief MySQL ワーカーが装備スロット 1 つを保存する最終タスク
 *
 * @note
 * - roleId+unitId+slot で重複を排除する。タスクはそのスロットの変更後の状態（絶対値）を持つ。
 * - itemId == 0 はスロットが空になったことを表す。消費側は該当する unit_equips の行を削除すること。
 * - unit_equips.role_id は BIGINT なので、消費側で明示的に数値へ変換し、失敗したらタスクを破棄すること。
 */
struct MySqlSaveUnitEquip {
  std::string roleId;
  uint32_t    unitId = 0;
  uint8_t     slot = 0;
  uint32_t    itemId = 0;
};

/**
 * @brief MySQL ワーカーがキャラクターの建物 1 つの状態を保存する最終タスク
 *
 * @note
 * - roleId+buildingId で重複を排除する。タスクはその建物の変更後の状態（絶対値）を持つ。
 * - role_buildings.role_id は roles と同じ VARCHAR(32) なので、文字列のままバインドする。
 *   role_items/unit_equips のような数値変換の処理を通さないこと。
 */
struct MySqlSaveRoleBuilding {
  std::string roleId;
  uint32_t    buildingId = 0;
  float       x = 0.f;
  float       y = 0.f;
  uint32_t    level = 1;
  uint8_t     state = 0;
};

/**
 * @brief MySQL ワーカーがキャラクターの精霊を追加する（募集）最終タスク
 *
 * @note
 * - 募集の成功後に投入する。消費側は INSERT IGNORE を使うので、重複して投入しても行は重複しない。
 * - ステータスは unit_templates から role_units へ一度コピーし、以降は role_units の値を正とする。
 * - role_units.role_id は VARCHAR なので、文字列のままバインドする。
 */
struct MySqlInsertRoleUnit {
  std::string roleId;
  uint32_t    unitId = 0;
  uint32_t    baseHp = 100;
  uint32_t    baseAtk = 10;
  uint32_t    baseDef = 0;
  uint32_t    baseSpeed = 100;
  float       attackRange = 1.f;
};

/**
 * @brief MySQL ワーカーが編成スロット 1 つを保存する最終タスク
 *
 * @note
 * - roleId+teamId+slotIndex で重複を排除する。タスクはそのスロットの変更後の状態（絶対値）を持つ。
 * - unitId == 0 は編成から外すことを表す。消費側はそのスロットの行を削除すること。
 * - team_slots.role_id は VARCHAR なので、文字列のままバインドする。
 */
struct MySqlSaveTeamSlot {
  std::string roleId;
  uint8_t     teamId = 0;
  uint8_t     slotIndex = 0;
  uint32_t    unitId = 0;
};

/**
 * @brief MySQL ワーカーが出撃中の編成を保存する最終タスク
 *
 * @note
 * - roleId で重複を排除する。タスクは切り替え後の値（絶対値）を持つ。
 */
struct MySqlSaveSelectedTeam {
  std::string roleId;
  uint8_t     teamId = 1;
};

/**
 * @brief MySQL ワーカーの永続化タスクの共用体（variant）
 *
 * @note
 * - 消費側は variant の型で処理を振り分けること。キューに特定の種類のタスクしか入っていないと仮定しないこと。
 */
using MySqlTask = std::variant<MySqlSaveShipExp, MySqlSaveShipArea,
                               MySqlSaveRoleItem, MySqlSaveRoleGold,
                               MySqlSaveUnitEquip, MySqlSaveRoleBuilding,
                               MySqlInsertRoleUnit, MySqlSaveTeamSlot,
                               MySqlSaveSelectedTeam>;

/**
 * @brief ロジックスレッドから Redis ワーカーへの経験値タスクのキュー
 *
 * @note
 * - 生産側は単純なタスクを enqueue するだけ。Redis へのキャッシュと転送は Redis ワーカーが担当する。
 * - 最終的な DB 保存、重複排除、失敗時のリトライは MySQL ワーカーが担当する。Redis が使えなくても最終的な書き戻しを止めてはならない。
 */
inline moodycamel::ConcurrentQueue<ShipExpTask>  g_ship_exp_tasks;

/**
 * @brief ロジックスレッドから MySQL ワーカーへのエリアタスクのキュー
 *
 * @note
 * - 生産側は単純なタスクを enqueue するだけ。重複排除、一括書き込み、失敗時のリトライは消費側が担当する。
 */
inline moodycamel::ConcurrentQueue<ShipAreaTask> g_ship_area_tasks;

/**
 * @brief Redis ワーカーまたはロジックスレッドから MySQL ワーカーへの最終タスクのキュー
 *
 * @note
 * - 取り出したタスクは消費側が所有し、完了させるか再投入する。生産側は投入後にタスクオブジェクトへアクセスしない。
 */
inline moodycamel::ConcurrentQueue<MySqlTask>    g_mysql_tasks;
