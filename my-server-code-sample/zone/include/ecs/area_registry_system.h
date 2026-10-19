#pragma once
#include "ecs/map_components.h"
#include "ecs/player_ship_components.h"
#include <array>
#include <cstddef>
#include <entt/entt.hpp>
#include <vector>

/**
 * @brief 生存しているモンスターをエリア別にキャッシュする
 *
 * @note
 * - world.ctx() が所有する。world をまたいで共有しないこと。
 * - byArea[0] は未使用。業務上のエリア ID は 1 から始まる。
 * - HINT: MapMode ごとに独立したシステムテーブルを持つようになったら、この world.ctx()
 *   シングルトンを map_type_scheduler の「PvE メインシティ専用システムテーブル」へ移す。
 *   その代わり、インスタンスの初期化と tick の入口の両方で MapMode による分岐が必要になる。
 */
struct AreaMonsterRegistry {
  static constexpr std::size_t kAreaCount = 5; // index 0 unused
  std::array<std::vector<entt::entity>, kAreaCount> byArea;
};

namespace area_registry_system {

/**
 * @brief エリア別モンスター索引を ctx に初期化する
 *
 * @param world マップの ECS world。AreaMonsterRegistry がなければ emplace する。
 *
 * @note
 * - MapInstance の作成後に 1 回呼ぶ。複数回呼んでも既存の ctx の状態は保持される。
 */
void initialize(entt::registry &world);

/**
 * @brief エリア別モンスター索引を再構築する
 *
 * @param world マップの ECS world。AreaMonsterRegistry をクリアして詰め直す。
 *
 * @note
 * - 毎フレーム、プレイヤー船 AI が攻撃対象を選ぶ前に呼ぶ。
 * - 索引に入るのは CAreaMember と UnitBase を持ち、まだ生きているモンスターだけ。
 */
void update(entt::registry &world);

/**
 * @brief 指定エリア内で、指定座標に最も近い生存モンスターを返す
 *
 * @param world マップの ECS world。AreaMonsterRegistry を読み取るだけ。
 * @param areaId 業務上のエリア ID（1 始まり）。
 * @param fromX 基準点のワールド座標 X。通常は索敵する精霊の位置。
 * @param fromY 基準点のワールド座標 Y。
 *
 * @return 見つかったモンスターのエンティティ。
 * @retval entt::null areaId が無効、索引がない、またはエリアに生存モンスターがいない。
 *
 * @note
 * - 「最初の 1 体」ではなく「最も近い 1 体」を選ぶ。登録順で選ぶと、精霊が目の前の敵を
 *   素通りして遠くの敵へ向かい、途中で攻撃を受けてしまう（「目の前の敵を殴らない」ように見える）。
 * - 索敵距離に上限は設けない。精霊が自分の担当エリアを歩き回って敵を探すのは、
 *   放置バトルとして想定どおりの挙動。距離の制限はモンスター側（UnitSensor）にだけあり、
 *   「どちらが先に相手を見つけるか」を決めている。
 * - 判定はターゲット取得時の 1 回だけ。Chasing 中は選び直さないので、途中でターゲットが
 *   コロコロ変わって行ったり来たりすることはない。現在のターゲットが死亡・無効化したときに
 *   改めて最寄りの 1 体を選ぶ。
 */
entt::entity pickNearestAliveMonster(entt::registry &world, uint32_t areaId,
                                     float fromX, float fromY);

} // namespace area_registry_system
