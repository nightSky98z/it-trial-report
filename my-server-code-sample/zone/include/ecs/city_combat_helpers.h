#pragma once
#include "ecs/events.h"
#include "ecs/map_components.h"
#include "ecs/nav_components.h"
#include "ecs/player_ship_components.h"
#include "game/player_ship_constants.h"
#include "generated/game_generated.h"
#include "utils/time_utils.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <entt/entt.hpp>

namespace city_combat {

/**
 * @brief エンティティのナビゲーション経路と移動速度をクリアする
 *
 * @param world マップの ECS world。対象エンティティの CNavAgent/Velocity を変更する。
 * @param e 対象エンティティ。該当するコンポーネントがなければスキップする。
 *
 * @note
 * - 死亡・瀕死・攻撃時に経路を凍結する場合に使う。呼び出し後、古い nav 経路は NavPathStore に返却済み。
 */
inline void clearNavMotion(entt::registry &world, entt::entity e) {
  if (auto *nav = world.try_get<game::CNavAgent>(e)) {
    nav->active = false;
    nav->pathDirty = false;
    nav->arrived = true;
    nav->nextIndex = 0;
    game::navClearPath(world, *nav);
  }

  if (auto *vel = world.try_get<Velocity>(e)) {
    vel->vx = 0.f;
    vel->vy = 0.f;
  }
}

/**
 * @brief 2 つのユニット間のワールド距離を返す
 *
 * @param a 借用するユニット A。
 * @param b 借用するユニット B。
 *
 * @return 2 次元のユークリッド距離。
 */
inline float distance(const UnitBase &a, const UnitBase &b) {
  const float dx = a.x - b.x;
  const float dy = a.y - b.y;
  return std::sqrt(dx * dx + dy * dy);
}

/**
 * @brief ユニットが Alive かどうかを判定する
 *
 * @param b 借用するユニットの基本コンポーネント。
 *
 * @retval true lifeStatus がプロトコルの Alive と等しい。
 */
inline bool isAlive(const UnitBase &b) {
  return b.lifeStatus == static_cast<uint8_t>(Game::LifeStatus_Alive);
}

/**
 * @brief ユニットが Dying かどうかを判定する
 *
 * @param b 借用するユニットの基本コンポーネント。
 *
 * @retval true lifeStatus がプロトコルの Dying と等しい。
 */
inline bool isDying(const UnitBase &b) {
  return b.lifeStatus == static_cast<uint8_t>(Game::LifeStatus_Dying);
}

/**
 * @brief エンティティがメインシティでの攻撃対象になれるかを判定する
 *
 * @param world マップの ECS world。対象の UnitBase を読むだけ。
 * @param e 候補のエンティティ。
 *
 * @retval true エンティティが有効で、UnitBase を持ち、Alive である。
 * @retval false エンティティが無効、コンポーネントがない、または Dying/Dead。
 */
inline bool isTargetable(entt::registry &world, entt::entity e) {
  if (!world.valid(e)) return false;
  if (!world.all_of<UnitBase>(e)) return false;
  const auto &b = world.get<UnitBase>(e);
  return isAlive(b);
}

/**
 * @brief 1 回分の戦闘ダメージを適用し、戦闘イベントを書き込む
 *
 * @param world マップの ECS world。target の UnitBase・nav・velocity を変更する。
 * @param source 攻撃者のエンティティ。イベントには値として保存する。
 * @param target 被弾したエンティティ。有効で、UnitBase を持っていること。
 * @param damage 元のダメージ値。0 未満なら 0 として HP を減らす。
 * @param hub イベントハブ。combat_result.curr に書き込み、撃破時は death/unit_killed/aggro_clear にも書き込む。
 * @param mapId 現在のマップ ID（イベントのペイロードに使う）。
 * @param skillId スキル ID。255 は通常攻撃またはスキルなしを表す。
 * @param isCrit クリティカルかどうか。
 * @param isMiss 攻撃が外れたかどうか。
 *
 * @retval true target が今回 Alive から Dying または Dead に変わった。
 * @retval false target が無効、Alive ではない、または HP を減らした後も生きている。
 *
 * @note
 * - プレイヤー船は HP が 0 になると Dying に入り、撤退と治療は player_ship_ai_system が引き継ぐ。
 * - モンスターは HP が 0 になると Dead に入り、その後の破棄とリスポーンはモンスターのライフサイクルシステムが行う。
 */
inline bool applyDamage(entt::registry &world, entt::entity source,
                        entt::entity target, int damage,
                        MonsterAIEventHub &hub, uint32_t mapId,
                        uint8_t skillId = 255,
                        bool isCrit = false,
                        bool isMiss = false) {
  if (!world.valid(target) || !world.all_of<UnitBase>(target)) return false;
  auto &tb = world.get<UnitBase>(target);
  if (!isAlive(tb)) return false;

  const int newHp =
      std::max(0, static_cast<int>(tb.curHp) - std::max(0, damage));
  const int delta = newHp - static_cast<int>(tb.curHp);
  tb.curHp = static_cast<uint32_t>(newHp);

  CombatResultEvent ev;
  ev.source = source;
  ev.target = target;
  ev.map_id = mapId;
  ev.damage = damage;
  ev.delta = delta;
  ev.final_hp = tb.curHp;
  ev.max_hp = tb.maxHp;
  ev.is_crit = isCrit;
  ev.is_miss = isMiss;
  ev.skill_id = skillId;
  ev.hit_x = tb.x;
  ev.hit_y = tb.y;
  hub.combat_result.curr.push_back(ev);

  if (tb.curHp > 0) return false;

    // HP が 0 になったときの分岐
  clearNavMotion(world, target);
  if (tb.unitKind == static_cast<uint8_t>(Game::UnitKind_Monster)) {
    tb.lifeStatus  = static_cast<uint8_t>(Game::LifeStatus_Dead);
    tb.actionState = static_cast<uint8_t>(Game::ActionState_Dead);

    DeathEvent de;
    de.victim = target;
    de.killer = source;
    de.map_id = mapId;
    hub.death.curr.push_back(de);

        // UnitKilledEvent を発行する（プレイヤー船 AI が次のフレームで処理し、経験値を与える）
    UnitKilledEvent ke;
    ke.killer = source;
    ke.victim = target;
    ke.victimUnitId = tb.unitId;
    ke.victimLevel = std::max<uint32_t>(1, tb.level);
    ke.map_id = mapId;
    ke.death_x = tb.x;
    ke.death_y = tb.y;
    hub.unit_killed.curr.push_back(ke);
    return true;
  }

    // プレイヤー船 → 瀕死
  tb.lifeStatus  = static_cast<uint8_t>(Game::LifeStatus_Dying);
  tb.actionState = static_cast<uint8_t>(Game::ActionState_Retreating);

    // 瀕死になった瞬間に、このユニットに向けられたヘイトの解除を要求する
  AggroClearRequestEvent ag;
  ag.victim = target;
  ag.map_id = mapId;
  hub.aggro_clear.curr.push_back(ag);
  return true;
}

/**
 * @brief 指定したレベルに上がるのに必要な経験値を返す
 *
 * @param level 目標レベル。1 は基礎レベル。
 *
 * @return 現在の経験値曲線における、そのレベルの必要量。
 */
inline uint64_t xpRequiredForLevel(uint32_t level) {
  using namespace player_ship_constants;
  return static_cast<uint64_t>(
      static_cast<double>(kBaseXpPerLevel) *
      std::pow(static_cast<double>(kXpGrowthRate),
               static_cast<double>(level - 1)));
}

/**
 * @brief 精霊とモンスターのレベル差に応じて、1 回の撃破で得る経験値を減衰させる
 *
 * @param baseXp そのモンスターの基礎経験値（現在は unit_templates.gold_reward を使う）。
 * @param unitLevel 経験値を受け取る精霊の現在のレベル。
 * @param monsterLevel 倒されたモンスターのレベル。1 以上であることは呼び出し側が保証する。
 *
 * @return その精霊が今回実際に得る経験値。**常に kMinKillXp（1）以上で、0 や負になることはない**。
 *
 * @note
 * - 減衰は精霊のレベルがモンスターより高いときだけ起こり、1 レベルごとに kXpKeepPctPerLevel/100 を掛ける。
 *   モンスターのレベルが精霊以上なら baseXp を満額返す（ボーナスはない）。
 * - **負の経験値の防止**：レベル差は飽和減算（足りなければ 0）で求め、計算はすべて uint64_t で行う。
 *   掛け算と正の整数での割り算だけを行い、最後に下限へ丸める。どんな入力の組み合わせでもアンダーフローしない。
 *   baseXp が 0（テンプレートに gold_reward がない）でも 1 を返すので、経験値 0 にはならない。
 * - 整数演算だけを使い、浮動小数点は持ち込まない。数値の計算結果はすべてのプラットフォームで完全に一致しなければならない。
 */
inline uint64_t xpRewardForKill(uint64_t baseXp, uint32_t unitLevel,
                                uint32_t monsterLevel) {
  using namespace player_ship_constants;

    // 飽和減算：精霊のレベルがモンスター以下ならレベル差は 0。アンダーフローで天文学的な値になることは絶対にない。
  const uint32_t levelGap =
      unitLevel > monsterLevel ? unitLevel - monsterLevel : 0u;

  uint64_t xp = baseXp;
    // 1 レベルずつ減衰させる。xp は単調に減少し、下限に達したら止まるので、ループ回数は levelGap を超えない。
  for (uint32_t i = 0; i < levelGap && xp > kMinKillXp; ++i) {
    xp = xp * kXpKeepPctPerLevel / 100ULL;
  }
  return xp < kMinKillXp ? kMinKillXp : xp;
}

/**
 * @brief 経験値を加算し、レベルアップを処理する
 *
 * @param exp プレイヤー船の経験値コンポーネント。レベル・現在の経験値・dirty フラグを更新する。
 * @param base プレイヤー船の基本コンポーネント。レベルアップ時に HP を同期する。
 * @param combat プレイヤー船の戦闘コンポーネント。レベルアップ時に HP と攻撃力を同期する。
 * @param xpGain 今回得た経験値。
 * @param hub イベントハブ。レベルアップ時に level_up.curr に書き込む。
 * @param self プレイヤー船のエンティティ。
 * @param mapId 現在のマップ ID（イベントのペイロードに使う）。
 * @param maxLevel 現在のレベル上限。env.zone の MAX_UNIT_LEVEL から来ており、ホットリロードできる。
 *                 定数を読まずに明示的に受け取る。そうしないと、上限を変えた後もこの関数は古い値で頭打ちにしてしまう。
 *
 * @retval true 1 レベル以上上がった。
 * @retval false 経験値が増えただけで、レベルは上がっていない。
 *
 * @note
 * - dirtyStats は、tick の最後に player_ship_db_writer が収集してクリアする。
 */
inline bool applyExpAndLevelUp(CExperience &exp, UnitBase &base,
                               UnitCombat &combat, uint64_t xpGain,
                               MonsterAIEventHub &hub, entt::entity self,
                               uint32_t mapId, uint32_t maxLevel) {
  using namespace player_ship_constants;
  const uint32_t cap = maxLevel > 0 ? maxLevel : kDefaultMaxLevel;
  exp.currentXp += xpGain;
  bool leveled = false;
  while (exp.currentXp >= exp.nextLevelXp && exp.level < cap) {
    exp.currentXp -= exp.nextLevelXp;
    exp.level += 1;
    exp.nextLevelXp = xpRequiredForLevel(exp.level + 1);
    combat.max_hp += static_cast<int>(kHpPerLevel);
    combat.attack_power += static_cast<int>(kAtkPerLevel);
    combat.cur_hp = combat.max_hp;
    base.maxHp = static_cast<uint32_t>(combat.max_hp);
    base.curHp = static_cast<uint32_t>(combat.cur_hp);
        // レベルは UnitBase にも必ず書き戻す。プロトコルの送信（UnitStaticData.level、
        // S2C_UnitStatsChanged.level）はここを読み、DB 保存は exp.level を読む。
        // 片方だけ更新すると、「DB ではレベルが上がっているのに、画面には生成時の古いレベルが表示され続ける」ことになる。
    base.level = exp.level;
    exp.dirtyStats = true;
    leveled = true;

    LevelUpEvent le;
    le.unit = self;
    le.newLevel = exp.level;
    le.map_id = mapId;
    hub.level_up.curr.push_back(le);

        // レベルアップで max_hp/attack_power/level が変わるので、クライアントの UnitStaticData キャッシュも更新させる必要がある。
        // LevelUp はエフェクトを出すためだけのもので、数値の更新はこちらで行う。
    StatsChangedEvent sc;
    sc.unit = self;
    sc.map_id = mapId;
    hub.stats_changed.curr.push_back(sc);
  }

    // 最大レベルに達したら経験値はそれ以上溜めない。
    //
    // 丸めないと currentXp が際限なく増え続け、DB にも書き込まれ続ける。いつか上限が引き上げられたとき、
    // そうした精霊が一気に十数レベル上がり、上限に達していた間の放置時間の分がまるごと支給されてしまう。
    // 最大レベルでは「あと 1 でレベルアップ」の位置で止めておき、上限が引き上げられたら新しい曲線に沿って普通に再開する。
  if (exp.level >= cap && exp.currentXp >= exp.nextLevelXp) {
    exp.currentXp = exp.nextLevelXp > 0 ? exp.nextLevelXp - 1 : 0;
  }

    // 50% の書き込みしきい値：currentXp が nextLevelXp に対して何パーセントか
  const uint32_t pct = exp.nextLevelXp > 0
      ? static_cast<uint32_t>((exp.currentXp * 100ULL) / exp.nextLevelXp)
      : 0;
  if (leveled ||
      (pct / kExpWriteThresholdPct) !=
      (exp.lastWrittenXpPct / kExpWriteThresholdPct)) {
    exp.lastWrittenXpPct = pct;
    exp.dirtyStats = exp.dirtyStats || leveled;  // レベルアップしたなら必ず dirty
        // tick の最後に player_ship_db_writer がまとめて収集し、enqueue する
    exp.dirtyStats = true;
  }
  return leveled;
}

} // namespace city_combat
