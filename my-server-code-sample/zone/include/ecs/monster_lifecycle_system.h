#pragma once
#include "ecs/city_monster_factory.h"
#include "ecs/components.h"
#include "ecs/events.h"
#include "ecs/map_components.h"
#include "ecs/nav_components.h"
#include "game/template_cache.h"
#include "generated/game_generated.h"
#include <entt/entt.hpp>
#include <vector>

namespace monster_lifecycle_system {

/**
 * @brief モンスターの死亡後、死体エンティティを残しておく秒数
 *
 * @note
 * - クライアントが死亡アニメーションを再生し、S2C_Death を受け取るための時間。
 * - 短すぎると死亡演出が途切れ、長すぎると倒したはずのモンスターが見え続けているように感じられる。
 */
inline constexpr float kCorpseStaySec   = 1.0f;

/**
 * @brief 固定スポーン地点で、死体の破棄から再出現までの秒数
 *
 * @note
 * - CMonsterRespawnSlot がこの値でカウントダウンする。
 */
inline constexpr float kRespawnDelaySec = 5.0f;

/**
 * @brief 現在のフレームの死亡イベントを処理し、死体とリスポーンの状態を作る
 *
 * @param world マップの ECS world。CCorpseTimer と CMonsterRespawnSlot を追加する。
 * @param hub イベントハブ。death.curr を読む。
 *
 * @note
 * - death の curr が prev にローテートされる前に呼ぶこと。
 * - プレイヤー船は Dying/Retreating/Healing の流れをたどり、ここでのモンスターの死亡サイクルには入らない。
 */
inline void consumeDeaths(entt::registry &world, MonsterAIEventHub &hub) {
  for (const auto &ev : hub.death.curr) {
    if (!world.valid(ev.victim)) continue;
    auto *base = world.try_get<UnitBase>(ev.victim);
    if (!base) continue;
    if (base->unitKind != static_cast<uint8_t>(Game::UnitKind_Monster)) continue;

        // 重複防止：同じモンスターのイベントが重複して push されても（本来は起きないはず）、タイマーは 1 回だけ設定する。
    if (world.all_of<CCorpseTimer>(ev.victim)) continue;
    world.emplace<CCorpseTimer>(ev.victim, CCorpseTimer{kCorpseStaySec});

        // 速度を止める：直前の速度のまま死体が滑っていかないようにする。
    if (auto *vel = world.try_get<Velocity>(ev.victim)) {
      vel->vx = 0.f;
      vel->vy = 0.f;
    }

        // 固定スポーンではないモンスター（将来のボスやダンジョンの動的出現など）は CSpawnOrigin を持たないので、リスポーン枠は作らない。
    const auto *origin = world.try_get<CSpawnOrigin>(ev.victim);
    if (!origin) continue;

    auto slot = world.create();
    world.emplace<CMonsterRespawnSlot>(slot, CMonsterRespawnSlot{
        .spawnPointId = origin->spawnPointId,
        .unitId       = origin->unitId,
        .campId       = origin->campId,
        .remainSec    = kRespawnDelaySec,
    });
  }
}

/**
 * @brief 死体の破棄と、固定スポーン地点でのリスポーンを進める
 *
 * @param world マップの ECS world。死体や枠のエンティティを破棄し、新しいモンスターを生成することがある。
 * @param alloc networkEntityId のアロケータ。リスポーンに成功すると新しい ID を消費する。
 * @param tc テンプレートキャッシュ。読み取りのみで、参照は保持しない。
 * @param dtSec このフレームのシミュレーション秒数。
 * @param mapId 現在のマップ ID。エリア境界の検索と、新しいモンスターのイベントのペイロードに使う。
 *
 * @note
 * - consumeDeaths の後に呼ぶこと。
 * - エンティティの追加・削除といった構造的な変更は、走査対象を集め終えてから行う。entt の view の走査を壊さないため。
 */
inline void update(entt::registry &world,
                   NetworkEntityIdAllocator &alloc,
                   const cache::TemplateCache &tc,
                   float dtSec,
                   uint32_t mapId) {
    // ① 死体のカウントダウン：時間が来たらエンティティを破棄する（次回の AOI 送信で left として通知される）。
  {
    std::vector<entt::entity> toDestroy;
    auto view = world.view<CCorpseTimer>();
    for (auto e : view) {
      auto &t = view.get<CCorpseTimer>(e);
      t.remainSec -= dtSec;
      if (t.remainSec <= 0.f) toDestroy.push_back(e);
    }
    for (auto e : toDestroy) {
      game::navClearEntityPath(world, e);
      world.destroy(e);
    }
  }

    // ② スポーン枠のカウントダウン：時間が来たら新しいモンスターを生成し、枠のエンティティを破棄する。
  {
    std::vector<entt::entity> toDestroy;
    auto view = world.view<CMonsterRespawnSlot>();
    for (auto e : view) {
      auto &slot = view.get<CMonsterRespawnSlot>(e);
      slot.remainSec -= dtSec;
      if (slot.remainSec > 0.f) continue;

      city_monster_factory::spawnAtPoint(world, alloc, tc,
                                         slot.spawnPointId, slot.unitId,
                                         slot.campId, mapId);
      toDestroy.push_back(e);
    }
    for (auto e : toDestroy) world.destroy(e);
  }
}

} // namespace monster_lifecycle_system
