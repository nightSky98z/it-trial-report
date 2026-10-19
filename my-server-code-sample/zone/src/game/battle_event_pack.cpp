#include "game/battle_event_pack.h"

#include "ecs/map_components.h"

#include <cstddef>

namespace game {
namespace {

uint32_t networkIdOf(entt::registry &world, entt::entity entity) {
  if (!world.valid(entity) || !world.all_of<NetworkId>(entity)) {
    return 0;
  }
  return world.get<NetworkId>(entity).value;
}

bool isVisible(const std::unordered_set<uint32_t> *visibleNetworkIds,
               uint32_t networkId) {
  if (!visibleNetworkIds) return true;
  if (networkId == 0) return false;
  return visibleNetworkIds->find(networkId) != visibleNetworkIds->end();
}

bool eitherVisible(const std::unordered_set<uint32_t> *visibleNetworkIds,
                   uint32_t lhs, uint32_t rhs) {
  if (!visibleNetworkIds) return true;
  return isVisible(visibleNetworkIds, lhs) || isVisible(visibleNetworkIds, rhs);
}

/**
 * @brief 今回パックする戦闘イベントの件数を数える
 *
 * @param hub prev のイベントバッファを借用するだけで、参照は保持しない。
 *
 * @return prev の全イベントキューの要素数の合計。NetworkId を持たないイベントは後でスキップされる。
 */
std::size_t pendingEventCount(const MonsterAIEventHub &hub) {
  return hub.state_changed.prev.size() +
         hub.action_started.prev.size() +
         hub.action_finished.prev.size() +
         hub.skill_cast.prev.size() +
         hub.combat_result.prev.size() +
         hub.hp_changed.prev.size() +
         hub.death.prev.size() +
         hub.revive.prev.size() +
         hub.buff_changed.prev.size() +
         hub.phase_changed.prev.size() +
         hub.level_up.prev.size() +
         hub.stats_changed.prev.size();
}

} // namespace

void appendBattleEventItems(entt::registry &world,
                            const MonsterAIEventHub &hub,
                            flatbuffers::FlatBufferBuilder &fbb,
                            uint64_t serverTimeMs,
                            const std::unordered_set<uint32_t> *visibleNetworkIds,
                            codec::BattleEventBatchSoA &out) {
  out.reserve(out.size() + pendingEventCount(hub));

  for (const auto &ev : hub.state_changed.prev) {
    const uint32_t id = networkIdOf(world, ev.monster);
    if (id == 0) continue;
    if (!isVisible(visibleNetworkIds, id)) continue;
    out.push(Game::BattleEvent_S2C_StateChanged,
        codec::buildStateChanged(fbb, serverTimeMs, id,
            static_cast<Game::ActionState>(ev.prev_state),
            static_cast<Game::ActionState>(ev.new_state)).Union());
  }

  for (const auto &ev : hub.action_started.prev) {
    const uint32_t id = networkIdOf(world, ev.entity);
    if (id == 0) continue;
    if (!isVisible(visibleNetworkIds, id)) continue;
    out.push(Game::BattleEvent_S2C_ActionStarted,
        codec::buildActionStarted(fbb, serverTimeMs, id, ev.action_type,
            ev.duration, ev.target_x, ev.target_y, ev.skill_id).Union());
  }

  for (const auto &ev : hub.action_finished.prev) {
    const uint32_t id = networkIdOf(world, ev.entity);
    if (id == 0) continue;
    if (!isVisible(visibleNetworkIds, id)) continue;
    out.push(Game::BattleEvent_S2C_ActionFinished,
        codec::buildActionFinished(fbb, serverTimeMs, id,
            ev.action_type).Union());
  }

  for (const auto &ev : hub.skill_cast.prev) {
    const uint32_t id = networkIdOf(world, ev.caster);
    if (id == 0) continue;
    if (!isVisible(visibleNetworkIds, id)) continue;
    out.push(Game::BattleEvent_S2C_SkillCast,
        codec::buildSkillCast(fbb, serverTimeMs, id, ev.skill_id,
            ev.origin_x, ev.origin_y, ev.target_x, ev.target_y).Union());
  }

  for (const auto &ev : hub.combat_result.prev) {
    const uint32_t srcId = networkIdOf(world, ev.source);
    const uint32_t tgtId = networkIdOf(world, ev.target);
    if (tgtId == 0) continue;
    if (!eitherVisible(visibleNetworkIds, srcId, tgtId)) continue;
    out.push(Game::BattleEvent_S2C_CombatResult,
        codec::buildCombatResult(fbb, serverTimeMs, srcId, tgtId,
            ev.damage, ev.delta, ev.final_hp, ev.max_hp, ev.is_crit,
            ev.is_miss, ev.skill_id, ev.hit_x, ev.hit_y).Union());
  }

  for (const auto &ev : hub.hp_changed.prev) {
    const uint32_t id = networkIdOf(world, ev.entity);
    if (id == 0) continue;
    if (!isVisible(visibleNetworkIds, id)) continue;
    out.push(Game::BattleEvent_S2C_HpChanged,
        codec::buildHpChanged(fbb, serverTimeMs, id, ev.cur_hp, ev.max_hp,
            ev.delta).Union());
  }

  for (const auto &ev : hub.death.prev) {
    const uint32_t id = networkIdOf(world, ev.victim);
    const uint32_t killerId = networkIdOf(world, ev.killer);
    if (id == 0) continue;
    if (!isVisible(visibleNetworkIds, id)) continue;
    out.push(Game::BattleEvent_S2C_Death,
        codec::buildDeath(fbb, serverTimeMs, id, killerId).Union());
  }

  for (const auto &ev : hub.revive.prev) {
    const uint32_t id = networkIdOf(world, ev.entity);
    if (id == 0) continue;
    if (!isVisible(visibleNetworkIds, id)) continue;
    out.push(Game::BattleEvent_S2C_Revive,
        codec::buildRevive(fbb, serverTimeMs, id).Union());
  }

  for (const auto &ev : hub.buff_changed.prev) {
    const uint32_t id = networkIdOf(world, ev.entity);
    if (id == 0) continue;
    if (!isVisible(visibleNetworkIds, id)) continue;
    out.push(Game::BattleEvent_S2C_BuffEvent,
        codec::buildBuffEvent(fbb, serverTimeMs, id, ev.buff_id,
            static_cast<Game::BuffOp>(ev.op), ev.stacks,
            ev.remain_sec).Union());
  }

  for (const auto &ev : hub.phase_changed.prev) {
    const uint32_t id = networkIdOf(world, ev.monster);
    if (id == 0) continue;
    if (!isVisible(visibleNetworkIds, id)) continue;
    out.push(Game::BattleEvent_S2C_PhaseChanged,
        codec::buildPhaseChanged(fbb, serverTimeMs, id, ev.prev_phase,
            ev.new_phase).Union());
  }

  for (const auto &ev : hub.level_up.prev) {
    const uint32_t id = networkIdOf(world, ev.unit);
    if (id == 0) continue;
    if (!isVisible(visibleNetworkIds, id)) continue;
    out.push(Game::BattleEvent_S2C_LevelUp,
        codec::buildLevelUp(fbb, serverTimeMs, id, ev.newLevel).Union());
  }

  // 派生ステータスの変化：値はイベントからではなく world から読み直す。
  // 同じ tick 内で 2 レベル上がることもあるため、キューに積んだ時点の古い値ではなく
  // 送信時点の最新値を送る。
  for (const auto &ev : hub.stats_changed.prev) {
    const uint32_t id = networkIdOf(world, ev.unit);
    if (id == 0) continue;
    if (!isVisible(visibleNetworkIds, id)) continue;
    if (!world.valid(ev.unit)) continue;
    const auto *base = world.try_get<UnitBase>(ev.unit);
    const auto *comb = world.try_get<UnitCombat>(ev.unit);
    if (!base || !comb) continue;
    out.push(Game::BattleEvent_S2C_UnitStatsChanged,
        codec::buildUnitStatsChanged(
            fbb, serverTimeMs, id, std::max<uint32_t>(1, base->level),
            static_cast<uint32_t>(std::max(0, comb->max_hp)),
            static_cast<uint32_t>(std::max(0, comb->attack_power))).Union());
  }
}

} // namespace game
