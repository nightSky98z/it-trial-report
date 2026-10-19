#include "db/player_ship_db_writer.h"
#include "db/db_types.h"

namespace player_ship_db_writer {

/**
 * @brief 経験値が変化した船を走査してキューに投入する
 *
 * @note
 * - tick の最後にだけ呼ぶ。処理したコンポーネントの dirtyStats をクリアする。
 */
void flushDirtyExp(entt::registry &world) {
  auto view = world.view<CExperience, UnitBase, Owner>();
  for (auto e : view) {
    auto &exp = view.get<CExperience>(e);
    if (!exp.dirtyStats) continue;
    const auto &base  = view.get<UnitBase>(e);
    const auto &owner = view.get<Owner>(e);
    if (owner.roleId.empty() || base.unitId == 0) {
      exp.dirtyStats = false;
      continue;
    }
    g_ship_exp_tasks.enqueue(ShipExpTask{
        owner.roleId, base.unitId, exp.level, exp.currentXp});
    exp.dirtyStats = false;
  }
}

/**
 * @brief エリア割り当てタスクを投入する
 *
 * @note
 * - 投入後の文字列はキューの要素が所有するので、別スレッドで消費しても安全。
 */
void enqueueShipArea(const std::string &roleId, uint32_t unitId,
                     uint32_t areaId) {
  g_ship_area_tasks.enqueue(ShipAreaTask{roleId, unitId, areaId});
}

/**
 * @brief ログアウト前に、キャラクターの全プレイヤー船の状態を投入する
 *
 * @note
 * - プレイヤー船のエンティティを破棄する前に呼ぶこと。
 */
void flushAllShips(entt::registry &world, const std::string &roleId) {
  auto view = world.view<UnitBase, Owner, CPlayerShipAssignment, CExperience>();
  for (auto e : view) {
    const auto &owner = view.get<Owner>(e);
    if (owner.roleId != roleId) continue;
    const auto &base   = view.get<UnitBase>(e);
    const auto &assign = view.get<CPlayerShipAssignment>(e);
    auto &exp          = view.get<CExperience>(e);

    if (base.unitId == 0) continue;

    g_ship_exp_tasks.enqueue(ShipExpTask{
        roleId, base.unitId, exp.level, exp.currentXp});
    g_ship_area_tasks.enqueue(ShipAreaTask{
        roleId, base.unitId, assign.areaId});
    exp.dirtyStats = false;
  }
}

} // namespace player_ship_db_writer
