#include "db/role_economy_db_writer.h"
#include "db/db_types.h"

namespace role_economy_db_writer {

/**
 * @brief バッグのアイテム数を保存するタスクを投入する
 *
 * @note
 * - 投入後の文字列はキューの要素が所有するので、別スレッドで消費しても安全。
 */
void enqueueItemCount(const std::string &roleId, uint32_t itemId,
                      uint32_t newCount) {
  if (roleId.empty() || itemId == 0) return;
  g_mysql_tasks.enqueue(MySqlSaveRoleItem{roleId, itemId, newCount});
}

/**
 * @brief ゴールドを保存するタスクを投入する
 *
 * @note
 * - 投入後の文字列はキューの要素が所有するので、別スレッドで消費しても安全。
 */
void enqueueGold(const std::string &roleId, uint32_t newGold) {
  if (roleId.empty()) return;
  g_mysql_tasks.enqueue(MySqlSaveRoleGold{roleId, newGold});
}

/**
 * @brief 装備スロットを保存するタスクを投入する
 *
 * @note
 * - 投入後の文字列はキューの要素が所有するので、別スレッドで消費しても安全。
 */
void enqueueUnitEquip(const std::string &roleId, uint32_t unitId, uint8_t slot,
                      uint32_t itemId) {
  if (roleId.empty() || unitId == 0 || slot == 0) return;
  g_mysql_tasks.enqueue(MySqlSaveUnitEquip{roleId, unitId, slot, itemId});
}

/**
 * @brief 建物の状態を保存するタスクを投入する
 *
 * @note
 * - 投入後の文字列はキューの要素が所有するので、別スレッドで消費しても安全。
 */
void enqueueRoleBuilding(const std::string &roleId, uint32_t buildingId,
                         float x, float y, uint32_t level, uint8_t state) {
  if (roleId.empty() || buildingId == 0) return;
  g_mysql_tasks.enqueue(
      MySqlSaveRoleBuilding{roleId, buildingId, x, y, level, state});
}

/**
 * @brief 募集で加わった精霊を保存するタスクを投入する
 */
void enqueueRoleUnit(const std::string &roleId, uint32_t unitId,
                     uint32_t baseHp, uint32_t baseAtk, uint32_t baseDef,
                     uint32_t baseSpeed, float attackRange) {
  if (roleId.empty() || unitId == 0) return;
  g_mysql_tasks.enqueue(MySqlInsertRoleUnit{roleId, unitId, baseHp, baseAtk,
                                            baseDef, baseSpeed, attackRange});
}

/**
 * @brief 編成スロットを保存するタスクを投入する
 */
void enqueueTeamSlot(const std::string &roleId, uint8_t teamId,
                     uint8_t slotIndex, uint32_t unitId) {
  if (roleId.empty() || teamId == 0) return;
  g_mysql_tasks.enqueue(MySqlSaveTeamSlot{roleId, teamId, slotIndex, unitId});
}

/**
 * @brief 出撃する編成を保存するタスクを投入する
 */
void enqueueSelectedTeam(const std::string &roleId, uint8_t teamId) {
  if (roleId.empty() || teamId == 0) return;
  g_mysql_tasks.enqueue(MySqlSaveSelectedTeam{roleId, teamId});
}

} // namespace role_economy_db_writer
