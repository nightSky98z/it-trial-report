#pragma once
#include "ecs/map_components.h"
#include "ecs/nav_components.h"
#include "ecs/player_ship_components.h"
#include "game/city_nav_grid.h"
#include "game/nav_grid.h"
#include "generated/game_generated.h"

namespace monster_wander_system {

/**
 * @brief 現在の world が持つ「エリア内ランダム地点選択」用のスクラッチ領域を返す
 *
 * @param world マップの ECS world。スクラッチ領域の寿命は ctx() が管理する。
 *
 * @return 使い回せる候補セル用の一時メモリプールへの参照
 *
 * @note
 * - モンスターが徘徊先を選び直すときだけ呼ぶ。
 * - monster_wander_system は world ごとに単一スレッドで動く。返した参照をスレッド間で共有しないこと。
 */
inline game::NavAreaPickScratch &areaPickScratch(entt::registry &world) {
  if (!world.ctx().contains<game::NavAreaPickScratch>()) {
    world.ctx().emplace<game::NavAreaPickScratch>();
  }
  return world.ctx().get<game::NavAreaPickScratch>();
}

// ── 毎フレーム呼ぶ：モンスターを自分の member.areaId のエリア内でランダムに徘徊させる ──
// 設計は player_ship_ai_system の Patrolling に合わせている。
//   ・目的地は必ずエリア内から選ぶ：pickRandomWalkableCellInArea(grid, member.areaId, scratch)
//   ・状態は CNavAgent（goalCell/arrived/pathDirty/active）だけで管理する
//   ・戦闘中や死亡時は制御を手放す（city_monster_combat_system や lifecycle が処理する）
//   ・毎フレームの最初に防御的なチェックを行う。現在の goalCell が member.areaId の外に
//     なっていたら（設定変更やエリアの再割り当て）、即座に「到着」扱いにしてこのフレームで選び直す。
//
// プレイヤー船はこのシステムを通らない。AiWander は削除済みで、bounds / origin / waitTimer も廃止した。
inline void update(entt::registry &world, float dtSec) {
  (void)dtSec;
  const auto &grid = game::navGridOf(world);
  auto view = world.view<UnitBase, Velocity, CAreaMember>();
  for (auto e : view) {
    auto &base = view.get<UnitBase>(e);
    auto &velocity = view.get<Velocity>(e);
    const auto &member = view.get<CAreaMember>(e);

        // 対象はモンスターだけ。プレイヤー船が CAreaMember とモンスターの unitKind を同時に持つことはないが、念のため弾く。
    if (base.unitKind != static_cast<uint8_t>(Game::UnitKind_Monster)) continue;

        // 戦闘中は city_monster_combat_system が追跡・攻撃を担当するので徘徊しない。
    if (world.all_of<UnitAggro>(e) && world.get<UnitAggro>(e).in_combat) {
      continue;
    }
        // 瀕死・死亡中は徘徊しない。
    if (base.lifeStatus != static_cast<uint8_t>(Game::LifeStatus_Alive)) {
      continue;
    }

    auto &nav = world.get_or_emplace<game::CNavAgent>(e);

        // 防御的チェック（毎フレーム）：今の目的地が自分のエリア外なら、即座に「到着」扱いにして選び直させる。
    if (nav.active &&
        game::navAreaIdAt(grid, nav.goalCell) != member.areaId) {
      nav.arrived = true;
      game::navClearPath(world, nav);
    }

    if (!nav.active || nav.arrived) {
      const game::NavCell goal =
          game::pickRandomWalkableCellInArea(grid, member.areaId,
                                             areaPickScratch(world));
      nav.active = true;
      nav.goalCell = goal;
      nav.pathDirty = true;
      nav.arrived = false;
      nav.nextIndex = 0;
      game::navClearPath(world, nav);
    }

        // 速度はいったん 0 にする。実際の velocity は nav_follow_system が経路に沿って書き込む。
    velocity.vx = 0.f;
    velocity.vy = 0.f;
    base.actionState = static_cast<uint8_t>(Game::ActionState_Idle);
  }
}

} // namespace monster_wander_system
