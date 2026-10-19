#include "ecs/city_monster_combat_system.h"
#include "ecs/city_combat_helpers.h"
#include "ecs/nav_components.h"
#include "ecs/player_ship_components.h"
#include "game/city_nav_grid.h"
#include "game/nav_grid.h"
#include "generated/game_generated.h"

namespace city_monster_combat_system {

namespace {

/**
 * @brief 同じエリア内で、攻撃できる最も近いプレイヤー船を選ぶ
 *
 * @return プレイヤー船のエンティティ。
 * @retval entt::null エリア内に Alive のプレイヤー船がいない。
 *
 * @param world マップの ECS world。読み取りのみ。
 * @param areaId モンスターが属するエリア。
 * @param self モンスター自身の基本コンポーネント。距離の計算に使う。
 * @param sensorRadius 索敵半径。0 以下なら距離の制限なし。
 *
 * @note
 * - プレイヤー船を O(N) で走査する。現在のメインシティの規模なら問題ない。
 * - 判定に使うのは CAreaMember.areaId だけで、プレイヤー船の割り当て先エリアは見ない。
 * - **移動中の精霊はターゲットにしない**。理由は関数内のコメントを参照。
 * - sensorRadius を超える対象はスキップする。この制限がないと、精霊がエリアに入った瞬間に
 *   エリア内の**すべての**モンスターから同時に狙われ、到着と同時に袋叩きにされる。
 */
entt::entity pickPlayerShipInArea(entt::registry &world, uint32_t areaId,
                                  const UnitBase &self, float sensorRadius) {
  auto view = world.view<CAreaMember, UnitBase, CPlayerShipAI>();
  entt::entity best = entt::null;
    // 半径 0 以下は距離無制限として扱う。UnitSensor を持たないエンティティが何もできなくなるのを防ぐため。
  float bestD2 = sensorRadius > 0.f ? sensorRadius * sensorRadius : 1e30f;
  for (auto e : view) {
    const auto &member = view.get<CAreaMember>(e);
    if (member.areaId != areaId) continue;
    const auto &tb = view.get<UnitBase>(e);
    if (!city_combat::isAlive(tb)) continue;   // Dying / Dead を除外

        // 移動中の精霊は戦闘に参加しないので、モンスターもターゲットに選んではならない。
        //
        // CAreaMember.areaId は精霊がアンカーに到着するまで切り替わらないため、移動中はずっと
        // **出発元**のエリア番号のままになっている。除外しないと、本番環境で次の 2 つの現象が起きる。
        // ① 病院で HP を回復してエリアへ向かう途中（まだメインシティにいる）で元のエリアのモンスターに追いかけられ、
        //    着いたころにはまた HP がわずかになっている。「回復しても意味がない」ループに陥る。
        // ② 編成を切り替えて精霊が旧エリアを離れると、旧エリアのモンスターがずっと追いかけてきて、
        //    エリア入口の狭い通路で団子になる。
        // movement_clamp_system はモンスターを矩形内に押し戻すだけで、ターゲットを諦めさせはしないので、
        // モンスターは入口に張り付いたまま攻撃し続けることになる。
        //
        // MovingToAnchor の間は精霊自身も攻撃しないので、一方的に殴られる・一方的に無敵になる、ということはない。
    if (view.get<CPlayerShipAI>(e).state == PlayerShipAIState::MovingToAnchor) {
      continue;
    }
    const float dx = tb.x - self.x;
    const float dy = tb.y - self.y;
    const float d2 = dx * dx + dy * dy;
        // bestD2 の初期値は半径の 2 乗なので、この 1 回の比較で
        // 「索敵範囲内にいるか」と「最も近いものを選ぶ」の 2 つを同時に判定できる。
    if (d2 < bestD2) {
      bestD2 = d2;
      best = e;
    }
  }
  return best;
}

/**
 * @brief モンスターの現在の nav 経路をクリアする
 *
 * @note
 * - 攻撃範囲内で移動を止めるときに呼ぶ。古い経路は NavPathStore に返却する。
 */
inline void clearNav(entt::registry &world, game::CNavAgent &nav) {
  nav.active = false;
  nav.pathDirty = false;
  nav.arrived = true;
  nav.nextIndex = 0;
  game::navClearPath(world, nav);
}

} // namespace

/**
 * @brief メインシティのモンスターの戦闘状態を進める
 *
 * @note
 * - 攻撃が当たると、直ちに city_combat::applyDamage を呼んでイベントと状態を書き込む。
 */
void update(entt::registry &world, MonsterAIEventHub &hub, float dtSec,
            uint32_t mapId) {
  const auto &grid = game::navGridOf(world);
  auto view =
      world.view<UnitBase, UnitCombat, UnitAggro, Velocity, CAreaMember>();
  for (auto e : view) {
    auto &base    = view.get<UnitBase>(e);
    auto &combat  = view.get<UnitCombat>(e);
    auto &aggro   = view.get<UnitAggro>(e);
    auto &vel     = view.get<Velocity>(e);
    const auto &member = view.get<CAreaMember>(e);

    if (base.unitKind != static_cast<uint8_t>(Game::UnitKind_Monster)) continue;
    if (!city_combat::isAlive(base)) continue;

        // 索敵半径はモンスター自身の UnitSensor から取る（モンスターの生成処理がテンプレートに従って設定する）。
        // このコンポーネントがなければ距離無制限とし、従来の動作を保つ。コンポーネントが欠けたエンティティを「盲目」にしないため。
    const auto *sensor = world.try_get<UnitSensor>(e);
    const float sensorRadius = sensor ? sensor->radius : 0.f;

    entt::entity target =
        pickPlayerShipInArea(world, member.areaId, base, sensorRadius);
    if (target == entt::null) {
      const bool wasInCombat = aggro.in_combat;
      aggro.in_combat = false;
      aggro.target_instance_id.clear();
      combat.cooldown_remain_sec =
          std::max(0.f, combat.cooldown_remain_sec - dtSec);
      if (wasInCombat) {
                // 戦闘から離脱する境目で一度だけ古い追跡経路をクリアする。ターゲットがいない状態が続く間は、
                // nav の管理は徘徊システムに任せる。
        vel.vx = 0.f;
        vel.vy = 0.f;
        base.actionState = static_cast<uint8_t>(Game::ActionState_Idle);
        if (world.all_of<game::CNavAgent>(e)) {
          clearNav(world, world.get<game::CNavAgent>(e));
        }
      }
      continue;
    }

    aggro.in_combat = true;
    aggro.target_instance_id =
        std::to_string(static_cast<uint32_t>(target));

    const auto &tb = world.get<UnitBase>(target);
    const float dist = city_combat::distance(base, tb);
    auto &nav = world.get_or_emplace<game::CNavAgent>(e);

    combat.cooldown_remain_sec =
        std::max(0.f, combat.cooldown_remain_sec - dtSec);

    if (dist <= combat.attack_range) {
            // 攻撃範囲に入った：移動を止めて nav を凍結し、nav_follow が古い経路を進み続けないようにする。
      vel.vx = 0.f;
      vel.vy = 0.f;
      clearNav(world, nav);

      if (combat.cooldown_remain_sec > 0.f) {
        base.actionState = static_cast<uint8_t>(Game::ActionState_Idle);
        continue;
      }

      combat.cooldown_remain_sec = combat.attack_cooldown;
      base.actionState = static_cast<uint8_t>(Game::ActionState_Attack);

            // ActionStarted を配信する（クライアントのモーションと同期させる）
      ActionStartedEvent ase;
      ase.entity = e;
      ase.action_type = static_cast<uint8_t>(Game::ActionState_Attack);
      ase.duration = combat.attack_cooldown;
      ase.target_x = tb.x;
      ase.target_y = tb.y;
      hub.action_started.curr.push_back(ase);

      city_combat::applyDamage(world, e, target, combat.attack_power, hub, mapId);
      continue;
    }

    if (dist > combat.attack_range) {
      const auto targetCell = game::navWorldToCell(grid, tb.x, tb.y);
      if (!nav.active || nav.goalCell.x != targetCell.x || nav.goalCell.y != targetCell.y) {
        nav.active = true;
        nav.goalCell = targetCell;
        nav.pathDirty = true;
        nav.arrived = false;
      }
      base.actionState = static_cast<uint8_t>(Game::ActionState_Move);
      continue;
    }
  }
}

} // namespace city_monster_combat_system
