#pragma once

#include <cstdint>
#include <entt/entt.hpp>
#include <string>
#include <vector>

/**
 * @brief ユニットがダメージを受けたイベント
 *
 * @note
 * - source/target は現在の MapInstance.world 内のエンティティハンドル。
 * - map_id は、システム間の連携やプロトコルのパック時に、マップのテンプレートやインスタンスのコンテキストを特定するために使う。
 */
struct DamageTakenEvent {
  entt::entity source = entt::null;
  entt::entity target = entt::null;
  uint32_t map_id = 0;
  int damage = 0;
  bool heavy = false;
};

/**
 * @brief ターゲットが視界に入ったことを感知したイベント
 *
 * @note
 * - monster/target は現在の world の現在のフレームでのみ有効。フレームをまたぐ場合はエンティティが生きているか確認し直すこと。
 */
struct TargetSpottedEvent {
  entt::entity monster = entt::null;
  entt::entity target = entt::null;
};

/**
 * @brief ターゲットが視界から外れたことを感知したイベント
 *
 * @note
 * - monster/target は現在の world の現在のフレームでのみ有効。
 */
struct TargetLostEvent {
  entt::entity monster = entt::null;
  entt::entity target = entt::null;
};

/**
 * @brief モンスターの状態変化イベント
 *
 * @note
 * - prev_state/new_state は、対応する AI 状態列挙の通信上の値を使う。
 */
struct StateChangedEvent {
  entt::entity monster = entt::null;
  uint8_t prev_state = 0;
  uint8_t new_state = 0;
};

/**
 * @brief アクション開始イベント
 *
 * @note
 * - action_type/skill_id はプロトコルの列挙値を使う。skill_id=255 はスキルなしを表す。
 * - target_x/target_y は、アクション開始時点のターゲットのワールド座標のスナップショット。
 */
struct ActionStartedEvent {
  entt::entity entity = entt::null;
  uint8_t action_type = 0;
  float duration = 0.f;
  float target_x = 0.f;
  float target_y = 0.f;
  uint8_t skill_id = 255;
};

/**
 * @brief アクション終了イベント
 *
 * @note
 * - entity は現在の world 内部のハンドル。エンコード前に NetworkId を解決できなければならない。
 */
struct ActionFinishedEvent {
  entt::entity entity = entt::null;
  uint8_t action_type = 0;
};

/**
 * @brief スキル発動イベント
 *
 * @note
 * - origin/target の座標は値のスナップショットで、ターゲットのエンティティがまだ生きていることを意味しない。
 */
struct SkillCastEvent {
  entt::entity caster = entt::null;
  uint8_t skill_id = 255;
  float origin_x = 0.f;
  float origin_y = 0.f;
  float target_x = 0.f;
  float target_y = 0.f;
};

/**
 * @brief 1 回分の戦闘結果イベント
 *
 * @note
 * - source/target は現在の world 内部のハンドル。
 * - delta は負の値で HP の減少を表すことがある。final_hp/max_hp は結果確定後のターゲットの HP のスナップショット。
 * - skill_id=255 は通常攻撃、またはスキルによらない攻撃を表す。
 */
struct CombatResultEvent {
  entt::entity source = entt::null;
  entt::entity target = entt::null;
  uint32_t map_id = 0;
  int damage = 0;
  int delta = 0;
  uint32_t final_hp = 0;
  uint32_t max_hp = 0;
  bool is_crit = false;
  bool is_miss = false;
  uint8_t skill_id = 255;
  float hit_x = 0.f;
  float hit_y = 0.f;
};

/**
 * @brief フェーズ切り替えイベント
 *
 * @note
 * - prev_phase/new_phase は業務上のフェーズ ID を使う。プロトコルへのパック前に連番だと仮定しないこと。
 */
struct PhaseChangedEvent {
  entt::entity monster = entt::null;
  uint8_t prev_phase = 0;
  uint8_t new_phase = 0;
};

/**
 * @brief ユニットの死亡イベント
 *
 * @note
 * - killer は entt::null のこともある。消費側は撃破者のいない死亡も扱えなければならない。
 */
struct DeathEvent {
  entt::entity victim = entt::null;
  entt::entity killer = entt::null;
  uint32_t map_id = 0;
};

/**
 * @brief 危険な投射物の警告イベント
 *
 * @note
 * - target は現在の world 内部のハンドル。AOI／プロトコル層で NetworkId を改めて確認する必要がある。
 */
struct DangerousProjectileEvent {
  entt::entity target = entt::null;
  uint32_t map_id = 0;
};

/**
 * @brief 回復イベント
 *
 * @note
 * - amount は今回の回復量。最終的な HP は HpChangeEvent または UnitBase を正とすること。
 */
struct HealEvent {
  entt::entity target = entt::null;
  uint32_t map_id = 0;
  int amount = 0;
};

/**
 * @brief HP 変化イベント
 *
 * @note
 * - cur_hp/max_hp は変化後の値のスナップショット。delta は今回の変化量を表す。
 */
struct HpChangeEvent {
  entt::entity entity = entt::null;
  uint32_t map_id = 0;
  uint32_t cur_hp = 0;
  uint32_t max_hp = 0;
  int delta = 0;
};

/**
 * @brief バフ変化イベント
 *
 * @note
 * - op/stacks/remain_sec のプロトコル上の意味は、フロントエンドと FlatBuffers のスキーマで取り決めている。
 */
struct BuffEvent {
  entt::entity entity = entt::null;
  uint32_t map_id = 0;
  uint32_t buff_id = 0;
  uint8_t op = 0;
  uint8_t stacks = 0;
  float remain_sec = 0.f;
};

/**
 * @brief ユニットの復活イベント
 *
 * @note
 * - entity は現在の world 内部のハンドル。エンコード前に NetworkId を読み直すこと。
 */
struct ReviveEvent {
  entt::entity entity = entt::null;
  uint32_t map_id = 0;
};

/**
 * @brief 特定のターゲットへのヘイトの解除を要求するイベント
 *
 * @note
 * - victim は取り除く対象。消費側はモンスターのヘイトテーブルを走査してこれを削除する。
 * - map_id で解除の範囲を限定する。
 */
struct AggroClearRequestEvent {
  entt::entity victim = entt::null;   // 全モンスターからこのターゲットへのヘイトを解除する
  uint32_t     map_id = 0;
};

/**
 * @brief モンスターがプレイヤー船に倒されたイベント
 *
 * @note
 * - killer はプレイヤー船、victim はモンスターであることを想定している。
 * - victimUnitId/victimLevel は死亡した瞬間のスナップショットで、経験値の計算に使う。
 */
struct UnitKilledEvent {
  entt::entity killer = entt::null;   // プレイヤー船
  entt::entity victim = entt::null;   // モンスター
  uint32_t     victimUnitId = 0;
  uint32_t     victimLevel = 1;
  uint32_t     map_id = 0;
    // 死亡した座標：ドロップアイテムはここに生成する。victim を引き直すのではなく、イベントに値として持たせている。
    // 処理する時点では、victim がライフサイクルシステムによって既に破棄されている可能性があるため。
  float        death_x = 0.f;
  float        death_y = 0.f;
};

/**
 * @brief ユニットのレベルアップイベント
 *
 * @note
 * - unit は現在の world 内部のハンドル。newLevel はレベルアップ後のレベル。
 */
struct LevelUpEvent {
  entt::entity unit = entt::null;
  uint32_t     newLevel = 1;
  uint32_t     map_id = 0;
};

/**
 * @brief ユニットの派生ステータスが変化した（レベルアップ／装備変更／UnitCombat を変えるあらゆる操作）
 *
 * @note
 * - イベント自体は**数値を持たない**。パック時に world から UnitBase/UnitCombat を読み直すので、
 *   送るのは必ず現在の正となる値で、イベントがキューで待っている間に古くなった数値を送ることはない。
 * - UnitCombat を変更した側が、このイベントを発行する責任を持つ。そうしないとクライアントのキャッシュが古い値のまま残る。
 */
struct StatsChangedEvent {
  entt::entity unit = entt::null;
  uint32_t     map_id = 0;
};

/**
 * @brief プレイヤー AI の状態変化イベント
 *
 * @note
 * - prev_state/new_state は、プレイヤー AI 状態列挙の通信上の値を使う。
 */
struct PlayerStateChangedEvent {
  entt::entity player = entt::null;
  uint8_t prev_state = 0;
  uint8_t new_state = 0;
};

/**
 * @brief プレイヤーのアクション開始イベント
 *
 * @note
 * - player は現在の world 内部のハンドル。target の座標はアクション開始時点のスナップショット。
 */
struct PlayerActionStartedEvent {
  entt::entity player = entt::null;
  uint8_t action_type = 0;
  float duration = 0.f;
  float target_x = 0.f;
  float target_y = 0.f;
};

/**
 * @brief プレイヤーのアクション終了イベント
 *
 * @note
 * - action_type は、プレイヤーのアクション列挙の通信上の値を使う。
 */
struct PlayerActionFinishedEvent {
  entt::entity player = entt::null;
  uint8_t action_type = 0;
};

/**
 * @brief 1 種類のイベント用のダブルバッファキュー
 *
 * @note
 * - curr にはこの tick の生成側が追記し、prev は配信・消費側のシステムが前の tick のスナップショットとして読む。
 * - tick の境界での curr -> prev のローテートは、game_loop / event_clear_system が担当する。
 * - game_session のヘルパーは、ローテート済みの prev を AOI の送信待ちバッファに追記するだけで、ローテートは行わない。
 */
template <typename T> struct TickEventBuffer {
  std::vector<T> prev;
  std::vector<T> curr;
};

/**
 * @brief 現在の tick のイベントを、前の tick のスナップショットとしてローテートする
 *
 * @param buffer 1 種類のイベント用のダブルバッファ。prev/curr を入れ替え、新しい curr をクリアする。
 *
 * @note
 * - tick の最後に呼ぶ。呼び出し後、prev には終わったばかりの tick のイベントが入り、curr は次の tick のイベントを受け付けられる。
 */
template <typename T> inline void rotateTickEventBuffer(TickEventBuffer<T> &buffer) {
  buffer.prev.swap(buffer.curr);
  buffer.curr.clear();
}

/**
 * @brief モンスター／メインシティの戦闘イベント一式
 *
 * @note
 * - MapInstance.world の ctx、またはテスト用のローカルオブジェクトとして置く。
 * - 生成側は curr にだけ書き込む。tick の最後に prev へローテートしてから、購読側や AOI／プロトコル層が読む。
 */
struct MonsterAIEventHub {
  TickEventBuffer<DamageTakenEvent> damage_taken;
  TickEventBuffer<TargetSpottedEvent> target_spotted;
  TickEventBuffer<TargetLostEvent> target_lost;
  TickEventBuffer<StateChangedEvent> state_changed;
  TickEventBuffer<ActionStartedEvent> action_started;
  TickEventBuffer<ActionFinishedEvent> action_finished;
  TickEventBuffer<SkillCastEvent> skill_cast;
  TickEventBuffer<CombatResultEvent> combat_result;
  TickEventBuffer<PhaseChangedEvent> phase_changed;
  TickEventBuffer<DeathEvent> death;
  TickEventBuffer<DangerousProjectileEvent> dangerous_projectile;
  TickEventBuffer<HealEvent> heal;
  TickEventBuffer<HpChangeEvent> hp_changed;
  TickEventBuffer<BuffEvent> buff_changed;
  TickEventBuffer<ReviveEvent> revive;
  TickEventBuffer<AggroClearRequestEvent> aggro_clear;
  TickEventBuffer<UnitKilledEvent>        unit_killed;
  TickEventBuffer<LevelUpEvent>           level_up;
  TickEventBuffer<StatsChangedEvent>      stats_changed;
};

/**
 * @brief プレイヤー AI のイベント一式
 *
 * @note
 * - MonsterAIEventHub と同様に、イベントの種類ごとに prev/curr を分けて持つ。
 */
struct PlayerAIEventHub {
  TickEventBuffer<PlayerStateChangedEvent> state_changed;
  TickEventBuffer<PlayerActionStartedEvent> action_started;
  TickEventBuffer<PlayerActionFinishedEvent> action_finished;
  TickEventBuffer<CombatResultEvent> combat_result;
  TickEventBuffer<HpChangeEvent> hp_changed;
  TickEventBuffer<BuffEvent> buff_changed;
  TickEventBuffer<ReviveEvent> revive;
};

/**
 * @brief モンスター／メインシティの戦闘イベント一式の、すべてのイベント列をローテートする
 *
 * @param hub イベント一式。各イベント列の新しい curr をクリアする。
 *
 * @note
 * - MonsterAIEventHub にフィールドを追加したら、この関数にも必ず追加すること。忘れるとその種類のイベントが curr に残り続ける。
 */
inline void rotateMonsterAIEventHub(MonsterAIEventHub &hub) {
  rotateTickEventBuffer(hub.damage_taken);
  rotateTickEventBuffer(hub.target_spotted);
  rotateTickEventBuffer(hub.target_lost);
  rotateTickEventBuffer(hub.state_changed);
  rotateTickEventBuffer(hub.action_started);
  rotateTickEventBuffer(hub.action_finished);
  rotateTickEventBuffer(hub.skill_cast);
  rotateTickEventBuffer(hub.combat_result);
  rotateTickEventBuffer(hub.phase_changed);
  rotateTickEventBuffer(hub.death);
  rotateTickEventBuffer(hub.dangerous_projectile);
  rotateTickEventBuffer(hub.heal);
  rotateTickEventBuffer(hub.hp_changed);
  rotateTickEventBuffer(hub.buff_changed);
  rotateTickEventBuffer(hub.revive);
  rotateTickEventBuffer(hub.aggro_clear);
  rotateTickEventBuffer(hub.unit_killed);
  rotateTickEventBuffer(hub.level_up);
  rotateTickEventBuffer(hub.stats_changed);
}

/**
 * @brief プレイヤー AI のイベント一式の、すべてのイベント列をローテートする
 *
 * @param hub イベント一式。各イベント列の新しい curr をクリアする。
 *
 * @note
 * - PlayerAIEventHub にフィールドを追加したら、この関数にも必ず追加すること。忘れるとその種類のイベントが curr に残り続ける。
 */
inline void rotatePlayerAIEventHub(PlayerAIEventHub &hub) {
  rotateTickEventBuffer(hub.state_changed);
  rotateTickEventBuffer(hub.action_started);
  rotateTickEventBuffer(hub.action_finished);
  rotateTickEventBuffer(hub.combat_result);
  rotateTickEventBuffer(hub.hp_changed);
  rotateTickEventBuffer(hub.buff_changed);
  rotateTickEventBuffer(hub.revive);
}
