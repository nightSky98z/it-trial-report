#pragma once
#include "ecs/components.h"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <entt/entt.hpp>

// ── 前方宣言（map_components.h ↔ template_cache.h の循環依存を断つ）──
namespace cache { struct TemplateCache; }

// ═════════════════════════════════════════════════════════════════════════════
// 第 2 層 ECS：ゲーム内のオブジェクト（ユニット、AI、戦闘など）
// ═════════════════════════════════════════════════════════════════════════════

/**
 * @brief スポーンルールの関数シグネチャ
 *
 * @note
 * - world は対象の MapInstance.world。ルールがモンスターのエンティティとコンポーネントを書き込む。
 * - alloc はそのマップインスタンスの NetworkId アロケータ。エンティティの生成時は必ずこれを使うこと。
 * - tc はテンプレートキャッシュの読み取り専用の借用。ルールはその内部へのポインタを保持しないこと。
 */
using SpawnRule = std::function<void(
    entt::registry &world,
    NetworkEntityIdAllocator &alloc,
    const cache::TemplateCache &tc
)>;

/**
 * @brief マップのワールド座標
 *
 * @note
 * - x/y はサーバーのワールド単位。クライアント座標との縮尺は runtime_config で固定している。
 * - 値型なのでそのままコピーでき、所有権は表さない。
 */
struct Position {
  float x = 0.f;
  float y = 0.f;
};

/**
 * @brief ユニットの向き
 *
 * @note
 * - 列挙値はクライアントの Game::FacingDir やアニメーションの向きの取り決めと一致させること。
 */
enum class FacingDir : uint8_t {
  Down = 0,
  Up = 1,
  Left = 2,
  Right = 3,
};

/**
 * @brief ユニットの向きの実行時状態
 *
 * @note
 * - value は現在の正となる向き。
 * - dirty は、このフレームの動的スナップショットやイベント配信に含める必要があることを表す。
 */
struct Facing {
  FacingDir value = FacingDir::Down;
  bool dirty = false;
};

/**
 * @brief 陣営／分類のタグビット
 *
 * @note
 * - これらの値はビットマスクとして使い、組み合わせて EntityTags::mask に書き込める。
 * - タグを追加する場合は、索敵・AOI・プロトコル側の対応が必要かを確認すること。
 */
enum class EntityTag : uint32_t {
  None = 0,
  Player = 1u << 0,
  Enemy = 1u << 1,
  NPC = 1u << 2,
};

inline constexpr uint32_t kEntityTagNone = static_cast<uint32_t>(EntityTag::None);
inline constexpr uint32_t kEntityTagPlayer = static_cast<uint32_t>(EntityTag::Player);
inline constexpr uint32_t kEntityTagEnemy = static_cast<uint32_t>(EntityTag::Enemy);
inline constexpr uint32_t kEntityTagNPC = static_cast<uint32_t>(EntityTag::NPC);

/**
 * @brief 戦闘陣営のビットマスク定数
 *
 * @note
 * - factionMask は自分が属する集合、enemyMask は攻撃できる集合を表す。
 * - NPC は現在どの対象も攻撃できない。戦闘する NPC にする場合は索敵システムもあわせて修正すること。
 */
static constexpr uint32_t kFactionPlayer = 1u;
static constexpr uint32_t kFactionEnemy = 2u;
static constexpr uint32_t kFactionNPC = 4u;
static constexpr uint32_t kPlayerEnemyMask = kFactionEnemy;    // プレイヤーは敵を攻撃する
static constexpr uint32_t kEnemyEnemyMask = kFactionPlayer;    // 敵はプレイヤーを攻撃する
static constexpr uint32_t kNPCEnemyMask = 0;                    // NPC は攻撃しない

/**
 * @brief エンティティの分類タグのコンポーネント
 *
 * @note
 * - mask は EntityTag のビットマスク。0 は未分類を表す。
 */
struct EntityTags {
  uint32_t mask = kEntityTagNone;
};

/**
 * @brief 戦闘陣営のコンポーネント
 *
 * @note
 * - 戦闘や索敵に参加できるすべてのユニットに付ける。
 * - factionMask は自身のタグ、enemyMask はこのユニットが攻撃してよい対象の集合。
 */
struct UnitFaction {
  uint32_t factionMask = 0;   // 自分のタグ
  uint32_t enemyMask = 0;     // 攻撃できる対象のタグ
};

/**
 * @brief このフレームで位置が変わったことを示すフラグ
 *
 * @note
 * - velocity/movement 系のシステムが書き込み、AOI の配信で使った後にクリアしてよい。
 * - dirty=false はエンティティが見えないことを意味しない。位置の変化による差分送信が不要なだけ。
 */
struct PositionDirty {
  bool dirty = false;
};

/**
 * @brief メインシティのエリアの内部インデックス
 *
 * @note
 * - 列挙は 0 から始まる。クライアントの通信上の areaId は 1..4 なので、両者を混同しないこと。
 */
enum class CityAreaId : uint8_t {
  MainCity = 0,     // メインシティ（モンスターは出現しない）
  Forest = 1,       // 森
  Battlefield = 2,  // 戦場
  Graveyard = 3,    // 墓地
  Count = 4         // エリアの総数
};

/**
 * @brief メインシティのエリア 1 つの解放状態
 *
 * @note
 * - requiredPower は現在の単純な戦闘力のしきい値。永続化や設定化をするまでは DB のフィールドとして扱わないこと。
 */
struct CityAreaState {
  bool unlocked = false;        // 解放済みかどうか
  uint32_t requiredPower = 0;   // 解放に必要な戦闘力
};

/**
 * @brief メインシティのエリアの解放状態の表
 *
 * @note
 * - areas の添字は CityAreaId の内部インデックスで、クライアントの通信上の areaId ではない。
 * - 現在はコンストラクタがデフォルト設定を書き込む。今後 DB や設定から読み込むようにする際は、明示的な所有者に置き換えること。
 */
struct CityAreas {
  std::array<CityAreaState, 4> areas;

  CityAreas() {
    areas[0].unlocked = true;  areas[0].requiredPower = 0;     // メインシティ
    areas[1].unlocked = true;  areas[1].requiredPower = 0;     // 森
    areas[2].unlocked = false; areas[2].requiredPower = 1000;   // 戦場
    areas[3].unlocked = false; areas[3].requiredPower = 2000;   // 墓地
  }

    /**
     * @brief 現在の戦闘力に応じてエリアの解放状態を更新する
     *
     * @param combatPower 現在のキャラクターまたは編成の戦闘力。
     *
     * @note
     * - すべての areas[*].unlocked を上書きする。呼び出し前に手動で付けた一時的な解放フラグは残らない。
     */
  void updateByPower(uint32_t combatPower) {
    for (auto &area : areas) {
      area.unlocked = combatPower >= area.requiredPower;
    }
  }
};

/**
 * @brief メインシティのエリアが解放済みかどうかを調べる
 *
 * @param cityAreas MapInstance が属するメインシティのエリア状態。呼び出し中だけ借用する。
 * @param wireAreaId クライアントのプロトコル上の area_id。範囲は 1..4。
 *
 * @retval true エリアが存在し、解放済み。
 * @retval false area_id が範囲外、またはエリアがまだロックされている。呼び出し側は船の派遣を拒否すること。
 */
inline bool cityAreaIsUnlocked(const CityAreas &cityAreas, uint32_t wireAreaId) {
  if (wireAreaId < 1 || wireAreaId > cityAreas.areas.size()) return false;
  return cityAreas.areas[wireAreaId - 1].unlocked;
}

// ── スポーン地点 ──────────────────────────────────────────────────
// 重複していた定義は削除した。template_cache.h の cache::AreaSpawnPoint に統一している

/**
 * @brief ユニットの基本状態のコンポーネント
 *
 * @note
 * - MapInstance.world の戦闘ユニットに付ける。
 * - unitKind/lifeStatus/actionState/facing は FlatBuffers の Game 列挙値を使う。
 * - x/y は正となるワールド座標。Position コンポーネントがある場合も、ここと二重に正を持たないこと。
 * - curHp/maxHp はプロトコルのスナップショット用のフィールド。戦闘システムが書き込む際は maxHp >= curHp を保つこと。
 */
struct UnitBase {
  uint32_t unitId = 0;     // unit_templates のテンプレート ID
  uint32_t campId = 0;     // 陣営
  uint8_t unitKind = 0;    // Game::UnitKind
  float x = 0.f;
  float y = 0.f;
  uint32_t curHp = 0;
  uint32_t maxHp = 0;
  uint8_t lifeStatus = 0;  // Game::LifeStatus
  uint8_t actionState = 0; // Game::ActionState
  uint8_t facing = 0;      // 0 down / 1 up / 2 left / 3 right
  uint32_t level = 1;      // 戦闘・経験値イベントで使うユニットのレベル。未設定ならレベル 1 として扱う
};

/**
 * @brief ユニットの戦闘数値とクールダウンの実行時状態
 *
 * @note
 * - 現在、生死の正となる状態は UnitBase.lifeStatus/actionState にある。dead/revive_remain_sec は
 *   旧処理のために残しているフィールドで、現在のメインシティの戦闘システムは管理していない。
 * - UnitBase のプロトコル用 HP フィールドと、ここの HP フィールドは、書き込む箇所で同期させる必要がある。
 */
struct UnitCombat {
  int max_hp = 100;
  int cur_hp = 100;
  int attack_power = 100;
  int combat_power = 0;
  float move_speed = 200.f;
  float attack_range = 60.f;
  float attack_commit_buffer = 8.f;
  float attack_cooldown = 1.f;
  float auto_battle_radius = 0.f;
  float cooldown_remain_sec = 0.f;
  bool dead = false;
  float revive_remain_sec = 0.f;
};

/**
 * @brief 索敵センサーの形状
 *
 * @note
 * - UnitSensor の半径／矩形／扇形のフィールドと組み合わせて使う。
 */
enum class DetectShape : uint8_t {
  Circle = 0,
  Sector = 1,
  Rect = 2,
};

/**
 * @brief ユニットの索敵センサー
 *
 * @note
 * - current_hits/entered_hits はターゲットのインスタンスの文字列 ID を使う。コンテナの中身は呼び出し側が所有する。
 * - faction_mask は感知できる対象の集合で、UnitFaction::enemyMask と同じではない。
 */
struct UnitSensor {
  DetectShape shape = DetectShape::Circle;  // 感知範囲の形状
  float radius = 0.f;                       // 円の半径
  float rect_width = 0.f;                   // 矩形の幅
  float rect_height = 0.f;                  // 矩形の高さ
  float sector_angle_deg = 90.f;             // 扇形の角度
  uint32_t faction_mask = 0;               // どのタグを感知するか
  std::unordered_set<std::string> current_hits;
  std::vector<std::string> entered_hits;
};

/**
 * @brief ユニットのヘイトの実行時状態
 *
 * @note
 * - target_instance_id は現在最も優先度の高いターゲットの業務上の ID で、entt::entity ではない。
 * - threat_table はヘイトシステムが管理し、フレームをまたいで保持する。ターゲットが死亡・離脱したら必ず削除すること。
 */
struct UnitAggro {
  bool in_combat = false;
  std::string target_instance_id;
  float threat = 0.f;
  std::unordered_map<std::string, float> threat_table;
};

/**
 * @brief エンティティを所有するキャラクター
 *
 * @note
 * - メインシティではプレイヤー船が属する roleId を表す。ダンジョンではホストや編成の所有者を表すこともある。
 * - roleId は業務上の意味に基づいてコピーした所有権付きの文字列で、外部の DB の行を借用してはいない。
 */
struct Owner {
  std::string roleId;
};

/**
 * @brief メインシティの永続化データ
 *
 * @note
 * - DB の role_cities テーブルから読み込み、関連するエンティティやコンテキストに付ける。
 * - cityLevel は業務上のレベルで、mapId ではない。
 */
struct CityData {
  uint32_t cityLevel = 1;
};

/**
 * @brief AI の意思決定に使う入力のスナップショット
 *
 * @note
 * - 感知や戦闘距離を計算するシステムが書き込み、AI のステートマシンはこのフレームのスナップショットを読むだけ。
 * - enabled=false の場合、ステートマシンはそのユニットをスキップし、暗黙の状態遷移を行わないこと。
 */
struct UnitAIContext {
  bool enabled = false;
  bool hasTarget = false;
  bool targetVisible = false;
  bool targetInAttackRange = false;
  bool targetInAttackCommitRange = false;
  bool isDead = false;
  bool isManualMoving = false;
  float distanceToTarget = 0.f;
};

/**
 * @brief 汎用のプレイヤー／ユニットの AI 状態の列挙
 *
 * @note
 * - 値は旧プロトコルやテストと互換性を保つ必要がある。状態を追加したら状態イベントのエンコードもあわせて修正すること。
 */
enum class PlayerAIStateType : uint8_t {
  Idle = 0,
  Moving = 1,
  Chase = 4,
  Combat = 5,
  Dead = 8,
};

/**
 * @brief 汎用の AI 状態のコンポーネント
 *
 * @note
 * - state/prev_state には PlayerAIStateType の uint8_t 値を格納する。
 * - dirty=true は状態イベントが配信待ちであることを表し、イベントのクリアを行うシステムが処理した後にリセットしてよい。
 */
struct UnitAIState {
  uint8_t state = static_cast<uint8_t>(PlayerAIStateType::Idle);
  uint8_t prev_state = static_cast<uint8_t>(PlayerAIStateType::Idle);
  bool dirty = false;
};

/**
 * @brief 現在ロックしているターゲット
 *
 * @note
 * - target_entity は現在の registry 内部のハンドル。world をまたいで保持しないこと。
 * - target_instance_id はシステム間のイベントの関連付けに使う。distance/visible はこのフレームのスナップショット。
 */
struct UnitTarget {
  entt::entity target_entity = entt::null;
  std::string target_instance_id;
  float distance = 0.f;
  bool visible = false;
};

/**
 * @brief エリアの矩形境界のキャッシュ
 *
 * @note
 * - city_monster_factory が生成したモンスターにだけ付ける。プレイヤー船には付けない。
 * - movement_clamp_system が velocity_system の後に読み、範囲外へ出たときに強制的に押し戻す保険として使う。
 * - monster_wander_system / city_monster_combat_system は nav のエリアを使い、bounds は読まない。
 */
struct CAreaBounds {
  float minX = 0.f, maxX = 0.f;
  float minY = 0.f, maxY = 0.f;
};

/**
 * @brief 汎用のアクション種別
 *
 * @note
 * - 現在は主にダンジョンや旧 AI のアクション処理で使う。メインシティのプレイヤー船は Game::ActionState を使う。
 */
enum class PlayerActionType : uint8_t {
  None = 0,
  Idle = 1,
  ChaseMove = 4,
  BasicAttack = 7,
};

/**
 * @brief ユニットのアクションの意図
 *
 * @note
 * - type には PlayerActionType の uint8_t 値を使う。
 * - dirty=true はこのフレームに新しい意図があることを表し、アクションシステムが処理した後にリセットする責任を持つ。
 */
struct UnitActionIntent {
  uint8_t type = static_cast<uint8_t>(PlayerActionType::None);
  entt::entity target = entt::null;
  float target_x = 0.f;
  float target_y = 0.f;
  bool dirty = false;
};

/**
 * @brief ユニットのアクションの実行状態
 *
 * @note
 * - current には PlayerActionType の uint8_t 値を使う。
 * - elapsed/duration はアクションシステムが進める。interruptible はアクションの切り替えを許すかどうかを制御する。
 */
struct UnitActionState {
  uint8_t current = static_cast<uint8_t>(PlayerActionType::None);
  float elapsed = 0.f;
  float duration = 0.f;
  bool interruptible = true;
};

/**
 * @brief 移動の意図
 *
 * @note
 * - 現在は主にダンジョンや手動移動の処理で使う。
 * - dirty=true はこのフレームに新しい入力があることを表し、処理した後にリセットすること。
 */
struct MoveIntent {
  float x = 0.f;
  float y = 0.f;
  float speed = 0.f;
  bool dirty = false; // true = このフレームに新しい入力がある
};

/**
 * @brief モンスターの固定スポーン地点への逆参照
 *
 * @note
 * - city_monster_factory::spawnAtPoint が、生成したモンスターに書き込む。
 * - 死亡時に monster_lifecycle_system::consumeDeaths が読み取り、リスポーン枠を作る。
 * - 固定スポーンでないモンスターはこのコンポーネントを持たないので、死亡しても自動でリスポーンしない。
 */
struct CSpawnOrigin {
  uint64_t spawnPointId = 0;
  uint32_t unitId = 0;
  uint32_t campId = 0;
};

/**
 * @brief 死体を残しておく時間のタイマー
 *
 * @note
 * - monster_lifecycle_system::consumeDeaths が、死亡イベントのフレームで付ける。
 * - remainSec が 0 になったら update がエンティティを破棄し、その後の AOI の left リストでフロントエンドにノードの削除を通知する。
 */
struct CCorpseTimer {
  float remainSec = 1.0f;
};

/**
 * @brief スポーン地点のリスポーン枠
 *
 * @note
 * - 独立したエンティティで、死体のエンティティとは切り離されている。
 * - remainSec が 0 になったら、update が元のスポーン地点に新しいモンスターを生成し、この枠のエンティティを破棄する。
 */
struct CMonsterRespawnSlot {
  uint64_t spawnPointId = 0;
  uint32_t unitId = 0;
  uint32_t campId = 0;
  float    remainSec = 5.0f;
};

/**
 * @brief ユニットの速度ベクトル
 *
 * @note
 * - vx/vy の単位はワールド単位毎秒。
 * - nav_follow/AI/移動システムが書き込み、velocity_system が読んで位置を進める。
 */
struct Velocity {
  float vx = 0.f;
  float vy = 0.f;
};

/**
 * @brief マップ内のプロトコル／AOI 用のエンティティ ID
 *
 * @note
 * - MapInstance.world のゲームオブジェクトに付ける。
 * - entt::entity はサーバー内部のハンドルで、プロトコルには載せない。
 * - value は現在の MapInstance 内でのプロトコル上の主キー。C2S の ID を処理する前に、まず MapInstance を特定すること。
 */
struct NetworkId {
  uint32_t value = 0;
};

// HINT: ダンジョンのインスタンスに独自の実行時データ、自動戦闘のオン／オフ、手動移動のロックが必要になったら、
// 対応するコンポーネントを追加する。その代わり、game_loop / マップスケジューラで City と Dungeon のステートマシンを区別する必要がある。

/**
 * @brief AOI で見えているユニットの状態のスナップショット
 *
 * @note
 * - 前のフレームの送信判断に使う値のスナップショットで、エンティティの寿命は管理しない。
 * - lifeStatus/actionState は Game の列挙値を使う。
 * - cadence/cooldownTicks/lastPush* は AOI の送信リズムを決めるロジックが管理する。
 */
struct VisibleUnitState {
  float x = 0.f;
  float y = 0.f;
  uint32_t curHp = 0;
  uint32_t maxHp = 0;
  uint8_t lifeStatus = 0;
  uint8_t actionState = 0;

    // ── 送信リズム用のフィールド ──
  uint8_t cooldownTicks = 0;  // 次の「定期送信（取りこぼし防止）」まで残り何回の AOI 周期か
  uint8_t cadence = 1;        // 現在の送信の間隔。具体的な値は game_loop.cpp の kCadence* で定義している
  bool wasMoving = false;      // 前回の送信時に移動していたかどうか
  uint64_t lastPushAtMs = 0;
  uint64_t lastPushTick = 0;
};

/**
 * @brief viewer 1 人分の、前のフレームで見えていたエンティティの集合
 *
 * @note
 * - 見えているエンティティは SoA の列で保存し、AOI の送信のたびにノード型の map を作り直さずに済むようにしている。
 * - indexByNetworkEntityId は派生インデックス。一括で書き込んだ後に rebuildIndex() で作り直す。
 * - 検索結果はこのオブジェクトの寿命の間だけ有効で、エンティティがまだ生きていることは意味しない。
 */
struct ViewerAoiState {
    /**
     * @brief viewer が前のフレームで何も見えていなかったかを判定する
     *
     * @retval true SoA の列が空。
     */
  bool empty() const { return networkEntityIds.empty(); }

    /**
     * @brief 前のフレームで見えていたエンティティの数を返す
     *
     * @return networkEntityIds の列の長さ。
     */
  std::size_t size() const { return networkEntityIds.size(); }

    /**
     * @brief 前のフレームで見えていたエンティティのプロトコル ID の列を借用する
     *
     * @return 読み取り専用の vector への参照。寿命は ViewerAoiState に依存する。
     *
     * @note
     * - clear()/pushVisible()/reserve() をまたいで内部への参照を保持しないこと。
     */
  const std::vector<uint32_t> &networkIds() const {
    return networkEntityIds;
  }

    /**
     * @brief デバッグ用に、SoA の列の長さと派生インデックスの整合性を検証する
     *
     * @retval true すべての列の長さが一致し、インデックスが正しい ID を指している。
     * @retval false 呼び出し側が一括書き込み／rebuildIndex の順序を守っていない。
     */
  bool debugValidate() const {
    const std::size_t count = networkEntityIds.size();
    if (xs.size() != count || ys.size() != count ||
        curHps.size() != count || maxHps.size() != count ||
        lifeStatuses.size() != count || actionStates.size() != count ||
        cooldownTicks.size() != count || cadences.size() != count ||
        movingFlags.size() != count || lastPushAtMs.size() != count ||
        lastPushTicks.size() != count) {
      return false;
    }

    for (const auto &[networkEntityId, index] : indexByNetworkEntityId) {
      if (index >= count || networkEntityIds[index] != networkEntityId) {
        return false;
      }
    }
    return true;
  }

    /**
     * @brief SoA の列の容量を予約する
     *
     * @param count 格納したい、見えているエンティティの数。
     *
     * @note
     * - capacity にだけ影響し、size やインデックスの意味は変えない。
     */
  void reserve(std::size_t count) {
    networkEntityIds.reserve(count);
    xs.reserve(count);
    ys.reserve(count);
    curHps.reserve(count);
    maxHps.reserve(count);
    lifeStatuses.reserve(count);
    actionStates.reserve(count);
    cooldownTicks.reserve(count);
    cadences.reserve(count);
    movingFlags.reserve(count);
    lastPushAtMs.reserve(count);
    lastPushTicks.reserve(count);
    indexByNetworkEntityId.reserve(count);
  }

    /**
     * @brief 前のフレームの可視集合と派生インデックスをクリアする
     *
     * @note
     * - 呼び出し後も以前の networkIds() の参照は同じ vector オブジェクトを指すが、中身は空になる。
     * - capacity は縮めず、AOI の tick をまたいでメモリを再利用する。
     */
  void clear() {
    networkEntityIds.clear();
    xs.clear();
    ys.clear();
    curHps.clear();
    maxHps.clear();
    lifeStatuses.clear();
    actionStates.clear();
    cooldownTicks.clear();
    cadences.clear();
    movingFlags.clear();
    lastPushAtMs.clear();
    lastPushTicks.clear();
    indexByNetworkEntityId.clear();
  }

    /**
     * @brief 見えているエンティティの状態を 1 件追加する
     *
     * @param networkEntityId 現在の MapInstance 内でのプロトコル／AOI 用 ID。
     * @param state 保存する値のスナップショット。列ごとにコピーする。
     *
     * @note
     * - networkEntityId が重複している場合は無視する。呼び出し側は後から書いたもので上書きされることを当てにしないこと。
     */
  void pushVisible(uint32_t networkEntityId, const VisibleUnitState &state) {
    const auto existingIt = indexByNetworkEntityId.find(networkEntityId);
    if (existingIt != indexByNetworkEntityId.end()) {
      return;
    }

    reserve(size() + 1);

    const std::size_t index = networkEntityIds.size();
    indexByNetworkEntityId.emplace(networkEntityId, index);

    networkEntityIds.push_back(networkEntityId);
    xs.push_back(state.x);
    ys.push_back(state.y);
    curHps.push_back(state.curHp);
    maxHps.push_back(state.maxHp);
    lifeStatuses.push_back(state.lifeStatus);
    actionStates.push_back(state.actionState);
    cooldownTicks.push_back(state.cooldownTicks);
    cadences.push_back(state.cadence);
    movingFlags.push_back(state.wasMoving ? 1u : 0u);
    lastPushAtMs.push_back(state.lastPushAtMs);
    lastPushTicks.push_back(state.lastPushTick);
    assert(debugValidate());
  }

    /**
     * @brief networkEntityIds の列から派生インデックスを作り直す
     *
     * @note
     * - networkEntityIds や SoA の列を一括で書き換えた後にだけ呼ぶ。
     * - 呼び出し前に、列の長さが一致していること。
     */
  void rebuildIndex() {
    assert(debugValidate());

    indexByNetworkEntityId.clear();
    indexByNetworkEntityId.reserve(networkEntityIds.size());
    for (std::size_t index = 0; index < networkEntityIds.size(); ++index) {
      indexByNetworkEntityId[networkEntityIds[index]] = index;
    }
  }

    /**
     * @brief 前のフレームに、あるプロトコル上のエンティティ ID が含まれていたかを判定する
     *
     * @param networkEntityId 現在の MapInstance 内でのプロトコル／AOI 用 ID。
     *
     * @retval true インデックスにその ID がある。
     */
  bool contains(uint32_t networkEntityId) const {
    return indexByNetworkEntityId.find(networkEntityId) !=
           indexByNetworkEntityId.end();
  }

    /**
     * @brief あるプロトコル上のエンティティ ID の、前のフレームの状態を読み取る
     *
     * @param networkEntityId 現在の MapInstance 内でのプロトコル／AOI 用 ID。
     * @param out 成功時に状態のスナップショットを書き込む。
     *
     * @retval true 見つかったので out に書き込んだ。
     * @retval false 前のフレームでは見えていなかった、またはインデックスがない。
     */
  bool loadState(uint32_t networkEntityId, VisibleUnitState &out) const {
    assert(debugValidate());

    const auto it = indexByNetworkEntityId.find(networkEntityId);
    if (it == indexByNetworkEntityId.end()) {
      return false;
    }

    out = stateAt(it->second);
    return true;
  }

    /**
     * @brief 前のフレームで見えていたエンティティの ID の列を、末尾に追加コピーする
     *
     * @param out 呼び出し側が所有する出力先の vector。末尾に追加する。
     *
     * @note
     * - out はクリアしない。必要なら呼び出し側でクリアすること。
     */
  void copyNetworkEntityIds(std::vector<uint32_t> &out) const {
    out.insert(out.end(), networkEntityIds.begin(), networkEntityIds.end());
  }

private:
  std::vector<uint32_t> networkEntityIds;  // 前のフレームで見えていたプロトコル／AOI 用 ID の集合
  std::vector<float> xs;
  std::vector<float> ys;
  std::vector<uint32_t> curHps;
  std::vector<uint32_t> maxHps;
  std::vector<uint8_t> lifeStatuses;
  std::vector<uint8_t> actionStates;
  std::vector<uint8_t> cooldownTicks;
  std::vector<uint8_t> cadences;
  std::vector<uint8_t> movingFlags;
  std::vector<uint64_t> lastPushAtMs;
  std::vector<uint64_t> lastPushTicks;
  std::unordered_map<uint32_t, std::size_t> indexByNetworkEntityId;

    /**
     * @brief SoA の添字から、見えている状態のスナップショットを組み立てる
     *
     * @param index SoA の列の添字。size() より小さいこと。
     *
     * @return 対応するエンティティの値のスナップショット。
     *
     * @note
     * - 呼び出し前に、列の長さが一致していることを保証すること。
     */
  VisibleUnitState stateAt(std::size_t index) const {
    assert(index < size());

    VisibleUnitState state;
    state.x = xs[index];
    state.y = ys[index];
    state.curHp = curHps[index];
    state.maxHp = maxHps[index];
    state.lifeStatus = lifeStatuses[index];
    state.actionState = actionStates[index];
    state.cooldownTicks = cooldownTicks[index];
    state.cadence = cadences[index];
    state.wasMoving = movingFlags[index] != 0;
    state.lastPushAtMs = lastPushAtMs[index];
    state.lastPushTick = lastPushTicks[index];
    return state;
  }

};
