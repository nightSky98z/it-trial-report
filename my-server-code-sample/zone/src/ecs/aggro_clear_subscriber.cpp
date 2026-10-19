#include "ecs/aggro_clear_subscriber.h"
#include <string>

namespace aggro_clear_subscriber {

/**
 * @brief 前フレームで解除要求されたヘイト対象を取り除く
 *
 * @note
 * - hub.aggro_clear.prev を消費してクリアする。
 */
void update(entt::registry &world, MonsterAIEventHub &hub) {
  if (hub.aggro_clear.prev.empty()) return;

  auto monsterView = world.view<UnitAggro, UnitBase>();
  for (const auto &ev : hub.aggro_clear.prev) {
    // threat_table のキーは文字列（target_instance_id）なので、
    // 被害者（victim）の entt::entity を文字列化してキーにする。
    // メインシティでは instance_id = std::to_string((uint32_t)entity) と決めており、
    // monster_combat 側も同じ規則で生成している。
    const std::string victimKey = std::to_string(static_cast<uint32_t>(ev.victim));

    for (auto e : monsterView) {
      auto &ag = monsterView.get<UnitAggro>(e);
      ag.threat_table.erase(victimKey);
      if (ag.target_instance_id == victimKey) {
        ag.target_instance_id.clear();
        ag.in_combat = !ag.threat_table.empty();
        ag.threat = 0.f;
      }
    }
  }
  hub.aggro_clear.prev.clear();
}

} // namespace aggro_clear_subscriber
