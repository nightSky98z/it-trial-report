#pragma once
#include <cstdint>
#include <entt/entt.hpp>

/**
 * @brief エリア戦闘のメンバーであることを示すタグ（プレイヤー船とモンスターで共用）
 *
 * @note
 * - areaId は現在索敵・戦闘に参加できる有効なエリアで、範囲は 1..4。
 * - プレイヤー船がエリアを移動している途中で、先に割り当て先のエリアを書き込まないこと。
 *   割り当て先は CPlayerShipAssignment が持つ。
 * - 割り当て先のアンカーに到着してから、player_ship_ai_system がこの値を割り当て先エリアに切り替える。
 */
struct CAreaMember {
  uint32_t areaId = 1;
};

/**
 * @brief プレイヤー船のエリア割り当て先
 *
 * @note
 * - areaId はプレイヤー船が向かっている、または既に駐留しているエリアで、範囲は 1..4。
 * - anchorX / anchorY はそのエリアのアンカーのワールド座標。AI が到着してから CAreaMember を更新できる。
 * - assignedAtMs は割り当てた側が書き込む。後の永続化や、エリア移動の順序の調査に使う。
 */
struct CPlayerShipAssignment {
  uint32_t areaId = 1;
  float    anchorX = 0.f;
  float    anchorY = 0.f;
  uint64_t assignedAtMs = 0;
    /**
     * 現在の出撃編成に入っているかどうか。
     *
     * マップ上にはキャラクターが所持する**すべての**精霊が存在する（待機中のものも、プレイヤーが選んで
     * 出撃させられるようにその場にいる）ため、「その場にいる」ことは「出撃している」ことを意味しない。
     * このフラグで区別する必要がある。
     * 撃破による経験値は出撃中の精霊にだけ配る。待機中の精霊は戦闘に参加せず、経験値も得ない。
     *
     * 書き込むのは 2 か所だけ：spawnPlayerTeamUnits（マップ入場時の生成）と
     * assignShipsToArea（編成の切り替え／エリア選択）。どちらも team_ops::deployedUnitIds を基準にしており、
     * 他の場所で独自に判定しないこと。
     */
  bool     deployed = false;
};

/**
 * @brief メインシティでのプレイヤー船の AI 状態
 *
 * @note
 * - 状態遷移を書き込むのは player_ship_ai_system だけ。
 * - MovingToAnchor で到着してから CAreaMember.areaId を更新できる。
 * - Attacking/Healing/Retreating は、nav を active にするかどうかと経路の寿命に影響する。
 */
enum class PlayerShipAIState : uint8_t {
  MovingToAnchor = 0,
  Patrolling     = 1,
  Chasing        = 2,
  Attacking      = 3,
  Retreating     = 4,
  Healing        = 5,
};

/**
 * @brief プレイヤー船の AI の実行時状態
 *
 * @note
 * - currentTarget は現在の MapInstance.world 内部の entt ハンドル。world をまたいで保持しないこと。
 * - nextRetargetAtMs/deathStayStartedAtMs はマップのロジック時刻（ミリ秒）を使う。
 * - Patrolling 用のフィールドは state == Patrolling のときだけ意味を持つ。
 */
struct CPlayerShipAI {
  PlayerShipAIState state = PlayerShipAIState::MovingToAnchor;
  entt::entity      currentTarget = entt::null;
  uint64_t          nextRetargetAtMs = 0;
  uint64_t          deathStayStartedAtMs = 0;  // Retreating に入った時刻

    /**
     * MovingToAnchor に入った時刻。0 は未記録。
     *
     * MovingToAnchor の間、精霊はモンスターのターゲットに選ばれない（city_monster_combat_system を参照）。
     * そのため、この状態で止まってしまった精霊は永久に無敵になる。症状は「立ち止まっている」から
     * 「立ち止まっていて、しかもモンスターに完全に無視されている」に変わり、モンスター AI の問題と
     * いっそう見分けにくくなる。
     * このフィールドはタイムアウト警告に使い、こうした表に出ない障害をログで見えるようにする。
     */
  uint64_t          movingToAnchorSinceMs = 0;
    /// 今回の滞留について警告済みかどうか。毎フレーム警告がログを埋め尽くさないようにする。
  bool              stuckWarned = false;

    // ── Patrolling 状態の徘徊用フィールド（Patrolling のときだけ使う）────
    // CPlayerShipAssignment.anchor{X,Y} を中心に、半径 kPatrolRadius の範囲を歩き回る。
    // プレイヤー船は **AiWander コンポーネントを持たない**。ここで徘徊の目標地点と停止タイマーを直接持つ。
  float    wanderTargetX   = 0.f;
  float    wanderTargetY   = 0.f;
  bool     hasWanderTarget = false;
  float    wanderWaitTimer = 0.f;
};

/**
 * @brief プレイヤー船の経験値とレベルアップの状態
 *
 * @note
 * - level/currentXp/nextLevelXp はメモリ上の正となる状態で、永続化は DB writer がまとめて行う。
 * - lastWrittenXpPct は、しきい値による書き戻しの間引きに使う。範囲は 0..100。
 * - dirtyStats は、レベルアップ後のステータスを DB 保存または同期する必要があることを表す。
 */
struct CExperience {
  uint32_t level = 1;
  uint64_t currentXp = 0;
  uint64_t nextLevelXp = 100;
  uint32_t lastWrittenXpPct = 0;   // 0-100 between writes (50% bucket)
  bool     dirtyStats = false;     // レベルアップ後、書き込み待ち
};

/**
 * @brief 治療施設のパラメータコンポーネント
 *
 * @note
 * - 現在、メインシティの治療施設にはこのコンポーネントを付けていない。
 *   システムは CBuilding(kind=HealingStation) と定数のパラメータを使う。
 * - 治療パラメータを実行時に変更できるようにする場合にだけ、エンティティがこれらのフィールドを持つ。
 */
// HINT: 現在、メインシティの治療施設は CBuilding(kind=HealingStation) で識別し、半径と回復速度は
// healing_station_system の定数から取っている。治療施設のパラメータを DB や実行時の設定にする段階になったら
// このコンポーネントを付ける。その代わり、マップ入場時のエンティティ生成と治療システムの両方を、
// コンポーネントのフィールドを読むように変える必要がある。
struct CHealingStation {
  float    x = 0.f;
  float    y = 0.f;
  float    radius = 300.f;
  float    hpPerSec = 50.f;
  uint32_t factionMask = 1u;  // kFactionPlayer
};
