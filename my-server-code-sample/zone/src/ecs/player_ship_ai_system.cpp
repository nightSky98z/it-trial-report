#include "ecs/player_ship_ai_system.h"
#include "ecs/area_registry_system.h"
#include "ecs/building_component.h"
#include "ecs/city_combat_helpers.h"
#include "ecs/nav_components.h"
#include "game/city_nav_grid.h"
#include "game/nav_grid.h"
#include "game/player_ship_constants.h"
#include "generated/game_generated.h"
#include "utils/log.h"
#include <algorithm>

namespace player_ship_ai_system {

namespace {
using namespace player_ship_constants;

void clearNav(entt::registry &world, game::CNavAgent &nav) {
  nav.active = false;
  nav.pathDirty = false;
  nav.arrived = true;
  nav.nextIndex = 0;
  game::navClearPath(world, nav);
}

void stop(UnitBase &base, Velocity &vel) {
  vel.vx = 0.f;
  vel.vy = 0.f;
  base.actionState = static_cast<uint8_t>(Game::ActionState_Idle);
}

void hold(UnitBase &base, Velocity &vel, uint8_t actionState) {
  vel.vx = 0.f;
  vel.vy = 0.f;
  base.actionState = actionState;
}

/**
 * @brief 現在の world が持つ「エリア内ランダム地点選択」用のスクラッチ領域を返す
 *
 * @param world マップの ECS world。スクラッチ領域の寿命は ctx() が管理する。
 *
 * @return 使い回せる候補セル用の一時メモリプールへの参照
 *
 * @note
 * - そのフレームで実際に新しい巡回地点を選ぶ必要があるときだけ呼ぶ。
 * - AI システムは world ごとに単一スレッドで動く。返した参照をスレッド間で共有しないこと。
 */
game::NavAreaPickScratch &areaPickScratch(entt::registry &world) {
  if (!world.ctx().contains<game::NavAreaPickScratch>()) {
    world.ctx().emplace<game::NavAreaPickScratch>();
  }
  return world.ctx().get<game::NavAreaPickScratch>();
}

/**
 * @brief 精霊が治療のためにシティへ戻るとき、向かうべきセルを求める
 *
 * @param world マップの ECS world。建物のエンティティだけを読む。
 * @param grid そのシティの nav グリッド（建物の形状を重ねたもの）。
 *
 * @return 病院の隣で最も近い通行可能セル。解放済みの病院がなければ出現地点にフォールバックする。
 *
 * @note
 * - 病院の座標は DB（buildings.default_x/y、将来はプレイヤーが配置した位置）から取る。直書きしないこと。
 *   病院の中心は病院自身の形状でふさがれているため、隣接する通行可能セルに補正しないと経路探索がいつまでも到達できない。
 * - 出現地点へのフォールバックは、「病院がまだ解放されていない／設定がない」ときに精霊が動けなくならないようにするため。
 *   この場合 healing_station_system は病院を見つけられず回復しないので、精霊は出現地点で待機する。
 * - 毎フレーム 1 回呼べば十分。建物のエンティティは 1 桁しかなく、走査コストは無視できる。
 */
game::NavCell healingApproachCell(entt::registry &world,
                                  const game::NavGridStatic &grid) {
  auto view = world.view<CBuilding>();
  for (auto e : view) {
    const auto &b = view.get<CBuilding>(e);
    if (b.buildingId != 3 || b.state != 1) continue;  // 病院かつ解放済み

    const game::NavCell center = game::navWorldToCell(grid, b.x, b.y);
    if (!game::navIsInside(grid, center)) break;

    game::NavNearestWalkableScratch scratch;
    return game::findNearestWalkableCell(grid, center, scratch);
  }
  return grid.playerSpawnCell;
}

} // namespace

void update(entt::registry &world, MonsterAIEventHub &hub, float dtSec,
            uint64_t nowMs, uint32_t mapId) {
  const auto &grid = game::navGridOf(world);

  auto view = world.view<CPlayerShipAI, CPlayerShipAssignment, UnitBase,
                         UnitCombat, Velocity, CAreaMember>();
  for (auto e : view) {
    auto &ai       = view.get<CPlayerShipAI>(e);
    auto &assign   = view.get<CPlayerShipAssignment>(e);
    auto &base     = view.get<UnitBase>(e);
    auto &combat   = view.get<UnitCombat>(e);
    auto &vel      = view.get<Velocity>(e);
    auto &member   = view.get<CAreaMember>(e);

        // ── 瀕死への切り替え：どの状態でも base.curHp==0 を検知したら Retreating に入る ──
        // 注意：ダメージを正として書き込むのは UnitBase.curHp（city_combat::applyDamage）。
        //      UnitCombat.cur_hp はレベルアップ／回復の処理でしか同期されないので、死亡判定に使ってはならない。
    if (base.curHp == 0 &&
        ai.state != PlayerShipAIState::Retreating &&
        ai.state != PlayerShipAIState::Healing) {
      ai.state = PlayerShipAIState::Retreating;
      ai.deathStayStartedAtMs = nowMs;
      ai.currentTarget = entt::null;
      ai.nextRetargetAtMs = 0;
      ai.hasWanderTarget = false;
      ai.wanderWaitTimer = 0.f;
      city_combat::clearNavMotion(world, e);
      base.lifeStatus  = static_cast<uint8_t>(Game::LifeStatus_Dying);
      base.actionState = static_cast<uint8_t>(Game::ActionState_Retreating);

      AggroClearRequestEvent ag;
      ag.victim = e;
      ag.map_id = mapId;
      hub.aggro_clear.curr.push_back(ag);
    }

        // 滞留タイマーは MovingToAnchor の間だけ有効。この状態を離れたら即座に 0 に戻す。
        // そうしないと、次にこの状態に入ったときに前回の古いタイムスタンプが残っていて、すぐに「停止」と誤報してしまう。
        // この状態に入る経路はいくつもあり、個別に書くと必ず漏れが出るので、switch の前でまとめて処理する。
    if (ai.state != PlayerShipAIState::MovingToAnchor) {
      ai.movingToAnchorSinceMs = 0;
      ai.stuckWarned = false;
    }

    switch (ai.state) {

    case PlayerShipAIState::MovingToAnchor: {
      auto &nav = world.get_or_emplace<game::CNavAgent>(e);
      if (ai.movingToAnchorSinceMs == 0) ai.movingToAnchorSinceMs = nowMs;

      if (!nav.arrived) {
                // この状態の精霊はモンスターのターゲットにならないので、止まると永久に無敵になる。
                // 実際の経路が本当に長い可能性もあるので強制解除はせず、障害がログで見えることだけを保証する。
                // 引き算ではなく足し算で比較する。何らかの理由で nowMs が巻き戻っても、アンダーフローで巨大な値にならないようにするため。
        if (!ai.stuckWarned &&
            nowMs > ai.movingToAnchorSinceMs + kMovingToAnchorStuckMs) {
          ai.stuckWarned = true;
          zone_log::warning(
              "player ship stuck in MovingToAnchor: unit=%u area=%u "
              "pos=(%.0f,%.0f) held %llums; it is untargetable while moving",
              base.unitId, assign.areaId, base.x, base.y,
              static_cast<unsigned long long>(nowMs - ai.movingToAnchorSinceMs));
        }
        stop(base, vel);
        break;
      }

      member.areaId = assign.areaId;
      ai.state = PlayerShipAIState::Patrolling;
      ai.hasWanderTarget = false;
      ai.wanderWaitTimer = 0.f;
      stop(base, vel);
      break;
    }

    case PlayerShipAIState::Patrolling: {
            // ① モンスターのスキャン：そのエリアで**自分に最も近い**生存モンスターを取る。
            //    いれば Chasing に移り、速度は書かずにそのまま break する。
      if (nowMs >= ai.nextRetargetAtMs) {
        ai.nextRetargetAtMs = nowMs + kRetargetIntervalMs;
        entt::entity cand = area_registry_system::pickNearestAliveMonster(
            world, member.areaId, base.x, base.y);
        if (cand != entt::null) {
          auto &nav = world.get_or_emplace<game::CNavAgent>(e);
          const auto &tb = world.get<UnitBase>(cand);
          ai.currentTarget = cand;
          ai.state = PlayerShipAIState::Chasing;
          nav.active = true;
          nav.goalCell = game::navWorldToCell(grid, tb.x, tb.y);
          nav.pathDirty = true;
          nav.arrived = false;
          stop(base, vel);
          break;
        }
      }

      auto &nav = world.get_or_emplace<game::CNavAgent>(e);
      if (!ai.hasWanderTarget || !nav.active || nav.arrived) {
        const game::NavCell patrolCell =
            game::pickRandomWalkableCellInArea(grid, member.areaId,
                                               areaPickScratch(world));
        const Position patrolPos = game::navCellCenterToWorld(grid, patrolCell);
        ai.wanderTargetX = patrolPos.x;
        ai.wanderTargetY = patrolPos.y;
        nav.active = true;
        nav.goalCell = patrolCell;
        nav.pathDirty = true;
        nav.arrived = false;
        ai.hasWanderTarget = true;
      }
      stop(base, vel);
      break;
    }

    case PlayerShipAIState::Chasing: {
      if (!city_combat::isTargetable(world, ai.currentTarget)) {
        ai.currentTarget = entt::null;
        ai.state = PlayerShipAIState::Patrolling;
        auto &nav = world.get_or_emplace<game::CNavAgent>(e);
        clearNav(world, nav);
        stop(base, vel);
        break;
      }
      const auto &tb = world.get<UnitBase>(ai.currentTarget);
      const float dist = city_combat::distance(base, tb);
      if (dist <= combat.attack_range) {
        auto &nav = world.get_or_emplace<game::CNavAgent>(e);
        clearNav(world, nav);
        ai.state = PlayerShipAIState::Attacking;
        stop(base, vel);
        break;
      }
      auto &nav = world.get_or_emplace<game::CNavAgent>(e);
      const auto targetCell = game::navWorldToCell(grid, tb.x, tb.y);
      if (!nav.active || nav.goalCell.x != targetCell.x ||
          nav.goalCell.y != targetCell.y) {
        nav.active = true;
        nav.goalCell = targetCell;
        nav.pathDirty = true;
        nav.arrived = false;
      }
      stop(base, vel);
      break;
    }

    case PlayerShipAIState::Attacking: {
      if (!city_combat::isTargetable(world, ai.currentTarget)) {
        ai.currentTarget = entt::null;
        ai.state = PlayerShipAIState::Patrolling;
        auto &nav = world.get_or_emplace<game::CNavAgent>(e);
        clearNav(world, nav);
        stop(base, vel);
        break;
      }
      const auto &tb = world.get<UnitBase>(ai.currentTarget);
      const float dist = city_combat::distance(base, tb);
      combat.cooldown_remain_sec =
          std::max(0.f, combat.cooldown_remain_sec - dtSec);

      if (dist > combat.attack_range) {
        ai.state = PlayerShipAIState::Chasing;
        break;
      }
      stop(base, vel);
      if (combat.cooldown_remain_sec > 0.f) break;

      combat.cooldown_remain_sec = combat.attack_cooldown;
      base.actionState = static_cast<uint8_t>(Game::ActionState_Attack);

      ActionStartedEvent ase;
      ase.entity = e;
      ase.action_type = static_cast<uint8_t>(Game::ActionState_Attack);
      ase.duration = combat.attack_cooldown;
      ase.target_x = tb.x;
      ase.target_y = tb.y;
      hub.action_started.curr.push_back(ase);

      city_combat::applyDamage(world, e, ai.currentTarget,
                               combat.attack_power, hub, mapId);
      break;
    }

    case PlayerShipAIState::Retreating: {
            // 1 秒間その場にとどまり（死亡時の静止演出）、その後 nav の目標を病院の近くに切り替える。到着したら Healing に入る。
      if (nowMs - ai.deathStayStartedAtMs < kDeathStayMs) {
        hold(base, vel, static_cast<uint8_t>(Game::ActionState_Retreating));
        break;
      }

      auto &nav = world.get_or_emplace<game::CNavAgent>(e);
            // 出現地点ではなく病院へ戻る。回復は病院が担当しており、病院の座標は DB から取得し、将来はプレイヤーが動かせるようになる。
      const auto healCell = healingApproachCell(world, grid);
      if (nav.goalCell.x != healCell.x || nav.goalCell.y != healCell.y) {
        nav.active = true;
        nav.goalCell = healCell;
        nav.pathDirty = true;
        nav.arrived = false;
        hold(base, vel, static_cast<uint8_t>(Game::ActionState_Retreating));
        break;
      }

      if (!nav.arrived) {
        hold(base, vel, static_cast<uint8_t>(Game::ActionState_Retreating));
        break;
      }

      ai.state = PlayerShipAIState::Healing;
      stop(base, vel);
      base.actionState = static_cast<uint8_t>(Game::ActionState_Idle);
      break;
    }

    case PlayerShipAIState::Healing: {
      stop(base, vel);
            // 上と同じく、HP の正となる値は UnitBase に持つ。healing_station_system は
            // base.curHp と combat.cur_hp を一緒に満タンまで同期する。
      if (base.curHp >= base.maxHp) {
        auto &nav = world.get_or_emplace<game::CNavAgent>(e);
        const auto anchorCell = grid.areaAnchorCells[assign.areaId];
        nav.active = true;
        nav.goalCell = anchorCell;
        nav.pathDirty = true;
        nav.arrived = false;
        base.lifeStatus  = static_cast<uint8_t>(Game::LifeStatus_Alive);
        base.actionState = static_cast<uint8_t>(Game::ActionState_Idle);
        ai.state = PlayerShipAIState::MovingToAnchor;

        ReviveEvent rv;
        rv.entity = e;
        rv.map_id = mapId;
        hub.revive.curr.push_back(rv);
      }
      break;
    }
    } // switch
  }
}

/**
 * @brief 精霊 1 体のエリア割り当て
 *
 * @note
 * - assignShipsToArea と同じ状態遷移を、精霊 1 体に対して行う。
 */
bool assignShipToAreaByUnitId(entt::registry &world, MonsterAIEventHub &hub,
                              const std::string &roleId, uint32_t unitId,
                              uint32_t areaId, uint64_t nowMs, uint32_t mapId) {
  if (areaId < 1 || areaId > 4) return false;
  const auto &grid = game::navGridOf(world);
  const auto anchorCell = grid.areaAnchorCells[areaId];
  const Position anchor = game::navCellCenterToWorld(grid, anchorCell);

  auto view = world.view<CPlayerShipAI, CPlayerShipAssignment, CAreaMember,
                         Owner, UnitBase, Velocity>();
  for (auto e : view) {
    if (view.get<Owner>(e).roleId != roleId) continue;
    auto &base = view.get<UnitBase>(e);
    if (base.unitId != unitId) continue;

    auto &ai     = view.get<CPlayerShipAI>(e);
    auto &assign = view.get<CPlayerShipAssignment>(e);
    auto &vel    = view.get<Velocity>(e);

    assign.areaId       = areaId;
    assign.anchorX      = anchor.x;
    assign.anchorY      = anchor.y;
    assign.assignedAtMs = nowMs;

        // assignShipsToArea と同じ：回復中の精霊は割り当て先エリアだけを更新し、ステートマシンは中断しない。
    if (ai.state == PlayerShipAIState::Retreating ||
        ai.state == PlayerShipAIState::Healing) {
      return true;
    }

    ai.state            = PlayerShipAIState::MovingToAnchor;
    ai.currentTarget    = entt::null;
    ai.nextRetargetAtMs = 0;
    ai.hasWanderTarget  = false;
    ai.wanderWaitTimer  = 0.f;

    auto &nav = world.get_or_emplace<game::CNavAgent>(e);
    clearNav(world, nav);
    nav.active = true;
    nav.goalCell = anchorCell;
    nav.pathDirty = true;
    nav.arrived = false;
    stop(base, vel);

    AggroClearRequestEvent ag;
    ag.victim = e;
    ag.map_id = mapId;
    hub.aggro_clear.curr.push_back(ag);
    return true;
  }
  return false;
}

void assignShipsToArea(entt::registry &world, MonsterAIEventHub &hub,
                       const std::string &roleId, uint32_t areaId,
                       uint64_t nowMs, uint32_t mapId,
                       const std::vector<uint32_t> &deployedUnitIds) {
  if (areaId < 1 || areaId > 4) return;
  const auto &grid = game::navGridOf(world);

  const auto anchorOf = [&grid](uint32_t area) {
    return game::navCellCenterToWorld(grid, grid.areaAnchorCells[area]);
  };

  auto view = world.view<CPlayerShipAI, CPlayerShipAssignment, CAreaMember,
                         Owner, UnitBase, Velocity>();
  for (auto e : view) {
    const auto &owner = view.get<Owner>(e);
    if (owner.roleId != roleId) continue;

    auto &ai     = view.get<CPlayerShipAI>(e);
    auto &assign = view.get<CPlayerShipAssignment>(e);
    auto &base   = view.get<UnitBase>(e);
    auto &vel    = view.get<Velocity>(e);

        // マップ上にはそのキャラクターの全精霊がいるので、待機中の精霊まで一緒に出撃させてはならない。
        // 現在の出撃編成の精霊だけを割り当て先エリアへ向かわせ、それ以外はすべてメインシティへ戻す。
    const bool deployed =
        std::find(deployedUnitIds.begin(), deployedUnitIds.end(),
                  base.unitId) != deployedUnitIds.end();
    const uint32_t targetArea =
        deployed ? areaId : player_ship_constants::kDefaultAreaId;
    const auto anchorCell = grid.areaAnchorCells[targetArea];
    const Position anchor = anchorOf(targetArea);

    assign.areaId       = targetArea;
    assign.anchorX      = anchor.x;
    assign.anchorY      = anchor.y;
    assign.assignedAtMs = nowMs;
        // 経験値は編成単位で配るので、出撃状態はここでコンポーネントに反映しておく必要がある。
        // 下の Retreating/Healing の continue より前に書くこと。
        // 回復中の精霊はステートマシンこそ中断しないが、編成への所属は即座に反映させる。
    assign.deployed     = deployed;

        // 瀕死で撤退中・治療中の精霊は中断できない。まずメインシティへ戻って HP を満タンにするのが仕事だから。
        // 割り当て先エリアだけを更新する。Healing が終わると assign.areaId を読んで行き先を決めるので、
        // 新しい編成への所属は、治療が終わってから有効になる形で反映される。
        //
        // ここでいきなり MovingToAnchor に変えると、回復の流れから外れてしまう。AI 冒頭の自己回復判定の
        // 条件は curHp == 0 だが、すでに一部回復した精霊は curHp > 0 なので判定が働かず、
        // Healing にも戻れず病院にも行かず、その場で止まってしまう。
    if (ai.state == PlayerShipAIState::Retreating ||
        ai.state == PlayerShipAIState::Healing) {
      continue;
    }

    ai.state            = PlayerShipAIState::MovingToAnchor;
    ai.currentTarget    = entt::null;
    ai.nextRetargetAtMs = 0;
    ai.hasWanderTarget  = false;
    ai.wanderWaitTimer  = 0.f;

    auto &nav = world.get_or_emplace<game::CNavAgent>(e);
    clearNav(world, nav);
    nav.active = true;
    nav.goalCell = anchorCell;
    nav.pathDirty = true;
    nav.arrived = false;
    stop(base, vel);

    AggroClearRequestEvent ag;
    ag.victim = e;
    ag.map_id = mapId;
    hub.aggro_clear.curr.push_back(ag);
  }
}

void consumeKills(entt::registry &world, MonsterAIEventHub &hub,
                  const cache::TemplateCache &tc, uint32_t mapId,
                  uint32_t maxLevel) {
  if (hub.unit_killed.prev.empty()) return;

  auto teamView = world.view<CExperience, CPlayerShipAssignment, Owner,
                             UnitBase, UnitCombat>();

  for (const auto &ev : hub.unit_killed.prev) {
        // 撃破者は「この経験値がどのキャラクターの編成に入るか」を決めるためだけに使い、撃破者自身が独占するわけではない。
        // 撃破者が同じフレームで返り討ちに遭ったり、編成の切り替えで破棄されたりした場合、その経験値はすべて無効になる。
        // 撃破者がいなければ、所属するキャラクターを特定できないため。
    if (!world.valid(ev.killer)) continue;
    if (!world.all_of<Owner>(ev.killer)) continue;
    const std::string &killerRole = world.get<Owner>(ev.killer).roleId;

        // 基礎経験値は当面、モンスターテンプレートの gold_reward を使う。
        // TODO: unit_templates に専用の exp_reward 列を追加したら、そちらを読むように変える。
        //       ゴールドと経験値が同じ設定値を使い続けるべきではない。
    uint64_t baseXp = 0;
    if (const auto tplIt = tc.units.find(ev.victimUnitId);
        tplIt != tc.units.end()) {
      baseXp = tplIt->second.goldReward;
    }
    const uint32_t monsterLevel = std::max<uint32_t>(1, ev.victimLevel);

        // 同じ経験値を編成内の精霊全員に配る（**総量の山分けではない**）。
        // 各精霊は自分とモンスターのレベル差に応じて個別に減衰するので、結果はそれぞれ異なることがある。
    for (auto e : teamView) {
      if (teamView.get<Owner>(e).roleId != killerRole) continue;
      if (!teamView.get<CPlayerShipAssignment>(e).deployed) continue;

      auto &exp   = teamView.get<CExperience>(e);
      auto &base  = teamView.get<UnitBase>(e);
      auto &comb  = teamView.get<UnitCombat>(e);

      const uint64_t gain =
          city_combat::xpRewardForKill(baseXp, exp.level, monsterLevel);
      city_combat::applyExpAndLevelUp(exp, base, comb, gain, hub, e, mapId,
                                      maxLevel);
    }
  }
  hub.unit_killed.prev.clear();
}

} // namespace player_ship_ai_system
