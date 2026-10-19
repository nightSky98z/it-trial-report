#include "ecs/healing_station_system.h"
#include "ecs/building_component.h"
#include "ecs/player_ship_components.h"
#include "generated/game_generated.h"
#include <algorithm>
#include <cmath>

namespace healing_station_system {

/**
 * @brief 回復が有効になる最大距離（**保険であり、主な判定条件ではない**）
 *
 * @note
 * - 主な判定条件は AI の状態で、PlayerShipAIState::Healing の精霊だけが回復する。
 *   精霊がこの状態になるのは病院まで歩いて停止した後なので、通常の流れではこの半径で弾かれることはない。
 * - 「状態は Healing なのに病院にいない」といった異常（病院が移動された直後など）を受け止めるためだけに使う。
 * - 値は、撤退時の待機地点から病院中心までの距離（現在は病院の外周から 1 マス外で、約 50〜82）より
 *   **十分大きく**すること。小さすぎると、到着しても回復しきれず Healing 状態から抜けられなくなる。
 *   その場合、精霊が病院の横で棒立ちになり、普通の AI のバグと見分けがつきにくい。
 */
constexpr float kHealingRadius = 300.f;

/**
 * @brief 治療施設が 1 秒あたりに回復する HP
 *
 * @note
 * - 1 フレームあたり最低 1 は回復させる。dt が小さいときに回復が止まらないようにするため。
 */
constexpr float kHpPerSec = 50.f;

/**
 * @brief 病院で治療中のプレイヤー船の HP を回復し、HP 変化イベントを発行する
 *
 * @note
 * - 対象は CBuilding::buildingId == Hospital(3) の建物、つまり DB の buildings.id=3（病院）だけ。
 *   病院の位置はエンティティの x/y から取得する。元データは DB の buildings.default_x/y
 *   （将来的にはプレイヤーが配置した位置）。ここに座標を直書きしてはならない。
 *   病院が移動したとたんに、回復が黙って効かなくなる。
 * - **回復の条件は立ち位置ではなく AI の状態。** 病院まで歩いて Healing になった精霊だけが回復する。
 *   初期実装は単純な距離オーラだったため、2 つの問題が起きた。撤退中に回復が始まってしまうことと、
 *   エリア 1 のアンカー自体が病院に近いため、そこに配置された精霊が戦いながら回復し続けてしまうこと。
 *   状態遷移は player_ship_ai_system の担当で、本システムは「HP を回復する」ことだけを担当する。
 */
void update(entt::registry &world, MonsterAIEventHub &hub, float dtSec,
            uint32_t mapId) {
  auto stationView = world.view<CBuilding>();
  auto shipView = world.view<CPlayerShipAI, UnitBase, UnitCombat>();

  for (auto sEnt : stationView) {
    const auto &bld = stationView.get<CBuilding>(sEnt);
        // 治療施設だけを処理する
    if (bld.buildingId != 3) continue;  // 病院（buildings.id=3）
        // ロック中の建物も価格を送るためにエンティティ化されるが、未解放なら回復は提供しない。
    if (bld.state != 1) continue;
    const float r2 = kHealingRadius * kHealingRadius;

    for (auto ship : shipView) {
      const auto &ai = shipView.get<CPlayerShipAI>(ship);
            // 主な判定：病院に到着して治療状態に入った精霊だけを回復する。
      if (ai.state != PlayerShipAIState::Healing) continue;

      auto &pb = shipView.get<UnitBase>(ship);
      auto &pc = shipView.get<UnitCombat>(ship);
      if (pb.lifeStatus == static_cast<uint8_t>(Game::LifeStatus_Dead)) continue;
      if (pb.curHp >= pb.maxHp) continue;

      const float dx = pb.x - bld.x;
      const float dy = pb.y - bld.y;
      if (dx * dx + dy * dy > r2) continue;

      const uint32_t regen = static_cast<uint32_t>(std::max(1,
          static_cast<int>(std::lround(kHpPerSec * dtSec))));
      const uint32_t before = pb.curHp;
      pb.curHp = std::min(pb.maxHp, pb.curHp + regen);
      pc.cur_hp = static_cast<int>(pb.curHp);
      pc.max_hp = static_cast<int>(pb.maxHp);
      const int delta = static_cast<int>(pb.curHp - before);
      if (delta <= 0) continue;

      HpChangeEvent ev;
      ev.entity = ship;
      ev.map_id = mapId;
      ev.cur_hp = pb.curHp;
      ev.max_hp = pb.maxHp;
      ev.delta  = delta;
      hub.hp_changed.curr.push_back(ev);
    }
  }
}

} // namespace healing_station_system
