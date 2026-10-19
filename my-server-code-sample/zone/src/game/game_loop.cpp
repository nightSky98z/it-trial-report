#include "game/game_loop.h"
#include "game/battle_event_pack.h"
#include "game/player_team_runtime.h"
#include "game/game_session.h"
#include "game/player_ship_constants.h"
#include "game/template_cache.h"
#include "config/config_reload.h"
#include "game/building_cache.h"
#include "game/city_nav_grid.h"
#include "game/item_cache.h"
#include "game/equip_handler.h"
#include "game/shop_handler.h"
#include "game/team_ops.h"
#include "db/player_ship_db_writer.h"
#include "db/role_economy_db_writer.h"
#include "ecs/aggro_clear_subscriber.h"
#include "ecs/monster_wander_system.h"
#include "ecs/area_registry_system.h"
#include "ecs/city_combat_helpers.h"
#include "ecs/city_monster_combat_system.h"
#include "ecs/city_monster_factory.h"
#include "ecs/healing_station_system.h"
#include "ecs/boss_challenge_system.h"
#include "ecs/item_drop_system.h"
#include "ecs/pickable_component.h"
#include "ecs/building_component.h"
#include "ecs/events.h"
#include "ecs/map_components.h"
#include "ecs/nav_components.h"
#include "ecs/nav_follow_system.h"
#include "ecs/nav_request_system.h"
#include "ecs/monster_lifecycle_system.h"
#include "ecs/player_ship_ai_system.h"
#include "ecs/velocity_system.h"
#include "ecs/movement_clamp_system.h"
#include "game/nav_grid.h"
#include "network/msg_types.h"
#include "protocol/codec.h"
#include "protocol/roster_codec.h"
#include "protocol/shop_codec.h"
#include "generated/game_generated.h"
#include "runtime/shutdown.h"
#include "utils/log.h"
#include "utils/time_utils.h"
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <unordered_set>
#include <entt/entt.hpp>

namespace {

// NetworkEntityIdAllocator は ecs/map_components.h で定義している
constexpr uint64_t kAoiPushIntervalMs = 33;  // 30Hz (~33.33ms)
constexpr float kAoiPushIntervalSec =
    static_cast<float>(kAoiPushIntervalMs) / 1000.f;
constexpr float kAoiPositionEpsilon = 0.01f;
/**
 * @brief AOI 送信時に、速度が静止しているとみなすしきい値
 *
 * @note
 * - 現在、フロントエンドとサーバーの座標系は 1 world unit = 1 ピクセルとして扱っている。
 */
constexpr float kAoiVelocityEpsilon = 0.01f;

/**
 * @brief 移動中は AOI の周期ごとに毎回送る
 */
constexpr uint8_t kCadenceActive   = 1;   // 移動中：AOI の周期ごとに毎回送る（33ms / 30Hz）

/**
 * @brief 停止した直後の、頻度を落とした送信周期
 */
constexpr uint8_t kCadenceCoasting = 3;   // 停止直後：3 周期に 1 回送る（約 100ms）。これを 2 回続ける

/**
 * @brief 完全に静止しているときの定常の送信周期
 */
constexpr uint8_t kCadenceSteady   = 2;   // 完全に静止：2 周期に 1 回送る（約 66ms）

/**
 * @brief メインシティのインスタンスが無人になってから回収するまでの猶予時間
 */
constexpr uint64_t kCityIdleGracePeriodMs = 30'000;  // アイドル時の猶予：30 秒

/**
 * @brief メインシティのインスタンスの GC の走査間隔
 */
constexpr float    kCityGcIntervalSec    = 5.f;     // GC の走査間隔：5 秒

/**
 * @brief マップインスタンスの現在のスナップショット時刻を返す
 *
 * @return インスタンスのシミュレーション時計に対応するミリ秒。
 */
uint64_t snapshotTimeMsForInstance(const MapInstance &inst) {
  return mapSimTimeMs(inst.simClock);
}

/**
 * @brief プロトコルのスナップショットから、viewer の前フレームの状態を作る
 *
 * @return AOI の送信間引きの比較に必要なフィールドだけを含む。
 */
VisibleUnitState buildVisibleUnitState(const codec::UnitSnapshotData &snap) {
  return VisibleUnitState{
    .x = snap.x,
    .y = snap.y,
    .curHp = snap.curHp,
    .maxHp = snap.maxHp,
    .lifeStatus = static_cast<uint8_t>(snap.lifeStatus),
    .actionState = static_cast<uint8_t>(snap.actionState),
  };
}

/**
 * @brief AOI でエンティティ 1 つを送るかどうかの判定結果
 *
 * @note
 * - Skip はエンコードしない、Push は状態が変わったので即座にエンコードする、Cadence は送信間隔が来たのでエンコードする、を表す。
 */
enum class PushVerdict : uint8_t {
  Skip,        // 送らない
  Push,        // 送る（状態が変わった）
  Cadence,     // 送る（送信間隔が来た）
};

/**
 * @brief 現在のスナップショットと前フレームの状態の差分を調べる
 *
 * @note
 * - 今回の AOI の判定の中でだけ一時的に使う型で、外部のリソースは持たない。
 */
struct PushProbe {
  float dx = 0.f;
  float dy = 0.f;
  bool hpChanged = false;
  bool statusChanged = false;
  bool posChanged = false;
};

/**
 * @brief ECS のコンポーネントから集めた、移動元の情報
 *
 * @note
 * - found=false は、networkEntityId に一致するエンティティが現在存在しないことを表す。
 */
struct MotionSourceProbe {
  float baseX = 0.f;
  float baseY = 0.f;
  float vx = 0.f;
  float vy = 0.f;
  bool hasVelocity = false;
  bool found = false;
};

/**
 * @brief AOI 送信用の差分情報を作る
 *
 * @return 位置・HP・状態の変化フラグ。
 */
PushProbe buildPushProbe(const codec::UnitSnapshotData &snap,
                         const VisibleUnitState &prev) {
  PushProbe probe;
  probe.hpChanged = snap.curHp != prev.curHp || snap.maxHp != prev.maxHp;
  probe.statusChanged =
      static_cast<uint8_t>(snap.lifeStatus) != prev.lifeStatus ||
      static_cast<uint8_t>(snap.actionState) != prev.actionState;
  probe.dx = std::fabs(snap.x - prev.x);
  probe.dy = std::fabs(snap.y - prev.y);
  probe.posChanged =
      probe.dx > kAoiPositionEpsilon || probe.dy > kAoiPositionEpsilon;
  return probe;
}

/**
 * @brief 送信判定のログ用の文字列を返す
 *
 * @return 静的な文字列。解放は不要。
 */
const char *toString(PushVerdict verdict) {
  switch (verdict) {
  case PushVerdict::Skip:
    return "skip";
  case PushVerdict::Push:
    return "push";
  case PushVerdict::Cadence:
    return "cadence";
}
  return "unknown";
}

/**
 * @brief networkEntityId から、エンティティの現在の移動元の情報を探す
 *
 * @return found=true のとき、UnitBase の座標と、あれば Velocity を含む。
 */
MotionSourceProbe collectMotionSourceProbe(entt::registry &world,
                                           uint32_t networkEntityId) {
  MotionSourceProbe probe;
  auto view = world.view<NetworkId, UnitBase>();
  for (auto e : view) {
    const auto &nid = view.get<NetworkId>(e);
    if (nid.value != networkEntityId) {
      continue;
    }

    const auto &base = view.get<UnitBase>(e);
    probe.baseX = base.x;
    probe.baseY = base.y;
    if (const auto *vel = world.try_get<Velocity>(e)) {
      probe.vx = vel->vx;
      probe.vy = vel->vy;
      probe.hasVelocity = true;
    }
    probe.found = true;
    break;
  }
  return probe;
}

/**
 * @brief エンティティを現在も移動中とみなすべきかを判定する
 *
 * @retval true 位置が変わった、または速度がしきい値を超えている。
 */
bool isMotionActive(const PushProbe &probe, const MotionSourceProbe &motion) {
  if (probe.posChanged) {
    return true;
  }

  if (!motion.hasVelocity) {
    return false;
  }

  return std::fabs(motion.vx) > kAoiVelocityEpsilon ||
         std::fabs(motion.vy) > kAoiVelocityEpsilon;
}

/**
 * @brief AOI でエンティティ 1 つの動きを追跡するデバッグ用の入口
 *
 * @note
 * - 現在の実装はデフォルトで何も出力しない。一時的に詳しく追跡したいときのために引数を残している。
 */
void logAoiEntityTrace(uint8_t zoneId, uint32_t mapId,
                       const std::string &viewerRoleId, uint64_t aoiTick,
                       const char *eventName, uint32_t networkEntityId,
                       uint64_t msSinceLastPush,
                       const codec::UnitSnapshotData *snap,
                       const VisibleUnitState *prevState,
                       const VisibleUnitState *nextState,
                       const PushProbe *probe,
                       const char *verdictText, MotionSourceProbe motion) {
  (void)zoneId;
  (void)mapId;
  (void)viewerRoleId;
  (void)aoiTick;
  (void)eventName;
  (void)networkEntityId;
  (void)msSinceLastPush;
  (void)snap;
  (void)prevState;
  (void)nextState;
  (void)probe;
  (void)verdictText;
  (void)motion;
  }

/**
 * @brief エンティティ 1 つについて、このフレームに AOI で送る必要があるかを評価する
 *
 * @param probe 現在のスナップショットと前フレームの状態の差分。
 * @param motion ECS 上の移動元の状態。
 * @param prev viewer の前フレームの可視状態。送信間隔のフィールドを更新する。
 *
 * @return このフレームの送信判定。
 */
PushVerdict evaluatePush(const PushProbe &probe, const MotionSourceProbe &motion,
                         VisibleUnitState &prev) {
  const bool movingNow = isMotionActive(probe, motion);

    // 移動中：高頻度で送り続ける
  if (movingNow) {
    prev.cadence = kCadenceActive;
    prev.cooldownTicks = 0;
    prev.wasMoving = true;
    return PushVerdict::Push;
  }

    // 停止直後：このフレームに action/status の変化があっても、まず coasting に切り替える
  if (prev.wasMoving) {
    prev.wasMoving = false;
    prev.cadence = kCadenceCoasting;
    prev.cooldownTicks = prev.cadence;
    return PushVerdict::Push;
  }

    // 状態だけの変化：即座に送るが、静止しているエンティティを active の送信間隔に戻さない
  if (probe.hpChanged || probe.statusChanged) {
    if (prev.cadence == 0) {
      prev.cadence = kCadenceSteady;
    }
    prev.cooldownTicks = prev.cadence;
    return PushVerdict::Push;
  }

    // 送信間隔のカウントダウン
  if (prev.cooldownTicks > 0) {
    prev.cooldownTicks--;
  }

  if (prev.cooldownTicks == 0) {
    if (prev.cadence == kCadenceCoasting) {
      prev.cadence = kCadenceSteady;
    }
    prev.cooldownTicks = prev.cadence;
    return PushVerdict::Cadence;
  }

  return PushVerdict::Skip;
}

/**
 * @brief sub-world 内のすべての建物の静的スナップショットを集める
 *
 * @return 所有権を持つ vector。JoinMapSnapshot に使う。
 */
/**
 * @brief マップインスタンスに、キャラクターの建物のエンティティを生成する
 *
 * @param inst 対象のマップインスタンス。エンティティを生成し、network entity id を割り当てる。
 * @param rb 借用するキャラクターの建物の状態。
 * @param bc 借用する建物テンプレートのキャッシュ。building_id の妥当性の確認に使う。
 *
 * @note
 * - シティの生成時に 1 回だけ呼ぶこと。複数回呼ぶと建物のエンティティが重複する。
 * - 設定にない建物はエンティティ化しない。ロック中の建物も価格を見せるためにエンティティ化する（本体のコメントを参照）。
 */
/**
 * @brief 受信した MsgType に対して期待される MsgBody の union の型を返す
 *
 * @param type クライアントから届いた外側のメッセージ種別。
 *
 * @return 対応する union の型。
 * @retval Game::MsgBody_NONE サーバーが処理しない種別。呼び出し側は検証不要。
 *
 * @note
 * - サーバーが実際に処理する C2S メッセージだけを対象にしている。
 * - union の値は fbs での宣言順で決まる。フロントエンドとサーバーのどちらかが順序を変えて、
 *   もう一方が再生成していないと、body が別のテーブルとして解釈されてしまう。この表はそうしたずれを
 *   リクエストが黙って捨てられるのではなく、はっきりと表に出すためのもの。
 */
static Game::MsgBody expectedBodyType(Game::MsgType type) {
  switch (type) {
  case Game::MsgType_C2S_RequestEnterMap:  return Game::MsgBody_C2S_RequestEnterMap;
  case Game::MsgType_C2S_EnterMapReady:    return Game::MsgBody_C2S_EnterMapReady;
  case Game::MsgType_C2S_CameraUpdate:     return Game::MsgBody_C2S_CameraUpdate;
  case Game::MsgType_C2S_AssignShipToArea: return Game::MsgBody_C2S_AssignShipToArea;
  case Game::MsgType_C2S_Ping:             return Game::MsgBody_C2S_Ping;
  case Game::MsgType_C2S_RecruitUnit:      return Game::MsgBody_C2S_RecruitUnit;
  case Game::MsgType_C2S_TeamOperation:    return Game::MsgBody_C2S_TeamOperation;
  case Game::MsgType_C2S_PickupItem:       return Game::MsgBody_C2S_PickupItem;
  case Game::MsgType_C2S_EquipItem:        return Game::MsgBody_C2S_EquipItem;
  case Game::MsgType_C2S_UnequipItem:      return Game::MsgBody_C2S_UnequipItem;
  case Game::MsgType_C2S_ChallengeBoss:    return Game::MsgBody_C2S_ChallengeBoss;
  case Game::MsgType_C2S_BuyBuilding:      return Game::MsgBody_C2S_BuyBuilding;
  case Game::MsgType_C2S_ShopQueryList:    return Game::MsgBody_C2S_ShopQueryList;
  case Game::MsgType_C2S_ShopBuy:          return Game::MsgBody_C2S_ShopBuy;
  case Game::MsgType_C2S_ShopSell:         return Game::MsgBody_C2S_ShopSell;
  default:                                 return Game::MsgBody_NONE;
  }
}

/// 前方宣言：定義は下にあり、このファイルの前半にある検証用ヘルパーから使う。
MapInstance *tryGetLiveMapInstance(entt::registry &mainWorld,
                                   entt::entity instEntity);

/**
 * @brief その接続が、まだ生きているマップインスタンスの中にいるかを判定する
 *
 * @param mainWorld メイン world。マップインスタンスのコンポーネントを読むだけ。
 * @param connToInstance 接続からマップインスタンスへの索引。読み取りのみ。
 * @param connId 対象の接続。
 *
 * @retval false マップに入っていない、またはインスタンスが破棄済み。呼び出し側は、マップのコンテキストが必要な操作を拒否すること。
 *
 * @note
 * - 改ざん対策として使う。取引などの操作は正規のマップ入場の後でなければならず、
 *   プロトコルを直接組み立ててマップ入場を飛ばし、ゴールドやバッグを書き換えることを許さない。
 * - 判定するだけで、無効になった索引の削除は行わない。読み取り専用の検証で副作用を起こさないため。
 */
static bool sessionInLiveMap(
    entt::registry &mainWorld,
    const std::unordered_map<uint64_t, entt::entity> &connToInstance,
    uint64_t connId) {
  const auto it = connToInstance.find(connId);
  if (it == connToInstance.end()) return false;
  return tryGetLiveMapInstance(mainWorld, it->second) != nullptr;
}

static void spawnRoleBuildings(MapInstance &inst,
                               const buildings::RoleBuildings &rb,
                               const cache::BuildingCache &bc) {
    // ロック中の建物もエンティティを生成して送る。クライアントは state=0 + unlock_cost で
    // 「未解放 + 価格」を表示し、それをクリックすることで C2S_BuyBuilding を送れる。解放済みのものだけを送ると、
    // プレイヤーはロック中の建物を見ることができず、解放もできなくなる。
  for (const auto &[buildingId, tpl] : bc.buildings) {
    const buildings::RoleBuilding *const owned = buildings::find(rb, buildingId);

        // キャラクターのレコードがない = まだ解放されていないので、テンプレートのデフォルト座標を仮に使う。
    const float x = owned ? owned->x : tpl.defaultX;
    const float y = owned ? owned->y : tpl.defaultY;
    const uint32_t level = owned && owned->level > 0 ? owned->level
                                                     : tpl.baseLevel;
    const uint8_t state = owned ? owned->state : buildings::kStateLocked;

    const auto ent = inst.world.create();
    const uint32_t netId = inst.networkEntityIdAlloc.alloc();
    inst.world.emplace<NetworkId>(ent, NetworkId{netId});
    inst.world.emplace<CBuilding>(ent, CBuilding{
        .buildingId = buildingId,
        .networkEntityId = netId,
        .x = x,
        .y = y,
        .level = level,
        .state = state,
    });
  }

    // 設定テーブルにない建物が role_buildings にあるのはデータの不整合なので、警告だけ出してエンティティは作らない。
  for (const auto &e : rb.entries) {
    if (!cache::findBuilding(bc, e.buildingId)) {
      zone_log::warning("skip role building id=%u: not in buildings config",
                        e.buildingId);
    }
  }
}

std::vector<codec::BuildingStaticData>
collectBuildingSnapshot(entt::registry &world, const std::string &ownerRoleId,
                        const cache::BuildingCache &bc) {
  std::vector<codec::BuildingStaticData> result;
  auto view = world.view<NetworkId, CBuilding>();
  for (auto e : view) {
    auto &nid = view.get<NetworkId>(e);
    auto &bld = view.get<CBuilding>(e);
    codec::BuildingStaticData snap;
    snap.networkEntityId = nid.value;
        // buildings.id は building_id であり building_kind でもあるので、変換はしない。
        // HINT: 現在の fbs の BuildingKind は 4 までしか定義しておらず、5〜8 番の建物は列挙子の名前のない値として送られる。
        // フロントエンドは building_id で見た目の表を引くので kind の名前には依存しないが、
        // 将来 kind で分岐するなら、先に BuildingKind を 8 まで追加すること。
    snap.buildingId = bld.buildingId;
    snap.buildingKind = static_cast<Game::BuildingKind>(bld.buildingId);
    snap.ownerRoleId = ownerRoleId;
    snap.x = bld.x;
    snap.y = bld.y;
    snap.level = bld.level;
    snap.state = bld.state;
    if (const cache::BuildingTemplate *const tpl =
            cache::findBuilding(bc, bld.buildingId)) {
      snap.buildingName = tpl->buildingName;
      snap.unlockCost = static_cast<uint32_t>(tpl->unlockCost);
    }
    result.push_back(std::move(snap));
  }
  return result;
}

/**
 * @brief sub-world 内のすべてのユニットの静的スナップショットを集める
 *
 * @return 所有権を持つ vector。JoinMapSnapshot に使う。
 */
std::vector<codec::UnitStaticData>
collectStaticSnapshot(entt::registry &world, const cache::TemplateCache &tc) {
  std::vector<codec::UnitStaticData> result;
  auto view = world.view<NetworkId, UnitBase>();
  result.reserve(view.size_hint());
  for (auto e : view) {
    auto &nid  = view.get<NetworkId>(e);
    auto &base = view.get<UnitBase>(e);
    const auto *own = world.try_get<Owner>(e);  // Owner を持つのはプレイヤーの精霊だけ
    codec::UnitStaticData snap;
    snap.networkEntityId = nid.value;
    snap.ownerRoleId = own ? own->roleId : "";
    snap.campId      = base.campId;
    snap.unitId      = base.unitId;
    snap.unitKind    = static_cast<Game::UnitKind>(base.unitKind);
        // レベルは実行時のエンティティから取る。クライアントの頭上ラベルはそのまま表示し、テーブルを引き直さない。
    snap.level       = std::max<uint32_t>(1, base.level);
        // HP と攻撃力は実行時の戦闘コンポーネントから取る。レベル成長と装備ボーナスを含んだ、プレイヤーの本当の戦闘力。
        // テンプレートの基礎値は使えない。それはレベル 1・装備なしの数値なので、パネルの表示がずっと変わらなくなる。
    if (const auto *cb = world.try_get<UnitCombat>(e)) {
      snap.maxHp       = static_cast<uint32_t>(std::max(0, cb->max_hp));
      snap.attackPower = static_cast<uint32_t>(std::max(0, cb->attack_power));
    } else {
      snap.maxHp       = base.maxHp;
      snap.attackPower = 0;
    }

        // ユニット名を埋める
    auto it = tc.units.find(base.unitId);
    if (it != tc.units.end()) {
      std::string name = it->second.unitName;
            // プレイヤーの精霊かどうかは Owner の有無で判定する（プレイヤーの精霊は Owner を持つ）
      if (own != nullptr) {
        snap.unitName = "P." + name;  // プレイヤーの精霊には接頭辞を付ける
      } else {
        snap.unitName = name;  // モンスターはテンプレートの名前を使う
      }
    } else {
      snap.unitName = "不明";
    }

    result.push_back(std::move(snap));
  }
  return result;
}

/**
 * @brief 接続リストから指定の接続を取り除く
 *
 * @note
 * - 残りの接続の相対的な順序は保つ。
 */
void eraseConnId(std::vector<uint64_t> &connIds, uint64_t connId) {
  connIds.erase(std::remove(connIds.begin(), connIds.end(), connId),
                connIds.end());
}

/**
 * @brief 接続リストに重複しないように接続を追加する
 *
 * @note
 * - O(N) で重複を確認する。現在はインスタンスあたりの接続数がごく少ない。
 */
void pushConnIdUnique(std::vector<uint64_t> &connIds, uint64_t connId) {
  if (std::find(connIds.begin(), connIds.end(), connId) == connIds.end()) {
    connIds.push_back(connId);
  }
}

/**
 * @brief インスタンス上の、指定キャラクターの viewer を破棄する
 *
 * @return 破棄した viewer に対応する connId の一覧。
 */
std::vector<uint64_t>
destroyViewersForRole(entt::registry &mainWorld, MapInstance &inst,
                      const std::string &roleId) {
  std::vector<uint64_t> removedConnIds;
  std::vector<entt::entity> kept;
  kept.reserve(inst.viewerEntities.size());

  for (entt::entity viewerEntity : inst.viewerEntities) {
    if (!mainWorld.valid(viewerEntity)) {
      continue;
    }

    auto *viewer = mainWorld.try_get<MapViewer>(viewerEntity);
    if (viewer && viewer->roleId == roleId) {
      removedConnIds.push_back(viewer->connId);
      mainWorld.destroy(viewerEntity);
      continue;
    }

    kept.push_back(viewerEntity);
  }

  inst.viewerEntities = std::move(kept);
  return removedConnIds;
}

/**
 * @brief 指定した接続に対応する viewer を破棄する
 *
 * @note
 * - inst.viewerEntities からもエンティティのハンドルを同時に削除する。
 */
void destroyViewerForConn(entt::registry &mainWorld, MapInstance &inst,
                          uint64_t connId) {
  std::vector<entt::entity> kept;
  kept.reserve(inst.viewerEntities.size());

  for (entt::entity viewerEntity : inst.viewerEntities) {
    if (!mainWorld.valid(viewerEntity)) {
      continue;
    }

    auto *viewer = mainWorld.try_get<MapViewer>(viewerEntity);
    if (viewer && viewer->connId == connId) {
      mainWorld.destroy(viewerEntity);
      continue;
    }

    kept.push_back(viewerEntity);
  }

  inst.viewerEntities = std::move(kept);
}

/**
 * @brief インスタンス上のすべての viewer を破棄する
 *
 * @note
 * - インスタンスの GC に使う。呼び出し後、inst.viewerEntities は空になる。
 */
void destroyAllViewers(entt::registry &mainWorld, MapInstance &inst) {
  for (entt::entity viewerEntity : inst.viewerEntities) {
    if (mainWorld.valid(viewerEntity)) {
      mainWorld.destroy(viewerEntity);
    }
  }
  inst.viewerEntities.clear();
}

/**
 * @brief まだ有効な MapInstance コンポーネントを取得する
 *
 * @retval 非nullptr instEntity が現在有効で、MapInstance を持っている。
 * @retval nullptr エンティティが無効、またはコンポーネントが存在しない。
 */
MapInstance *tryGetLiveMapInstance(entt::registry &mainWorld,
                                   entt::entity instEntity) {
  if (!mainWorld.valid(instEntity) ||
      !mainWorld.all_of<MapInstance>(instEntity)) {
    return nullptr;
  }
  return &mainWorld.get<MapInstance>(instEntity);
}

/**
 * @brief sub-world にメインシティの固定モンスターのエンティティを読み込む
 *
 * @note
 * - TemplateCache::fixedSpawns[mapId] を走査し、city_monster_factory を再利用する。
 * - 実行時のリスポーンと初回のスポーンは同じ処理を通るので、モンスターの組み立て方が必ず一致する。
 */
void loadCityFixedEntities(entt::registry &world,
                           NetworkEntityIdAllocator &alloc,
                           const cache::TemplateCache &tc,
                           const uint32_t mapId) {
  auto it = tc.fixedSpawns.find(mapId);
  if (it == tc.fixedSpawns.end()) return;

  for (const auto &fs : it->second) {
    city_monster_factory::spawnAtPoint(world, alloc, tc,
                                       fs.spawnPointId, fs.unitId,
                                       fs.campId, mapId);
  }
}

/**
 * @brief ワールド座標が viewer のカメラのビューポート内にあるかを判定する
 *
 * @retval true 点が矩形のビューポートの内側または境界上にある。
 */
bool inViewport(const ViewerCamera &cam, float ex, float ey) {
  return ex >= cam.center.x - cam.halfWidth  &&
         ex <= cam.center.x + cam.halfWidth  &&
         ey >= cam.center.y - cam.halfHeight &&
         ey <= cam.center.y + cam.halfHeight;
}

/**
 * @brief ビューポート内のユニットの networkEntityId の集合を集める
 *
 * @return 所有権を持つ set。UnitBase を持つエンティティだけを含む。
 */
std::unordered_set<uint32_t>
collectVisibleNetworkEntityIds(entt::registry &world, const ViewerCamera &cam) {
  std::unordered_set<uint32_t> result;
  auto view = world.view<NetworkId, UnitBase>();
  for (auto e : view) {
    auto &base = view.get<UnitBase>(e);
    if (inViewport(cam, base.x, base.y)) {
      result.insert(view.get<NetworkId>(e).value);
    }
  }
  return result;
}

/**
 * @brief networkEntityId の集合に従って、ユニットの完全なスナップショットを集める
 *
 * @return 所有権を持つ vector。まだ存在していて UnitBase を持つエンティティだけを含む。
 */
std::vector<codec::UnitSnapshotData>
collectSnapshotByNetworkEntityIds(entt::registry &world,
                                  const std::unordered_set<uint32_t> &ids,
                                  const cache::TemplateCache &tc) {
  std::vector<codec::UnitSnapshotData> result;
  auto view = world.view<NetworkId, UnitBase>();
  for (auto e : view) {
    auto &nid = view.get<NetworkId>(e);
    if (ids.find(nid.value) == ids.end()) continue;
    auto &base = view.get<UnitBase>(e);
    const auto *vel = world.try_get<Velocity>(e);
    const auto *own = world.try_get<Owner>(e);  // Owner を持つのはプレイヤーの精霊だけ
    codec::UnitSnapshotData snap;
    snap.networkEntityId = nid.value;
    snap.ownerRoleId = own ? own->roleId : "";
    snap.campId      = base.campId;
    snap.unitId      = base.unitId;
    snap.unitKind    = static_cast<Game::UnitKind>(base.unitKind);
    snap.x           = base.x;
    snap.y           = base.y;
    snap.vx          = vel ? vel->vx : 0.f;
    snap.vy          = vel ? vel->vy : 0.f;
    snap.facing      = base.facing;
    snap.curHp       = base.curHp;
    snap.maxHp       = base.maxHp;
    snap.level       = std::max<uint32_t>(1, base.level);
    if (const auto *cb = world.try_get<UnitCombat>(e)) {
      snap.attackPower = static_cast<uint32_t>(std::max(0, cb->attack_power));
    }
    snap.lifeStatus  = static_cast<Game::LifeStatus>(base.lifeStatus);
    snap.actionState = static_cast<Game::ActionState>(base.actionState);

        // ユニット名を埋める
    auto it = tc.units.find(base.unitId);
    if (it != tc.units.end()) {
      std::string name = it->second.unitName;
      if (own != nullptr) {
        snap.unitName = "P." + name;  // プレイヤーの精霊には接頭辞を付ける
      } else {
        snap.unitName = name;  // モンスターはテンプレートの名前を使う
      }
    } else {
      snap.unitName = "不明";
    }

    result.push_back(std::move(snap));
  }
  return result;
}

/**
 * @brief ユニットの完全なスナップショットを、静的スナップショットに絞り込む
 *
 * @return UnitStaticData に必要なフィールドだけを含む。
 *
 * @note
 * - この tick の visibleSnaps にある entered の要素を、静的データに変換するのに使う。
 */
inline codec::UnitStaticData
snapshotToStatic(const codec::UnitSnapshotData &snap) {
  codec::UnitStaticData s;
  s.networkEntityId = snap.networkEntityId;
  s.ownerRoleId     = snap.ownerRoleId;
  s.campId          = snap.campId;
  s.unitId          = snap.unitId;
  s.unitKind        = snap.unitKind;
  s.unitName        = snap.unitName;
    // 以前はここで level が漏れていたため、実行中に視界に入った精霊の静的データのレベルが常に 1 になっていた。
    // 一方、マップ入場時のスナップショットの経路は正しかったので、同じ精霊のレベルが視界に出入りするたびに変わって見えていた。
  s.level           = std::max<uint32_t>(1, snap.level);
  s.maxHp           = snap.maxHp;
  s.attackPower     = snap.attackPower;
  return s;
}

} // namespace

/**
 * @brief ゲームロジックワーカーのメインループ
 *
 * @param zone_id 現在のゾーンの実行時 ID。ログと gateway との境界で使う。
 * @param cfg zone 設定。関数の開始時に DB/Redis/待ち受け関連の設定を読む。
 *
 * @note
 * - runtime_shutdown で終了が要求されるまで、呼び出したスレッドをブロックする。
 * - 関数は ProducerGuard を持ち、終了前に DB/Redis ワーカーがキューを処理しきれるようにする。
 */
void game::run(uint8_t zone_id, const zone::ZoneConfig &cfg) {
  runtime_shutdown::ProducerGuard producerGuard;

    // ── 起動時に設定テーブルのキャッシュを読み込む ──
  cache::TemplateCache tc;
  if (!cache::loadAll(cfg, tc)) {
    zone_log::error("game:%u FAILED to load template cache, exiting", zone_id);
    return;
  }

    // アイテムとショップの設定：読み込みに失敗した場合も、必ず起動を失敗させる。
    // そうしないとショップが黙って空のリストになり、クライアントには「サービスの異常」ではなく「商品がない」と見えてしまう。
  cache::ItemCache itemCache;
  if (!cache::loadItems(cfg, itemCache)) {
    zone_log::error("game:%u FAILED to load item cache, exiting", zone_id);
    return;
  }

    // 建物テンプレート：シティの生成時にこれで role_buildings を検証するので、読み込みに失敗したら必ず起動を失敗させる。
  cache::BuildingCache buildingCache;
    // レベル上限：起動時に env.zone から読み、その後は /api/reload-config によって tick の境界で差し替える。
  uint32_t maxUnitLevel = static_cast<uint32_t>(cfg.maxUnitLevel);
  if (!cache::loadBuildings(cfg, buildingCache)) {
    zone_log::error("game:%u FAILED to load building cache, exiting", zone_id);
    return;
  }

    // 地形の nav グリッドは建物を含まないので、そのまま使うと経路探索が建物を素通りしてしまう。ここで建物の形状を重ねる。
    //
    // 現在は全プレイヤーの建物の位置がテンプレートのデフォルト値と同じ（DB で実際に確認したところ、ずれている行は 0）なので、
    // サーバー全体でこの 1 つを共有すればよく、プレイヤーごとのコストはゼロ。
    // 将来プレイヤーが自由に配置できるようになったら、配置がデフォルトと異なるシティだけ別に作り、
    // そのシティの world の CCityNavGrid をそちらに向ければよい。経路探索のコードは変更不要。
  game::CityNavGrid defaultCityNavGrid;
  {
    buildings::RoleBuildings templateLayout;
    for (const auto &[bid, tpl] : buildingCache.buildings) {
      buildings::RoleBuilding e;
      e.buildingId = bid;
      e.x = tpl.defaultX;
      e.y = tpl.defaultY;
      e.level = tpl.baseLevel;
            // ロック中の建物も場所を占める。クライアントはそれらを描画しプレイヤーにも見えているので、
            // 経路探索もそれに合わせないと「見た目は壁にぶつかっているのに、ロジック上はすり抜ける」ことになる。
      e.state = buildings::kStateUnlocked;
      templateLayout.entries.push_back(e);
    }
    defaultCityNavGrid.build(game::mainCityNavGrid(), buildingCache,
                             templateLayout, /*blockLocked=*/true);
    zone_log::info("city nav grid built: %zu blocked cells (terrain + %zu buildings)",
                   defaultCityNavGrid.blockedCellCount(),
                   templateLayout.entries.size());
  }
    // ── メイン World：エンティティ 1 つ = マップインスタンス 1 つ ──
  entt::registry mainWorld;

    // 高速に引くための索引
  std::unordered_map<std::string, entt::entity> cityByRole;
  std::unordered_map<uint64_t, entt::entity> connToInstance;
  std::unordered_map<uint64_t, entt::entity> connToViewer;
  std::unordered_map<uint64_t, GameSession> sessions;
  std::unordered_map<std::string, uint64_t> roleToConn;
  std::unordered_map<entt::entity, MonsterAIEventHub> pendingAoiBattleEvents;

    // 早く届きすぎた InboundMsg の一時置き場：zone_http は SessionRegister を enqueue してから Accepted を send する。
    // 通常はクライアントの後続のメッセージが SessionRegister より先に来ることはないが、gSessionRegister と gInbound は
    // 独立した 2 本の MPMC キューであり、キューをまたいだ可視性は絶対には保証されない。また、将来の拡張でクライアントが
    // Accepted の前に複数のメッセージを続けて送ることもありうる。そこで保険として、gInbound にまだセッションが登録されていない
    // connId のメッセージが現れたら pendingInbound に一時保存し、次の tick でまず再投入してから処理する。
    // kMaxPendingInboundTicks を超えてもセッションが見つからない場合は、ログを出して破棄する。
  std::unordered_map<uint64_t, std::vector<InboundMsg>> pendingInbound;
  std::unordered_map<uint64_t, int> pendingInboundTicks;
  constexpr int kMaxPendingInboundTicks = 5;
  float cityGcElapsedSec = 0.f;

  const int tickRate = cfg.tickRate > 0 ? cfg.tickRate : 30;
  const uint64_t tickIntervalMs = 1000 / tickRate;
  const float tickDtSec = 1.f / static_cast<float>(tickRate);
  constexpr int kMaxCatchUpTicks = 5;
  uint64_t lastLoopTimeMs = timeutil::steady_now_ms();
  float simAccumulatorSec = tickDtSec;

  while (!runtime_shutdown::isShutdownRequested()) {
    auto tickStart = timeutil::steady_now_ms();
    const uint64_t elapsedLoopMs =
        tickStart >= lastLoopTimeMs ? (tickStart - lastLoopTimeMs) : 0;
    lastLoopTimeMs = tickStart;
    const uint64_t clampedElapsedMs = std::min<uint64_t>(elapsedLoopMs, 250);
    simAccumulatorSec += static_cast<float>(clampedElapsedMs) / 1000.f;

        // ── 0. 設定のホットリロード：どのシステムも動く前に、まとめて差し替える ─────────
        // ここに置くことで、同じフレームのすべてのシステムが同じ設定のスナップショットを見ることを保証する。
        // 新しい設定は管理 API のスレッドで読み込みと検証が済んでいるので、ここでは差し替えを 1 回行うだけ。
    config_reload::applyPending(zone_id, tc, itemCache, buildingCache,
                                maxUnitLevel);

        // ── 1. gSessionRegister を処理する：zone_http が登録した新しいセッション ─────
    {
      SessionRegister reg;
      while (gSessionRegister[zone_id].try_dequeue(reg)) {
        auto &sess = sessions[reg.connId];
        sess.connId = reg.connId;
        sess.roleId = reg.roleId;
        sess.passToken = reg.passToken;
        sess.roleDto = std::move(reg.roleDto);
        sess.isReconnect = reg.isReconnect;
        sess.inventory = std::move(reg.inventory);
        sess.equipment = std::move(reg.equipment);
        sess.buildings = std::move(reg.buildings);
        sess.ownedUnits = std::move(reg.ownedUnits);
        sess.hasPendingMap = false;
        sess.pendingMapId = 0;
        roleToConn[sess.roleId] = reg.connId;
      }
    }

        // ── 2. gInbound を処理する：ネットワークのメッセージ ──────────────────────────
        //
        // まず前回一時保存した pendingInbound のうち、セッションが用意できたものを再投入し、
        // 続けてこの tick の gInbound を順に追加して、まとめて 1 つのバッチとして処理する。
    std::vector<InboundMsg> inboundBatch;
    for (auto it = pendingInbound.begin(); it != pendingInbound.end();) {
      if (sessions.find(it->first) != sessions.end()) {
        for (auto &pm : it->second) {
          inboundBatch.push_back(std::move(pm));
        }
        pendingInboundTicks.erase(it->first);
        it = pendingInbound.erase(it);
      } else {
        int &ticks = pendingInboundTicks[it->first];
        if (++ticks > kMaxPendingInboundTicks) {
          pendingInboundTicks.erase(it->first);
          it = pendingInbound.erase(it);
        } else {
          ++it;
        }
      }
    }
    {
      InboundMsg dq;
      while (gInbound[zone_id].try_dequeue(dq)) {
        inboundBatch.push_back(std::move(dq));
      }
    }

    for (auto &msg : inboundBatch) {
      switch (msg.type) {

      case MsgType::Connect:
        break;

      case MsgType::Data: {
        auto sessIt = sessions.find(msg.connId);
        if (sessIt == sessions.end()) {
                    // セッションがまだ登録されていない：pendingInbound に一時保存し、次の tick でまず再投入してから処理する。
                    // 通常は zone_http が SessionRegister を enqueue してから Accepted を送っているので、
                    // ここに来るのは、たいていキューをまたいだ可視性がまだ揃っていない一瞬の間。
          const uint64_t connId = msg.connId;
          pendingInbound[connId].push_back(std::move(msg));
          break;
        }
        auto &sess = sessIt->second;

        const auto *env = codec::decode(msg.payload);
        if (!env) break;
        if (!sessionIsCurrentRoleConnection(roleToConn, sess, msg.connId)) {
          network::send(msg.connId,
              codec::encodeErrorMessage(env->seq(),
                  Game::ErrorType_Net_AuthFailed, "stale role connection"));
          break;
        }

                // プロトコルの整合性チェック：msg_type と body の union は一致していなければならない。
                // 一致しないのは、ほぼ確実に「片方が fbs を変えて、もう片方が再生成していない」ケース。
                // その場合 body_as_* が nullptr を返し、リクエストは黙って捨てられ、原因を追えなくなる。
        {
          const Game::MsgBody expected = expectedBodyType(env->msg_type());
          if (expected != Game::MsgBody_NONE &&
              env->body_type() != expected) {
            zone_log::warning(
                "protocol mismatch conn=%llu msg_type=%u body_type=%u "
                "expected_body=%u (client fbs out of sync?)",
                static_cast<unsigned long long>(msg.connId),
                static_cast<unsigned>(env->msg_type()),
                static_cast<unsigned>(env->body_type()),
                static_cast<unsigned>(expected));
            network::send(msg.connId,
                codec::encodeErrorMessage(env->seq(),
                    Game::ErrorType_Game_InvalidState,
                    "protocol version mismatch: please regenerate client schema"));
            break;
          }
        }

        switch (env->msg_type()) {

                // ── C2S_RequestEnterMap ──────────────────────────────
        case Game::MsgType_C2S_RequestEnterMap: {
          auto *m = env->body_as_C2S_RequestEnterMap();
          if (!m) break;

          std::string reqRoleId = m->role_id() ? m->role_id()->str() : "";
          uint8_t reqMapId = m->map_id();
          if (reqRoleId != sess.roleId) {
            network::send(msg.connId,
                codec::encodeErrorMessage(env->seq(),
                    Game::ErrorType_Net_AuthFailed, "role_id mismatch"));
            break;
          }

                    // passToken の検証
          std::string passToken = m->pass_token() ? m->pass_token()->str() : "";
          if (passToken.empty() || passToken != sess.passToken) {
            network::send(msg.connId, codec::encodeErrorMessage(env->seq(),
                Game::ErrorType_Net_AuthFailed, "passToken mismatch"));
            break;
          }

                    // map_id が存在することを確認する
          if (tc.maps.find(reqMapId) == tc.maps.end()) {
            network::send(msg.connId, codec::encodeErrorMessage(env->seq(),
                Game::ErrorType_Game_InvalidState, "invalid map_id"));
            break;
          }

          sessionSetPendingMap(sess, reqMapId);

                    // S2C_EnterMapAccepted を送る（クライアントはマップのリソースの読み込みを始める）
          network::send(msg.connId,
              codec::encodeEnterMapAccepted(env->seq(), true));

          break;
        }

                // ── C2S_EnterMapReady ────────────────────────────────
        case Game::MsgType_C2S_EnterMapReady: {
          auto *m = env->body_as_C2S_EnterMapReady();
          if (!m) break;

          std::string reqRoleId = m->role_id() ? m->role_id()->str() : "";
          if (reqRoleId != sess.roleId) {
            network::send(msg.connId,
                codec::encodeErrorMessage(env->seq(),
                    Game::ErrorType_Net_AuthFailed, "role_id mismatch"));
            break;
          }

                    // passToken の検証
          std::string reqPassToken = m->pass_token() ? m->pass_token()->str() : "";
          if (reqPassToken.empty() || reqPassToken != sess.passToken) {
            network::send(msg.connId, codec::encodeErrorMessage(env->seq(),
                Game::ErrorType_Net_AuthFailed, "passToken mismatch"));
            break;
          }

          uint32_t mapId = 0;
          if (!sessionConsumePendingMap(sess, mapId)) {
            network::send(msg.connId, codec::encodeErrorMessage(env->seq(),
                Game::ErrorType_Game_InvalidState,
                "EnterMapReady without pending map"));
            break;
          }

                    // マップのテンプレートを引く。Ready で使えるのは、今回の RequestEnterMap で検証済みの pending map だけ。
          auto mapIt = tc.maps.find(mapId);
          if (mapIt == tc.maps.end()) {
            network::send(msg.connId, codec::encodeErrorMessage(env->seq(),
                Game::ErrorType_Game_InvalidState, "invalid pending map_id"));
            break;
          }
          cache::MapTemplate mapTpl = mapIt->second;
          sess.roleDto.currentMapId = static_cast<uint8_t>(mapId);

                    // ── メインシティのインスタンスを探す（なければ作る）──
          entt::entity instEntity;
          auto cityIt = cityByRole.find(sess.roleId);
          const bool hadExistingCity = cityIt != cityByRole.end();

          if (hadExistingCity) {
            instEntity = cityIt->second;
                        // プレイヤーが戻ってきたので、アイドルの印を取り消す
            auto &instReconnect = mainWorld.get<MapInstance>(instEntity);
            setMapSimTickRate(instReconnect.simClock,
                              static_cast<uint32_t>(tickRate));
            instReconnect.emptyTimestampMs = 0;
          } else {
            instEntity = mainWorld.create();
            mainWorld.emplace<MapInstance>(instEntity, MapInstance{
                .mapId = mapTpl.mapId,
                .mapMode = static_cast<MapMode>(mapTpl.mapMode),
                .maxPlayers = mapTpl.maxPlayers,
                .connIds = {},
            });
            mainWorld.emplace<Owner>(instEntity, Owner{sess.roleId});
            mainWorld.emplace<CityData>(instEntity, CityData{
                .cityLevel = 1, // TODO: DB の role_cities テーブルから読み込む
            });

                        // メインシティのエリアの解放状態を初期化する（プレイヤーの初期状態ではメインシティと森が解放済み）
            CityAreas cityAreas;
            cityAreas.areas[0].unlocked = true;   // メインシティ
            cityAreas.areas[1].unlocked = true;   // 森
            cityAreas.areas[2].unlocked = false;  // 戦場（戦闘力 1000 が必要）
            cityAreas.areas[3].unlocked = false;  // 墓地（戦闘力 2000 が必要）
            mainWorld.emplace<CityAreas>(instEntity, cityAreas);

            auto &inst = mainWorld.get<MapInstance>(instEntity);
            setMapSimTickRate(inst.simClock, static_cast<uint32_t>(tickRate));
            loadCityFixedEntities(inst.world, inst.networkEntityIdAlloc, tc,
                                  mapTpl.mapId);

                        // ── 戦闘サブシステムの ctx シングルトンを初期化する ──
            inst.world.ctx().emplace<MonsterAIEventHub>();
                        // このシティが使う nav グリッド：現在はサーバー全体で、建物を含む同じグリッドを共有している。
            inst.world.ctx().emplace<game::CCityNavGrid>(
                game::CCityNavGrid{&defaultCityNavGrid.view()});
            area_registry_system::initialize(inst.world);

                        // ── role_buildings に従って、このキャラクターの建物を生成する ──
                        // 建物は静的なインタラクション対象で、AOI の差分配信には乗らない。JoinMapSnapshot で全件送り、
                        // その後の変化は S2C_BuildingChanged で送る。
            spawnRoleBuildings(inst, sess.buildings, buildingCache);

            cityByRole[sess.roleId] = instEntity;
          }

          auto &inst = mainWorld.get<MapInstance>(instEntity);
          const auto teamEnterResult = game::preparePlayerTeamUnitsForEnterMap(
              inst.world, inst.networkEntityIdAlloc, sess.roleId,
              sess.roleDto, tc, itemCache, sess.equipment, sess.ownedUnits,
              mapTpl.respawn.x, mapTpl.respawn.y,
              sess.isReconnect && hadExistingCity, maxUnitLevel);

          if (!teamEnterResult.removedNetworkEntityIds.empty()) {
            const uint64_t snapshotTick = inst.aoiClock.tick;
            const uint64_t snapshotTimeMs = snapshotTimeMsForInstance(inst);
            for (uint64_t cid : inst.connIds) {
              if (cid == msg.connId) continue;
              network::send(cid,
                  codec::encodeUpdateAoiSnapshot(
                      0, snapshotTick, snapshotTimeMs,
                      /*enteredIds=*/{}, /*enteredUnits=*/{},
                      /*updated=*/{},
                      teamEnterResult.removedNetworkEntityIds));
            }
          }

                    // このインスタンスの全エンティティのスナップショットを集める（カメラを作る前に行い、カメラを含めないようにする）
          auto allUnits = collectStaticSnapshot(inst.world, tc);
          auto allBuildings =
              collectBuildingSnapshot(inst.world, sess.roleId, buildingCache);
          ViewerCamera initCam;
          initCam.center.x = mapTpl.respawn.x;
          initCam.center.y = mapTpl.respawn.y;
          initCam.halfWidth = 500.f;   // ViewerCamera のデフォルト値と同じ
          initCam.halfHeight = 1000.f;
          auto initialVisibleIds = collectVisibleNetworkEntityIds(inst.world, initCam);
          auto initialVisibleSnaps =
              collectSnapshotByNetworkEntityIds(inst.world, initialVisibleIds, tc);
          codec::UnitDynamicDataSoA initialDynamics;
          initialDynamics.reserve(initialVisibleSnaps.size());
          for (auto &snap : initialVisibleSnaps) {
            codec::UnitDynamicData d;
            d.networkEntityId = snap.networkEntityId;
            d.x = snap.x;
            d.y = snap.y;
            d.vx = snap.vx;
            d.vy = snap.vy;
            d.facing = snap.facing;
            d.curHp = snap.curHp;
            d.maxHp = snap.maxHp;
            d.lifeStatus = snap.lifeStatus;
            d.actionState = snap.actionState;
            initialDynamics.push(d);
          }

                    // マップ入場用の roleDto を組み立てる
          codec::RoleInfoDto mapRoleDto = sess.roleDto;
          mapRoleDto.currentMapId = static_cast<uint8_t>(inst.mapId);
          const uint64_t snapshotTick = inst.aoiClock.tick;
          const uint64_t snapshotTimeMs = snapshotTimeMsForInstance(inst);

                    // S2C_JoinMapSnapshot を送る
          network::send(msg.connId,
              codec::encodeJoinMapSnapshot(
                  env->seq(), snapshotTick, snapshotTimeMs, mapRoleDto, allUnits, allBuildings));

                    // 続けてすぐに UpdateAoiSnapshot を送り、最初に見えているすべてのエンティティを entered の経路で配信する。
                    // クライアントとの取り決め：動的なノードは UpdateAoi.entered の経路でしか作らない（Join で (0,0) にノードを
                    // 作ってから座標を同期すると残像が出るため）。そのため最初のバッチは enteredIds=visibleIds でなければならず、
                    // そうしないとクライアントはマップに入った直後にプレイヤー船もモンスターも何も見えない。静的データは S2C_JoinMapSnapshot で
                    // 送って aoiGlobalCache にキャッシュ済みなので、ここでは enteredUnits を重ねて送らない。
          {
                        // snapshotTimeMs は JoinMapSnapshot と揃える。どちらも同じ tick の状態のスナップショットで、
                        // 意味としてももともと同時刻。クライアントの onJoinMapSnapshot は AOI の適用済み水位を進めないので、
                        // 同じタイムスタンプのこの UpdateAoi を古いものと誤判定することはない（city_manager.onJoinMapSnapshot を参照）。
                        // codec は vector を必要とし、visibleIds は unordered_set なので、ここで vector に変換する。
            std::vector<uint32_t> enteredIdVec(initialVisibleIds.begin(),
                                               initialVisibleIds.end());
            network::send(msg.connId,
                codec::encodeUpdateAoiSnapshot(
                    env->seq(), snapshotTick, snapshotTimeMs,
                    enteredIdVec, /*enteredUnits=*/{},
                    initialDynamics, /*leftIds=*/{}));
          }

                    // 接続をインスタンスに登録する。閲覧者の状態はメイン World に置き、inst.world のゲームオブジェクトの集合を汚さないようにする。
          auto removedViewerConnIds =
              destroyViewersForRole(mainWorld, inst, sess.roleId);
          for (uint64_t oldConnId : removedViewerConnIds) {
            connToViewer.erase(oldConnId);
            if (oldConnId != msg.connId) {
              connToInstance.erase(oldConnId);
            }
            eraseConnId(inst.connIds, oldConnId);
          }
          pushConnIdUnique(inst.connIds, msg.connId);
          connToInstance[msg.connId] = instEntity;

                    // S2C_EnterMapDone を送る
          network::send(msg.connId,
              codec::encodeEnterMapDone(env->seq(), true));

                    // ── マップ入場後の 1 回限りのデータ同期 ──────────────────────────
                    // 順序とメッセージの組み合わせは Java 版の EnterMapSystem に合わせている：
                    // 募集リスト → バッグ → プレイヤー情報パネル → ボスリスト。
                    // いずれも全件のスナップショットで、その後はそれぞれ対応する変更メッセージで差分を送る。
          network::send(msg.connId,
              codec::encodeRecruitUnitList(env->seq(), tc, sess.ownedUnits));
          network::send(msg.connId,
              codec::encodeBackpackChanged(env->seq(), itemCache,
                                           sess.inventory));
          network::send(msg.connId,
              codec::encodePlayerProfile(env->seq(), sess.roleDto));
          network::send(msg.connId, codec::encodeBossList(env->seq(), tc));

                    // メインシティモード：メイン World に閲覧者と最初の可視集合を作る
          if (mapTpl.mapMode == MapMode::City) {
            auto viewerEntity = mainWorld.create();
            mainWorld.emplace<MapViewer>(viewerEntity, MapViewer{
                .mapInstance = instEntity,
                .roleId = sess.roleId,
                .connId = msg.connId,
            });
            mainWorld.emplace<ViewerCamera>(viewerEntity, ViewerCamera{
                .center = { mapTpl.respawn.x, mapTpl.respawn.y },
            });

                        // 最初の可視集合は、最初のパケットで実際に entered として送ったエンティティと一致していなければならない。
                        // JoinMapSnapshot は静的キャッシュを全件運ぶが、それは viewer がすべてのエンティティを見たことを意味しない。
            ViewerAoiState vis;
            const uint64_t nowMs = timeutil::steady_now_ms();
            vis.reserve(initialVisibleSnaps.size());
            for (const auto &snap : initialVisibleSnaps) {
              VisibleUnitState vs = buildVisibleUnitState(snap);
              const MotionSourceProbe motion =
                  collectMotionSourceProbe(inst.world, snap.networkEntityId);
              const PushProbe probe = buildPushProbe(snap, vs);
              if (isMotionActive(probe, motion)) {
                vs.cadence = kCadenceActive;
                vs.cooldownTicks = 0;
                vs.wasMoving = true;
              } else {
                vs.cadence = kCadenceSteady;
                vs.cooldownTicks = kCadenceSteady;
                vs.wasMoving = false;
              }
              vs.lastPushAtMs = nowMs;
              vs.lastPushTick = snapshotTick;
              vis.pushVisible(snap.networkEntityId, vs);
            }
            vis.rebuildIndex();
            mainWorld.emplace<ViewerAoiState>(viewerEntity, std::move(vis));
            inst.viewerEntities.push_back(viewerEntity);
            connToViewer[msg.connId] = viewerEntity;
          }

          sess.isReconnect = false;
          break;
        }

        case Game::MsgType_C2S_CameraUpdate: {
          auto *m = env->body_as_C2S_CameraUpdate();
          if (!m || !m->position()) break;

          auto viewerIt = connToViewer.find(msg.connId);
          if (viewerIt == connToViewer.end()) break;
          entt::entity viewerEntity = viewerIt->second;
          if (!mainWorld.valid(viewerEntity) ||
              !mainWorld.all_of<MapViewer, ViewerCamera>(viewerEntity)) {
            connToViewer.erase(viewerIt);
            break;
          }

          auto &viewer = mainWorld.get<MapViewer>(viewerEntity);
          if (viewer.roleId != sess.roleId ||
              !mainWorld.valid(viewer.mapInstance)) {
            break;
          }

          auto &cam = mainWorld.get<ViewerCamera>(viewerEntity);
                    // TODO: std::clamp でマップの境界内に制限する
          cam.center.x = m->position()->x();
          cam.center.y = m->position()->y();
          cam.dirty = true;
          break;
        }

        case Game::MsgType_C2S_AssignShipToArea: {
          auto *m = env->body_as_C2S_AssignShipToArea();
          if (!m) break;
          const uint32_t areaId = static_cast<uint32_t>(m->area_id());
          if (areaId < 1 || areaId > 4) {
            network::send(msg.connId, codec::encodeErrorMessage(env->seq(),
                Game::ErrorType_Game_InvalidState, "bad area_id"));
            break;
          }
          auto instIt = connToInstance.find(msg.connId);
          if (instIt == connToInstance.end()) break;
          MapInstance *instPtr = tryGetLiveMapInstance(mainWorld, instIt->second);
          if (!instPtr) {
            connToInstance.erase(instIt);
            break;
          }
          const auto *cityAreas = mainWorld.try_get<CityAreas>(instIt->second);
          if (!cityAreas || !cityAreaIsUnlocked(*cityAreas, areaId)) {
            network::send(msg.connId, codec::encodeErrorMessage(env->seq(),
                Game::ErrorType_Game_InvalidState, "area locked"));
            break;
          }
          auto &inst = *instPtr;
          auto &hub = inst.world.ctx().get<MonsterAIEventHub>();
          const uint64_t nowMs = snapshotTimeMsForInstance(inst);

          player_ship_ai_system::assignShipsToArea(
              inst.world, hub, sess.roleId, areaId, nowMs, inst.mapId,
              team_ops::deployedUnitIds(sess.roleDto));

          auto ownerView =
              inst.world.view<Owner, UnitBase, CPlayerShipAssignment>();
          for (auto e : ownerView) {
            const auto &owner  = ownerView.get<Owner>(e);
            if (owner.roleId != sess.roleId) continue;
            const auto &base   = ownerView.get<UnitBase>(e);
            const auto &assign = ownerView.get<CPlayerShipAssignment>(e);
            if (base.unitId == 0) continue;
            player_ship_db_writer::enqueueShipArea(
                sess.roleId, base.unitId, assign.areaId);
          }
          break;
        }

                // ── ボスへの挑戦 ───────────────────────────────────────
                // ボスとは unit_templates の camp_id=2 のテンプレートのことで、専用の設定テーブルはない。
        case Game::MsgType_C2S_ChallengeBoss: {
          const auto *m = env->body_as_C2S_ChallengeBoss();
          if (!m) break;
          const uint32_t bossId = m->boss_id();

          auto rejectBoss = [&](const std::string &reason) {
            network::send(msg.connId,
                codec::encodeOperationResult(
                    env->seq(), Game::OpResultType_BossChallenge, false,
                    reason));
          };

          auto bossInstIt = connToInstance.find(msg.connId);
          MapInstance *const bossInst =
              bossInstIt == connToInstance.end()
                  ? nullptr
                  : tryGetLiveMapInstance(mainWorld, bossInstIt->second);
          if (!bossInst) {
            if (bossInstIt != connToInstance.end()) {
              connToInstance.erase(bossInstIt);
            }
            rejectBoss("現在マップにいません");
            break;
          }

          std::vector<uint32_t> clearedIds;
          uint32_t bossAreaId = 0;
          const auto err = boss_challenge_system::startChallenge(
              *bossInst, tc, bossId, clearedIds, bossAreaId);

          using BossErr = boss_challenge_system::ChallengeError;
          if (err != BossErr::None) {
            rejectBoss(err == BossErr::UnknownBoss  ? "ボスが存在しません"
                       : err == BossErr::NotABoss   ? "このユニットはボスではありません"
                       : err == BossErr::BossAlive  ? "既にボスが出現しています。先に倒してください"
                                                    : "このマップにはボスの出現地点がありません");
            break;
          }

                    // 片付けたモンスターは AOI の left の経路で回収する必要がある。そうしないとクライアントにノードが残る。
          if (!clearedIds.empty()) {
            const uint64_t bossNowMs = snapshotTimeMsForInstance(*bossInst);
            for (uint64_t cid : bossInst->connIds) {
              network::send(cid,
                  codec::encodeUpdateAoiSnapshot(
                      0, bossInst->aoiClock.tick, bossNowMs,
                      /*enteredIds=*/{}, /*enteredUnits=*/{},
                      /*updated=*/{}, clearedIds));
            }
          }

                    // プレイヤーの編成をボスのいるエリアへ割り当てる。既存のエリア割り当ての仕組みをそのまま使い、
                    // ボス戦のために別の AI の行き先変更の処理を書くことはしない。
          if (bossAreaId != 0) {
            auto &bossHub = bossInst->world.ctx().get<MonsterAIEventHub>();
            player_ship_ai_system::assignShipsToArea(
                bossInst->world, bossHub, sess.roleId, bossAreaId,
                snapshotTimeMsForInstance(*bossInst), bossInst->mapId,
                team_ops::deployedUnitIds(sess.roleDto));
            for (auto e : bossInst->world.view<Owner, UnitBase,
                                               CPlayerShipAssignment>()) {
              const auto &owner = bossInst->world.get<Owner>(e);
              if (owner.roleId != sess.roleId) continue;
              const auto &base = bossInst->world.get<UnitBase>(e);
              const auto &assign =
                  bossInst->world.get<CPlayerShipAssignment>(e);
              if (base.unitId == 0) continue;
              player_ship_db_writer::enqueueShipArea(sess.roleId, base.unitId,
                                                     assign.areaId);
            }
          }

          const auto bossTplIt = tc.units.find(bossId);
          network::send(msg.connId,
              codec::encodeOperationResult(
                  env->seq(), Game::OpResultType_BossChallenge, true,
                  "「" + (bossTplIt != tc.units.end()
                              ? bossTplIt->second.unitName
                              : std::string("Boss")) +
                      "」が出現しました。編成が戦場へ向かっています！"));
          break;
        }

                // ── 地面のドロップアイテムを拾う ──────────────────────────────────
                // entity_id はドロップアイテムの network_entity_id（このマップインスタンス内でのプロトコル上の主キー）。
        case Game::MsgType_C2S_PickupItem: {
          const auto *m = env->body_as_C2S_PickupItem();
          if (!m) break;
          const uint32_t targetNetId = m->entity_id();

          auto rejectPickup = [&](const std::string &reason) {
            network::send(msg.connId,
                codec::encodeOperationResult(
                    env->seq(), Game::OpResultType_ItemPickup, false, reason));
          };

          auto pickInstIt = connToInstance.find(msg.connId);
          MapInstance *const pickInst =
              pickInstIt == connToInstance.end()
                  ? nullptr
                  : tryGetLiveMapInstance(mainWorld, pickInstIt->second);
          if (!pickInst) {
            if (pickInstIt != connToInstance.end()) {
              connToInstance.erase(pickInstIt);
            }
            rejectPickup("現在マップにいません");
            break;
          }

                    // プロトコル ID でドロップのエンティティを探す。CPickableItem を持たないエンティティは一律に拾えない。
          entt::entity dropEntity = entt::null;
          auto dropView = pickInst->world.view<NetworkId, CPickableItem>();
          for (auto e : dropView) {
            if (dropView.get<NetworkId>(e).value != targetNetId) continue;
            dropEntity = e;
            break;
          }
          if (dropEntity == entt::null) {
            rejectPickup("アイテムは既に消えています");
            break;
          }

          const auto &pick = pickInst->world.get<CPickableItem>(dropEntity);
          const uint64_t nowPickMs = snapshotTimeMsForInstance(*pickInst);
          if (nowPickMs > pick.expireAtMs) {
            rejectPickup("アイテムは期限切れで消えました");
            break;
          }
          if (!cache::findItem(itemCache, pick.itemId)) {
            rejectPickup("アイテムの設定に異常があります");
            break;
          }

          uint32_t pickedNewCount = 0;
          if (!inventory::addItems(sess.inventory, pick.itemId, pick.count,
                                   pickedNewCount)) {
            rejectPickup("バッグがいっぱいのため、拾えません");
            break;
          }

          const uint32_t pickedItemId = pick.itemId;
          const uint32_t pickedCount = pick.count;

                    // メモリ上のバッグはもう変更したので、エンティティは同じフレームで必ず破棄する。そうしないと同じドロップを何度も拾えてしまう。
          pickInst->world.destroy(dropEntity);

          role_economy_db_writer::enqueueItemCount(sess.roleId, pickedItemId,
                                                   pickedNewCount);

                    // ドロップアイテムが消えたことは、拾った人だけでなく、そのインスタンスの全接続に配信する。
          {
            const std::vector<uint32_t> removed{targetNetId};
            for (uint64_t cid : pickInst->connIds) {
              network::send(cid,
                  codec::encodeUpdateAoiSnapshot(
                      0, pickInst->aoiClock.tick, nowPickMs,
                      /*enteredIds=*/{}, /*enteredUnits=*/{},
                      /*updated=*/{}, removed));
            }
          }

          const cache::ItemTemplate *const pickedTpl =
              cache::findItem(itemCache, pickedItemId);
          network::send(msg.connId,
              codec::encodeOperationResult(
                  env->seq(), Game::OpResultType_ItemPickup, true,
                  "「" + (pickedTpl ? pickedTpl->itemName : "アイテム") +
                      "」x" + std::to_string(pickedCount) + " を入手しました"));
          network::send(msg.connId,
              codec::encodeBackpackChanged(env->seq(), itemCache,
                                           sess.inventory));
          break;
        }

                // ── 精霊の募集 ────────────────────────────────────────
                // コスト = unit_templates.base_hp * kRecruitCostHpMultiplier（テーブルに cost 列はない）。
                // 募集では「所有関係」を作って role_units に保存する。新しい精霊はすぐにマップ上に生成されるが（下記参照）、
                // 戦闘に出すには C2S_TeamOperation で現在の出撃編成に入れる必要がある。
        case Game::MsgType_C2S_RecruitUnit: {
          const auto *m = env->body_as_C2S_RecruitUnit();
          if (!m) break;
          const uint32_t unitId = m->unit_id();

          auto rejectRecruit = [&](const std::string &reason) {
            network::send(msg.connId,
                codec::encodeOperationResult(
                    env->seq(), Game::OpResultType_Recruit, false, reason));
          };

          const auto tplIt = tc.units.find(unitId);
          if (tplIt == tc.units.end()) {
            rejectRecruit("精霊のテンプレートが存在しません");
            break;
          }
          const auto &tpl = tplIt->second;
          if (tpl.campId != codec::camp::kRecruitable) {
            rejectRecruit("このユニットは募集できません");
            break;
          }
          if (codec::ownsUnit(sess.ownedUnits, unitId)) {
            rejectRecruit("「" + tpl.unitName + "」は既に所持しています！");
            break;
          }

          const uint64_t cost = codec::recruitCostOf(tpl);
          if (cost > static_cast<uint64_t>(sess.roleDto.gold)) {
            rejectRecruit("ゴールドが足りません（必要 " + std::to_string(cost) +
                          "、所持 " + std::to_string(sess.roleDto.gold) +
                          "）");
            break;
          }

          const uint32_t goldBeforeRecruit = sess.roleDto.gold;
          sess.roleDto.gold -= static_cast<uint32_t>(cost);
          codec::TeamUnitInfoDto recruited{};
          recruited.unitId = unitId;
          recruited.runtimeLevel = 1;
          recruited.runtimeExp = 0;
          recruited.runtimeAreaId = player_ship_constants::kDefaultAreaId;
          recruited.hasRuntimePersistence = true;
          sess.ownedUnits.push_back(recruited);
          std::sort(sess.ownedUnits.begin(), sess.ownedUnits.end(),
                    [](const codec::TeamUnitInfoDto &a,
                       const codec::TeamUnitInfoDto &b) {
                      return a.unitId < b.unitId;
                    });

          role_economy_db_writer::enqueueGold(sess.roleId, sess.roleDto.gold);
          role_economy_db_writer::enqueueRoleUnit(
              sess.roleId, unitId, tpl.baseHp, tpl.baseAtk, tpl.baseDef,
              static_cast<uint32_t>(tpl.baseSpeed), tpl.attackRange);

          network::send(msg.connId,
              codec::encodeOperationResult(
                  env->seq(), Game::OpResultType_Recruit, true,
                  "募集に成功しました！「" + tpl.unitName + "」を入手。残りゴールド：" +
                      std::to_string(sess.roleDto.gold)));
          network::send(msg.connId,
              codec::encodeGoldChanged(env->seq(), goldBeforeRecruit,
                                       sess.roleDto.gold,
                                       codec::gold_reason::kRecruit));
                    // 新しい精霊はすぐにマップ上に出現させる必要がある。フロントエンドの「マイ精霊」一覧はマップ上の
                    // PlayerUnit エンティティそのものなので、その場にいなければ、いつまでも選んで出撃させることができない。
                    // この 1 体だけを生成し、全体を作り直さない。他の精霊のプロトコル ID が変わらないようにするため。
          {
            auto recInstIt = connToInstance.find(msg.connId);
            MapInstance *const rip =
                recInstIt == connToInstance.end()
                    ? nullptr
                    : tryGetLiveMapInstance(mainWorld, recInstIt->second);
            if (rip) {
              Position recRespawn{0.f, 0.f};
              if (const auto mtIt = tc.maps.find(rip->mapId);
                  mtIt != tc.maps.end()) {
                recRespawn = mtIt->second.respawn;
              }
                            // 募集しても自動では編成に入らないので、新しい精霊は通常は待機状態になる。
                            // それでも実際の編成で判定しておく。将来募集の流れが自動出撃に変わっても、ここがずれないようにするため。
              const auto recDeployed = team_ops::deployedUnitIds(sess.roleDto);
              const bool recIsDeployed =
                  std::find(recDeployed.begin(), recDeployed.end(),
                            recruited.unitId) != recDeployed.end();
              game::spawnSinglePlayerUnit(
                  rip->world, rip->networkEntityIdAlloc, sess.roleId, recruited,
                  tc, itemCache, sess.equipment, recRespawn.x, recRespawn.y,
                  recIsDeployed, maxUnitLevel);
            }
          }

                    // owned フラグが変わったので、リストを送り直してボタンの状態をすぐに正しくする。
          network::send(msg.connId,
              codec::encodeRecruitUnitList(env->seq(), tc, sess.ownedUnits));
          break;
        }

                // ── 編成の操作（出撃 / 待機 / 出撃する編成の切り替え）─────────────
        case Game::MsgType_C2S_TeamOperation: {
          const auto *m = env->body_as_C2S_TeamOperation();
          if (!m) break;

          const Game::TeamOp op = m->op();
          const uint8_t teamId = m->team_id();
          const uint8_t slotIndex = m->slot_index();
          const uint32_t unitId = m->unit_id();

          const Game::OpResultType opType =
              op == Game::TeamOp_Deploy     ? Game::OpResultType_TeamDeploy
              : op == Game::TeamOp_Undeploy ? Game::OpResultType_TeamUndeploy
                                            : Game::OpResultType_TeamSwitch;

          auto rejectTeam = [&](const std::string &reason) {
            network::send(msg.connId,
                codec::encodeOperationResult(env->seq(), opType, false, reason));
          };

          if (teamId < team_ops::kMinTeamId || teamId > team_ops::kMaxTeamId) {
            rejectTeam("編成番号が無効です");
            break;
          }

          bool ok = false;
          uint32_t undeployedUnitId = 0;
          switch (op) {
          case Game::TeamOp_Deploy:
            if (slotIndex >= team_ops::kMaxSlotCount) {
              rejectTeam("スロットが無効です");
            } else if (!codec::ownsUnit(sess.ownedUnits, unitId)) {
              rejectTeam("その精霊はまだ所持していません");
            } else {
              ok = team_ops::deploy(sess, teamId, slotIndex, unitId);
              if (!ok) rejectTeam("出撃に失敗しました");
            }
            break;
          case Game::TeamOp_Undeploy:
            if (slotIndex >= team_ops::kMaxSlotCount) {
              rejectTeam("スロットが無効です");
            } else {
                            // 外す精霊を先に記録しておく。undeploy の後では見つけられないため。
              undeployedUnitId = team_ops::unitAtSlot(sess, teamId, slotIndex);
              ok = team_ops::undeploy(sess, teamId, slotIndex);
              if (!ok) rejectTeam("このスロットはもともと空です");
            }
            break;
          case Game::TeamOp_SwitchTeam:
            ok = team_ops::switchTeam(sess, teamId);
            if (!ok) rejectTeam("既に出撃中の編成です");
            break;
          }
          if (!ok) break;

                    // 編成の変更では精霊の行き先を変えるだけで、エンティティを破棄して作り直すことはしない。
                    // 作り直すとプロトコル上のエンティティ ID が変わり、クライアントの静的キャッシュに古いエントリが溜まり続けて、
                    // 「マイ精霊」一覧に同じ精霊が何度も表示されてしまう。
                    //
                    // 出撃・待機・編成の切り替えで実現したいことは同じ ―「現在の出撃編成は戦闘エリアへ、
                    // それ以外の精霊はメインシティへ」。そのため、すべて assignShipsToArea に統一し、個別に実装しない。
          {
            auto teamInstIt = connToInstance.find(msg.connId);
            MapInstance *const tip =
                teamInstIt == connToInstance.end()
                    ? nullptr
                    : tryGetLiveMapInstance(mainWorld, teamInstIt->second);
            if (tip) {
              auto &teamHub = tip->world.ctx().get<MonsterAIEventHub>();
              const uint64_t teamNowMs = snapshotTimeMsForInstance(*tip);

                            // 現在の戦闘エリア = そのキャラクターの精霊のうち、メインシティにいない精霊がいるエリア。
                            // 待機中の精霊は常にメインシティにいる（この関数が保証する不変条件）ので、
                            // ここで見つかるメインシティ以外のエリアは、必ず前回出撃した精霊のもの。
              uint32_t combatAreaId = player_ship_constants::kDefaultAreaId;
              for (auto e : tip->world.view<Owner, CPlayerShipAssignment>()) {
                if (tip->world.get<Owner>(e).roleId != sess.roleId) continue;
                const auto &as = tip->world.get<CPlayerShipAssignment>(e);
                if (as.areaId != player_ship_constants::kDefaultAreaId) {
                  combatAreaId = as.areaId;
                  break;
                }
              }

              player_ship_ai_system::assignShipsToArea(
                  tip->world, teamHub, sess.roleId, combatAreaId, teamNowMs,
                  tip->mapId, team_ops::deployedUnitIds(sess.roleDto));

                            // エリアの変更は DB に保存する。そのキャラクターの全精霊を走査し、実際の結果に従ってタスクを投入する。
              for (auto e : tip->world.view<Owner, UnitBase,
                                            CPlayerShipAssignment>()) {
                const auto &owner = tip->world.get<Owner>(e);
                if (owner.roleId != sess.roleId) continue;
                const auto &base = tip->world.get<UnitBase>(e);
                if (base.unitId == 0) continue;
                const auto &as = tip->world.get<CPlayerShipAssignment>(e);
                player_ship_db_writer::enqueueShipArea(sess.roleId, base.unitId,
                                                       as.areaId);
              }
            }
          }

          network::send(msg.connId,
              codec::encodeOperationResultWithData(
                  env->seq(), opType, true, "編成を更新しました",
                  team_ops::teamsToJson(sess.roleDto)));
          network::send(msg.connId,
              codec::encodePlayerProfile(env->seq(), sess.roleDto));
          break;
        }

                // ── 建物：解放／購入 ─────────────────────────────────
                // フロントエンドは楽観的にゴールドを減らして表示し、失敗したら OperationResult に従って元に戻す。
                // そのため失敗した場合も、必ず明確な OperationResult(BuildingUnlock) を返すこと。
        case Game::MsgType_C2S_BuyBuilding: {
          const auto *m = env->body_as_C2S_BuyBuilding();
          if (!m) break;
          const uint32_t buildingId = m->building_id();

          auto reject = [&](const std::string &reason) {
            network::send(msg.connId,
                codec::encodeOperationResult(
                    env->seq(), Game::OpResultType_BuildingUnlock, false,
                    reason));
          };

          const cache::BuildingTemplate *const tpl =
              cache::findBuilding(buildingCache, buildingId);
          if (!tpl) {
            reject("建物が存在しません");
            break;
          }
          if (buildings::isUnlocked(sess.buildings, buildingId)) {
            reject("「" + tpl->buildingName + "」は既に解放済みです");
            break;
          }
          if (static_cast<uint64_t>(tpl->unlockCost) >
              static_cast<uint64_t>(sess.roleDto.gold)) {
            reject("ゴールドが足りません（必要 " + std::to_string(tpl->unlockCost) +
                   "、所持 " + std::to_string(sess.roleDto.gold) +
                   "）");
            break;
          }

          auto instIt = connToInstance.find(msg.connId);
          MapInstance *const instPtr =
              instIt == connToInstance.end()
                  ? nullptr
                  : tryGetLiveMapInstance(mainWorld, instIt->second);
          if (!instPtr) {
                        // 生きているマップインスタンスがなければ建物のエンティティを作れず、BuildingChanged も送れない。
                        // この場合はゴールドを減らしてはならない。減らすと、プレイヤーはお金を払ったのに建物が見えないことになる。
            if (instIt != connToInstance.end()) connToInstance.erase(instIt);
            reject("メインシティにいないため、建物を解放できません");
            break;
          }
          auto &inst = *instPtr;

                    // 既にレコード（ロック状態）があればその座標を使い、なければテンプレートのデフォルト座標を使う。
          const buildings::RoleBuilding *const existing =
              buildings::find(sess.buildings, buildingId);
          buildings::RoleBuilding next;
          next.buildingId = buildingId;
          next.x     = existing ? existing->x : tpl->defaultX;
          next.y     = existing ? existing->y : tpl->defaultY;
          next.level = existing && existing->level > 0 ? existing->level
                                                       : tpl->baseLevel;
          next.state = buildings::kStateUnlocked;

                    // まずメモリ上の状態を確定させる：ゴールド、建物のレコード、マップ上のエンティティ。
          const uint32_t goldBeforeUnlock = sess.roleDto.gold;
          sess.roleDto.gold -= static_cast<uint32_t>(tpl->unlockCost);
          buildings::upsert(sess.buildings, next);

                    // シティの生成時に、ロック中の建物のエンティティも作ってあるので、ここではその場で状態を変更しなければならない。
                    // 新たに create すると、クライアントは同じ building_id の建物を 2 つ受け取ってしまう。
          uint32_t netId = 0;
          {
            auto bview = inst.world.view<NetworkId, CBuilding>();
            for (auto bent : bview) {
              auto &bld = bview.get<CBuilding>(bent);
              if (bld.buildingId != next.buildingId) continue;
              bld.state = next.state;
              bld.level = next.level;
              next.x = bld.x;  // エンティティの現在の配置を使う。解放によって建物が移動しないようにするため
              next.y = bld.y;
              netId = bview.get<NetworkId>(bent).value;
              break;
            }
          }
          if (netId == 0) {
                        // 保険：シティの生成後に追加された設定項目で、まだエンティティが存在しない場合。
            const auto ent = inst.world.create();
            netId = inst.networkEntityIdAlloc.alloc();
            inst.world.emplace<NetworkId>(ent, NetworkId{netId});
            inst.world.emplace<CBuilding>(ent, CBuilding{
                .buildingId = next.buildingId,
                .networkEntityId = netId,
                .x = next.x,
                .y = next.y,
                .level = next.level,
                .state = next.state,
            });
          }

          role_economy_db_writer::enqueueGold(sess.roleId, sess.roleDto.gold);
          role_economy_db_writer::enqueueRoleBuilding(
              sess.roleId, next.buildingId, next.x, next.y, next.level,
              next.state);

          codec::BuildingStaticData snap;
          snap.networkEntityId = netId;
          snap.buildingId      = next.buildingId;
          snap.buildingKind =
              static_cast<Game::BuildingKind>(next.buildingId);
          snap.ownerRoleId = sess.roleId;
          snap.x     = next.x;
          snap.y     = next.y;
          snap.level = next.level;
          snap.state = next.state;
          snap.buildingName = tpl->buildingName;
          snap.unlockCost = static_cast<uint32_t>(tpl->unlockCost);

          network::send(msg.connId,
              codec::encodeOperationResult(
                  env->seq(), Game::OpResultType_BuildingUnlock, true,
                  "「" + tpl->buildingName + "」を解放しました"));
          network::send(msg.connId,
              codec::encodeGoldChanged(env->seq(), goldBeforeUnlock,
                                       sess.roleDto.gold,
                                       codec::gold_reason::kBuyBuilding));
          network::send(msg.connId,
              codec::encodeBuildingChanged(env->seq(), inst.aoiClock.tick,
                                           snapshotTimeMsForInstance(inst),
                                           snap));
          break;
        }

                // ── 装備：装着 ──────────────────────────────────────
        case Game::MsgType_C2S_EquipItem: {
          const auto *m = env->body_as_C2S_EquipItem();
          if (!m) break;

                    // 改造クライアント対策：自分が所持している精霊にしか装備できない。このチェックがないと、改ざんしたクライアントが
                    // 任意の unit_id にアイテムを装備でき、unit_equips が汚れてアイテムが勝手に移動してしまう。
          if (!codec::ownsUnit(sess.ownedUnits, m->unit_id())) {
            network::send(msg.connId,
                codec::encodeOperationResult(
                    env->seq(), Game::OpResultType_ItemEquip, false,
                    "その精霊はまだ所持していません"));
            break;
          }

          const equip::EquipOutcome r =
              equip::equipItem(itemCache, sess.inventory, sess.equipment,
                               m->unit_id(), m->item_id(), m->slot());

          network::send(msg.connId,
              codec::encodeOperationResult(env->seq(), Game::OpResultType_ItemEquip,
                                           r.success, r.message));
          if (!r.success) break;

          for (const auto &c : r.itemChanges) {
            role_economy_db_writer::enqueueItemCount(sess.roleId, c.itemId,
                                                     c.newCount);
          }
          role_economy_db_writer::enqueueUnitEquip(sess.roleId, r.unitId,
                                                   r.slot, r.equippedItemId);

                    // 装備の変更は戦闘の数値に影響するので、すぐに新しい装備でそのユニットのステータスを再計算する。
                    // プレイヤーがマップにいないときは変更するエンティティがないが、次のマップ入場時の生成でボーナスが反映される。
          if (auto it = connToInstance.find(msg.connId);
              it != connToInstance.end()) {
            if (MapInstance *const ip =
                    tryGetLiveMapInstance(mainWorld, it->second)) {
              game::refreshUnitCombatStats(ip->world, sess.roleId, r.unitId, tc,
                                           itemCache, sess.equipment);
            }
          }

          network::send(msg.connId,
              codec::encodeBackpackChanged(env->seq(), itemCache,
                                           sess.inventory));
          break;
        }

                // ── 装備：解除 ──────────────────────────────────────
        case Game::MsgType_C2S_UnequipItem: {
          const auto *m = env->body_as_C2S_UnequipItem();
          if (!m) break;

          if (!codec::ownsUnit(sess.ownedUnits, m->unit_id())) {
            network::send(msg.connId,
                codec::encodeOperationResult(
                    env->seq(), Game::OpResultType_ItemUnequip, false,
                    "その精霊はまだ所持していません"));
            break;
          }

          const equip::EquipOutcome r =
              equip::unequipItem(itemCache, sess.inventory, sess.equipment,
                                 m->unit_id(), m->slot());

          network::send(msg.connId,
              codec::encodeOperationResult(env->seq(),
                                           Game::OpResultType_ItemUnequip,
                                           r.success, r.message));
          if (!r.success) break;

          for (const auto &c : r.itemChanges) {
            role_economy_db_writer::enqueueItemCount(sess.roleId, c.itemId,
                                                     c.newCount);
          }
          role_economy_db_writer::enqueueUnitEquip(sess.roleId, r.unitId,
                                                   r.slot, r.equippedItemId);

                    // 装備の変更は戦闘の数値に影響するので、すぐに新しい装備でそのユニットのステータスを再計算する。
                    // プレイヤーがマップにいないときは変更するエンティティがないが、次のマップ入場時の生成でボーナスが反映される。
          if (auto it = connToInstance.find(msg.connId);
              it != connToInstance.end()) {
            if (MapInstance *const ip =
                    tryGetLiveMapInstance(mainWorld, it->second)) {
              game::refreshUnitCombatStats(ip->world, sess.roleId, r.unitId, tc,
                                           itemCache, sess.equipment);
            }
          }

          network::send(msg.connId,
              codec::encodeBackpackChanged(env->seq(), itemCache,
                                           sess.inventory));
          break;
        }

                // ── ショップ：商品リストの取得 ────────────────────────────────
                // フロントエンドは category フィールドに常に 0 を渡し（一度に全件取得）、サーバーも常に全件を送る。
                // そのためここでは category で絞り込まず、タブの切り替えはクライアント側で行う。
        case Game::MsgType_C2S_ShopQueryList: {
          if (!env->body_as_C2S_ShopQueryList()) break;
          network::send(msg.connId,
              codec::encodeShopList(env->seq(), itemCache, sess.inventory,
                                    sess.roleDto.gold));
          break;
        }

                // ── ショップ：購入 ───────────────────────────────────────
        case Game::MsgType_C2S_ShopBuy: {
          const auto *m = env->body_as_C2S_ShopBuy();
          if (!m) break;

                    // 改造クライアント対策：取引はマップに入場済みのセッションのコンテキストで行わなければならない。
                    // マップ入場の流れを飛ばして、プロトコルを直接送ってゴールドやバッグを書き換えるのを防ぐ。
          if (!sessionInLiveMap(mainWorld, connToInstance, msg.connId)) {
            network::send(msg.connId,
                codec::encodeOperationResult(
                    env->seq(), Game::OpResultType_ShopBuy, false,
                    "メインシティにいないため、取引できません"));
            break;
          }

          const uint32_t goldBefore = sess.roleDto.gold;
          const shop::TradeOutcome r =
              shop::buy(itemCache, sess.inventory, sess.roleDto.gold,
                        m->shop_item_id(), m->count());

          network::send(msg.connId,
              codec::encodeOperationResult(env->seq(), Game::OpResultType_ShopBuy,
                                           r.success, r.message));
          if (!r.success) break;

                    // メモリ上の値が既に正なので、先に応答を返してから DB に保存する。永続化には絶対値を投入する。
          role_economy_db_writer::enqueueGold(sess.roleId, r.newGold);
          role_economy_db_writer::enqueueItemCount(sess.roleId, r.itemId,
                                                   r.newItemCount);
          network::send(msg.connId,
              codec::encodeGoldChanged(env->seq(), goldBefore, r.newGold,
                                       codec::gold_reason::kShopBuy));
          network::send(msg.connId,
              codec::encodeBackpackChanged(env->seq(), itemCache,
                                           sess.inventory));
          break;
        }

                // ── ショップ：売却 ───────────────────────────────────────
        case Game::MsgType_C2S_ShopSell: {
          const auto *m = env->body_as_C2S_ShopSell();
          if (!m) break;

          if (!sessionInLiveMap(mainWorld, connToInstance, msg.connId)) {
            network::send(msg.connId,
                codec::encodeOperationResult(
                    env->seq(), Game::OpResultType_ShopSell, false,
                    "メインシティにいないため、取引できません"));
            break;
          }

          const uint32_t goldBefore = sess.roleDto.gold;
          const shop::TradeOutcome r =
              shop::sell(itemCache, sess.inventory, sess.roleDto.gold,
                         m->item_id(), m->count());

          network::send(msg.connId,
              codec::encodeOperationResult(env->seq(), Game::OpResultType_ShopSell,
                                           r.success, r.message));
          if (!r.success) break;

          role_economy_db_writer::enqueueGold(sess.roleId, r.newGold);
          role_economy_db_writer::enqueueItemCount(sess.roleId, r.itemId,
                                                   r.newItemCount);
          network::send(msg.connId,
              codec::encodeGoldChanged(env->seq(), goldBefore, r.newGold,
                                       codec::gold_reason::kShopSell));
          network::send(msg.connId,
              codec::encodeBackpackChanged(env->seq(), itemCache,
                                           sess.inventory));
          break;
        }

        case Game::MsgType_C2S_Ping: {
          auto *ping = env->body_as_C2S_Ping();
          if (!ping) break;
          uint64_t clientWallTimeMs = ping->client_time_ms();
          uint64_t serverSimTimeMs = 0;
          auto instIt = connToInstance.find(msg.connId);
          if (instIt != connToInstance.end()) {
            MapInstance *instPtr =
                tryGetLiveMapInstance(mainWorld, instIt->second);
            if (instPtr) {
              serverSimTimeMs = snapshotTimeMsForInstance(*instPtr);
            } else {
              connToInstance.erase(instIt);
            }
          }
          network::send(msg.connId,
              codec::encodePong(env->seq(), clientWallTimeMs, serverSimTimeMs));
          break;
        }

        default:
                    // Phase 2 以降：C2S_MoveInput など
          break;

        } // switch env->msg_type()
        break;
      } // case MsgType::Data

      case MsgType::Disconnect: {
        auto sessIt = sessions.find(msg.connId);
        if (sessIt == sessions.end()) break;
        auto &sess = sessIt->second;
        const bool isCurrentRoleConn =
            sessionIsCurrentRoleConnection(roleToConn, sess, msg.connId);

        auto instIt = connToInstance.find(msg.connId);
        if (instIt != connToInstance.end()) {
          auto instEntity = instIt->second;
          if (mainWorld.valid(instEntity)) {
            auto &inst = mainWorld.get<MapInstance>(instEntity);
            destroyViewerForConn(mainWorld, inst, msg.connId);
            connToViewer.erase(msg.connId);

            auto &cids = inst.connIds;
            eraseConnId(cids, msg.connId);

                        // ログアウト前に、船の経験値とエリアをすべて保存する（必ず destroyEntitiesForRole より前に行う）
            player_ship_db_writer::flushAllShips(inst.world, sess.roleId);

            if (msg.gracefulDisconnect && isCurrentRoleConn) {
              auto leftNetworkEntityIds =
                  game::collectNetworkEntityIdsForRole(inst.world, sess.roleId);
              game::destroyEntitiesForRole(inst.world, sess.roleId);

              if (!leftNetworkEntityIds.empty()) {
                const uint64_t snapshotTick = inst.aoiClock.tick;
                const uint64_t snapshotTimeMs =
                    snapshotTimeMsForInstance(inst);
                for (uint64_t cid : cids) {
                  network::send(cid,
                      codec::encodeUpdateAoiSnapshot(
                          0, snapshotTick, snapshotTimeMs,
                          /*enteredIds=*/{}, /*enteredUnits=*/{},
                          /*updated=*/{}, leftNetworkEntityIds));
                }
              }
            }

                        // --- アイドルとしてマークする ---
            if (cids.empty()) {
              destroyAllViewers(mainWorld, inst);
              inst.emptyTimestampMs = timeutil::steady_now_ms();
            }
          }
          connToInstance.erase(instIt);
        }

        sessionEraseCurrentRoleConnection(roleToConn, sess, msg.connId);
        sessions.erase(sessIt);
        break;
      }

      } // switch msg.type
    } // for inboundBatch

    int simSteps = 0;
    while (simAccumulatorSec + 0.000001f >= tickDtSec &&
           simSteps < kMaxCatchUpTicks) {
      simAccumulatorSec -= tickDtSec;
      ++simSteps;

            // ── 3. Tick のロジック：AI の意思決定 → 移動 → AOI の送信 ────────────────
      {
        auto instView = mainWorld.view<MapInstance>();
        for (auto instEnt : instView) {
        auto &inst = instView.get<MapInstance>(instEnt);

                // アイドル状態のインスタンスはスキップする：接続がなければ AI も動かさず、AOI も送らない
        if (inst.connIds.empty()) continue;

        const float mapDtSec = inst.simClock.fixedDtSec;
        auto &hub = inst.world.ctx().get<MonsterAIEventHub>();

                // ── 感知／意思決定層：エリア索引 → ヘイト解除 → プレイヤー船 AI → モンスターの戦闘 ──
        area_registry_system::update(inst.world);
        aggro_clear_subscriber::update(inst.world, hub);
        const uint64_t nowMsTick = snapshotTimeMsForInstance(inst);

                // ドロップは consumeKills より前に処理すること。consumeKills が unit_killed.prev をクリアしてしまうため。
        item_drop_system::spawnDropsForKills(inst, tc, hub.unit_killed.prev,
                                             nowMsTick);

                // 撃破の後処理：ゴールドの報酬と、ボス撃破後の雑魚の再配置。consumeKills より前に行うこと。
                // consumeKills が unit_killed.prev をクリアしてしまうため。
        if (!hub.unit_killed.prev.empty()) {
          uint64_t goldGain = 0;
          bool bossKilled = false;
          uint32_t killedBossAreaId = 0;

          for (const auto &ev : hub.unit_killed.prev) {
                        // ゴールドは倒されたユニットのテンプレートの gold_reward から取り、通常モンスターもボスも同じルール。
                        // 支給は 1 回だけ。ボスに追加で上乗せはしない。同じ撃破が 2 つの経路で二重に報酬を出さないようにするため。
            const auto killedTplIt = tc.units.find(ev.victimUnitId);
            if (killedTplIt != tc.units.end()) {
              goldGain += killedTplIt->second.goldReward;
            }
            if (boss_challenge_system::bossUnitIdOf(inst.world, ev.victim) != 0) {
              bossKilled = true;
                            // エリアはボスのエンティティがまだあるうちに読み出しておく必要がある。破棄された後では調べられない。
              killedBossAreaId =
                  boss_challenge_system::bossAreaIdOf(inst.world, ev.victim);
            }
          }

                    // ゴールドはそのマップインスタンスの所有プレイヤーのものになる。メインシティは 1 人用のインスタンスなので、owner が唯一のプレイヤー。
          if (goldGain > 0) {
            const auto *instOwner = mainWorld.try_get<Owner>(instEnt);
            if (instOwner) {
              const auto ownerConnIt = roleToConn.find(instOwner->roleId);
              if (ownerConnIt != roleToConn.end()) {
                const auto ownerSessIt = sessions.find(ownerConnIt->second);
                if (ownerSessIt != sessions.end()) {
                  auto &ownerSess = ownerSessIt->second;
                  const uint32_t goldBeforeKill = ownerSess.roleDto.gold;
                                    // ゴールドの上限の保護：少なめに支給することはあっても、オーバーフローして値が巻き戻ることは許さない。
                  const uint64_t after =
                      static_cast<uint64_t>(goldBeforeKill) + goldGain;
                  ownerSess.roleDto.gold =
                      static_cast<uint32_t>(std::min<uint64_t>(
                          after, std::numeric_limits<uint32_t>::max()));

                  if (ownerSess.roleDto.gold != goldBeforeKill) {
                    role_economy_db_writer::enqueueGold(ownerSess.roleId,
                                                        ownerSess.roleDto.gold);
                    network::send(ownerSess.connId,
                        codec::encodeGoldChanged(
                            0, goldBeforeKill, ownerSess.roleDto.gold,
                            codec::gold_reason::kKillReward));
                  }

                  if (bossKilled) {
                    network::send(ownerSess.connId,
                        codec::encodeOperationResult(
                            0, Game::OpResultType_BossChallenge, true,
                            "ボスを倒しました！ゴールド " + std::to_string(goldGain) +
                                " を獲得しました"));
                  }
                }
              }
            }
          }

                    // 雑魚は挑戦の開始時に片付けており、ボスを倒した後に固定設定どおりに元に戻す。
          if (bossKilled) {
            boss_challenge_system::respawnNormalMonsters(inst, tc);

                        // 復活した雑魚をすぐに索敵できるよう、編成をボスのエリアに割り当て直す。
                        // assignShipsToArea はターゲットをクリアし、nextRetargetAtMs を 0 にしてナビゲーションをリセットするので、
                        // 精霊は次の索敵周期を待たずに戦闘を始められる。同時に、ボスが消えた後に精霊がメインシティへ
                        // 戻ってしまわないことも保証する。
            const auto *resetOwner = mainWorld.try_get<Owner>(instEnt);
            if (resetOwner && killedBossAreaId != 0) {
                            // 編成の情報があるのは、そのマップインスタンスの所有プレイヤーがオンラインのときだけ。オフラインなら行き先は変えない。
              std::vector<uint32_t> resetDeployed;
              if (const auto rc = roleToConn.find(resetOwner->roleId);
                  rc != roleToConn.end()) {
                if (const auto rs = sessions.find(rc->second);
                    rs != sessions.end()) {
                  resetDeployed = team_ops::deployedUnitIds(rs->second.roleDto);
                }
              }
              player_ship_ai_system::assignShipsToArea(
                  inst.world, hub, resetOwner->roleId, killedBossAreaId,
                  nowMsTick, inst.mapId, resetDeployed);

                            // エリアの変更は DB に保存する。C2S_AssignShipToArea の経路と同じ扱いにする。
              for (auto e : inst.world.view<Owner, UnitBase,
                                            CPlayerShipAssignment>()) {
                const auto &owner = inst.world.get<Owner>(e);
                if (owner.roleId != resetOwner->roleId) continue;
                const auto &base = inst.world.get<UnitBase>(e);
                const auto &assign =
                    inst.world.get<CPlayerShipAssignment>(e);
                if (base.unitId == 0) continue;
                player_ship_db_writer::enqueueShipArea(
                    owner.roleId, base.unitId, assign.areaId);
              }
            }
          }
        }

        player_ship_ai_system::consumeKills(inst.world, hub, tc, inst.mapId,
                                           maxUnitLevel);

                // 期限切れのドロップアイテムは、AOI の left の経路で明示的に回収する必要がある。そうしないとクライアントにノードが残る。
        {
          std::vector<uint32_t> expiredDropIds;
          item_drop_system::cleanupExpiredDrops(inst, nowMsTick,
                                                expiredDropIds);
          if (!expiredDropIds.empty()) {
            for (uint64_t cid : inst.connIds) {
              network::send(cid,
                  codec::encodeUpdateAoiSnapshot(
                      0, inst.aoiClock.tick, nowMsTick,
                      /*enteredIds=*/{}, /*enteredUnits=*/{},
                      /*updated=*/{}, expiredDropIds));
            }
          }
        }
        player_ship_ai_system::update(inst.world, hub, mapDtSec, nowMsTick,
                                       inst.mapId);
        city_monster_combat_system::update(inst.world, hub, mapDtSec,
                                           inst.mapId);

                // ── 行動層：モンスターの巡回（in_combat はスキップ）→ 速度の適用 ──
                // プレイヤー船の巡回・徘徊は player_ship_ai_system::update で処理済みで、ここは通らない。
        monster_wander_system::update(inst.world, mapDtSec);
        const auto &navGrid = game::navGridOf(inst.world);
        game::nav_request_system::update(inst.world, navGrid, nowMsTick);
        game::nav_follow_system::update(inst.world, navGrid);
        velocity_system::update(inst.world, mapDtSec);
                // ── 強制的な押し戻し：CAreaBounds を持つエンティティ（シティモンスター）を矩形内に戻し、範囲外へ出た軸の速度を 0 にする ──
        movement_clamp_system::update(inst.world);

                // ── 補助：治療施設による自動回復 ──
        healing_station_system::update(inst.world, hub, mapDtSec, inst.mapId);

                // ── ライフサイクル：このフレームの死亡イベントを処理する → 死体のタイマー + スポーン枠 ──
                //   hub.death の curr → prev の swap より前に呼ぶこと（後だとイベントが既に移動している）。
        monster_lifecycle_system::consumeDeaths(inst.world, hub);
                // ── ライフサイクル：死体の破棄 / スポーン枠のカウントダウンを進める ──
        monster_lifecycle_system::update(inst.world, inst.networkEntityIdAlloc,
                                         tc, mapDtSec, inst.mapId);

                // tick の最後に全イベント列をまとめてローテートする。新しく追加したイベントが一部の入口でしか有効にならない事態を防ぐ。
        rotateMonsterAIEventHub(hub);

        sessionAppendMonsterHubPrevForAoi(pendingAoiBattleEvents[instEnt], hub);

                // ── DB への遅延書き込み（Write-Behind）：レベルアップやしきい値で dirty になった経験値を投入する ──
        player_ship_db_writer::flushDirtyExp(inst.world);

        advanceMapSimTick(inst.simClock);

        inst.aoiClock.pushElapsedSec += mapDtSec;
        const bool shouldPushAoi =
            inst.aoiClock.pushElapsedSec >= kAoiPushIntervalSec;
        if (!shouldPushAoi) {
          continue;
        }
        inst.aoiClock.pushElapsedSec -= kAoiPushIntervalSec;
        const uint64_t currentAoiTick = ++inst.aoiClock.tick;
        const uint64_t snapshotTimeMs = snapshotTimeMsForInstance(inst);
        const uint64_t nowMs = timeutil::steady_now_ms();

                // メイン World で、このインスタンスを閲覧している viewer を走査する。viewer が持つのはネットワーク／AOI の状態だけ。
        for (auto viewerIt = inst.viewerEntities.begin();
             viewerIt != inst.viewerEntities.end();) {
          entt::entity viewerEntity = *viewerIt;
          if (!mainWorld.valid(viewerEntity) ||
              !mainWorld.all_of<MapViewer, ViewerCamera, ViewerAoiState>(
                  viewerEntity)) {
            viewerIt = inst.viewerEntities.erase(viewerIt);
            continue;
          }

          auto &viewer = mainWorld.get<MapViewer>(viewerEntity);
          if (viewer.mapInstance != instEnt) {
            viewerIt = inst.viewerEntities.erase(viewerIt);
            continue;
          }

          auto &cam = mainWorld.get<ViewerCamera>(viewerEntity);
          auto &vis = mainWorld.get<ViewerAoiState>(viewerEntity);
          ++viewerIt;

          if (sessions.find(viewer.connId) == sessions.end()) continue;
          const uint64_t connId = viewer.connId;
          const std::string &viewerRoleId = viewer.roleId;

          std::unordered_set<uint32_t> enteredIds;
          std::unordered_set<uint32_t> updatedIds;
          std::vector<uint32_t> leftNetworkEntityIds;
          cam.dirty = false;

          const std::unordered_set<uint32_t> newVisibleNetworkEntityIds =
              collectVisibleNetworkEntityIds(inst.world, cam);

          // entered = newVisible - oldVisible
          for (uint32_t id : newVisibleNetworkEntityIds) {
            if (!vis.contains(id)) {
              enteredIds.insert(id);
            }
          }
          // left = oldVisible - newVisible
          for (uint32_t id : vis.networkIds()) {
            if (newVisibleNetworkEntityIds.find(id) == newVisibleNetworkEntityIds.end()) {
              leftNetworkEntityIds.push_back(id);
            }
          }

          for (uint32_t leftNetworkEntityId : leftNetworkEntityIds) {
            VisibleUnitState prev;
            if (!vis.loadState(leftNetworkEntityId, prev)) {
              continue;
            }

            const uint64_t msSinceLastPush =
                prev.lastPushAtMs > 0 ? (nowMs - prev.lastPushAtMs) : 0;
            logAoiEntityTrace(zone_id, inst.mapId, viewerRoleId, currentAoiTick,
                              "left", leftNetworkEntityId, msSinceLastPush,
                              nullptr, &prev, nullptr, nullptr, "left",
                              MotionSourceProbe{});
          }

          auto visibleSnaps =
              collectSnapshotByNetworkEntityIds(inst.world,
                                                newVisibleNetworkEntityIds, tc);
          ViewerAoiState nextVisibleState;
          nextVisibleState.reserve(visibleSnaps.size());
          std::vector<uint32_t> enteredIdsForEncode;
                    // enteredUnits と enteredIdsForEncode は添字が 1 対 1 で対応する。定常状態では enteredIds が空なので、
                    // この vector も空になり、コストはゼロ。
          std::vector<codec::UnitStaticData> enteredUnitsForEncode;
          codec::UnitDynamicDataSoA updatedDynamics;
          enteredIdsForEncode.reserve(enteredIds.size());
          enteredUnitsForEncode.reserve(enteredIds.size());
          updatedDynamics.reserve(visibleSnaps.size());

          for (const auto &snap : visibleSnaps) {
            const uint32_t networkEntityId = snap.networkEntityId;
            const MotionSourceProbe motion =
                collectMotionSourceProbe(inst.world, networkEntityId);
            const PushProbe enteredProbe =
                buildPushProbe(snap, buildVisibleUnitState(snap));

                        // entered のエンティティ：送信間隔の状態を初期化し、動的データも同時に送る
                        //
                        // 取り決め：entered のエンティティは、その UnitDynamicData と同じメッセージで一緒に送らなければならない。
                        // こうすることでクライアントは（本当の座標に）ノードを生成でき、「まず (0,0) にノードを作って
                        // 次のフレームで位置を同期する」ようなずれは決して起こらない。そのためここでは continue せず、動的データを
                        // updatedDynamics に入れて静的データと一緒に送る。updatedIds には entered の nid を入れない
                        // （enteredIds と二重に数えないため）が、updatedDynamics のペイロードには含める。
            if (enteredIds.find(networkEntityId) != enteredIds.end()) {
              enteredIdsForEncode.push_back(networkEntityId);
              enteredUnitsForEncode.push_back(snapshotToStatic(snap));
              VisibleUnitState vs = buildVisibleUnitState(snap);
              if (isMotionActive(enteredProbe, motion)) {
                vs.cadence = kCadenceActive;
                vs.cooldownTicks = 0;
                vs.wasMoving = true;
              } else {
                vs.cadence = kCadenceSteady;
                vs.cooldownTicks = kCadenceSteady;
                vs.wasMoving = false;
              }
              vs.lastPushAtMs = nowMs;
              vs.lastPushTick = currentAoiTick;
              nextVisibleState.pushVisible(networkEntityId, vs);
              logAoiEntityTrace(zone_id, inst.mapId, viewerRoleId, currentAoiTick,
                                "entered", networkEntityId, 0, &snap, nullptr,
                                &vs, nullptr, "entered", motion);

                            // entered のエンティティの最初の動的スナップショット（位置／HP／facing など）は、静的データと必ず一緒に送る
              codec::UnitDynamicData dyn;
              dyn.networkEntityId = snap.networkEntityId;
              dyn.x = snap.x;
              dyn.y = snap.y;
              dyn.vx = snap.vx;
              dyn.vy = snap.vy;
              dyn.facing = snap.facing;
              dyn.curHp = snap.curHp;
              dyn.maxHp = snap.maxHp;
              dyn.lifeStatus = snap.lifeStatus;
              dyn.actionState = snap.actionState;
              updatedDynamics.push(dyn);
              continue;
            }

            VisibleUnitState prev;
            if (!vis.loadState(networkEntityId, prev)) {
                            // 起こるはずはないが、念のための防御的な処理
              VisibleUnitState vs = buildVisibleUnitState(snap);
              if (isMotionActive(enteredProbe, motion)) {
                vs.cadence = kCadenceActive;
                vs.cooldownTicks = 0;
                vs.wasMoving = true;
              } else {
                vs.cadence = kCadenceSteady;
                vs.cooldownTicks = kCadenceSteady;
                vs.wasMoving = false;
              }
              vs.lastPushAtMs = nowMs;
              vs.lastPushTick = currentAoiTick;
              nextVisibleState.pushVisible(networkEntityId, vs);
              continue;
            }

                        // 前のフレームの prev から送信間隔の状態を読み、送るかどうかを評価する
            const VisibleUnitState prevBefore = prev;
            const PushProbe probe = buildPushProbe(snap, prevBefore);
            PushVerdict verdict = evaluatePush(probe, motion, prev);

                        // 更新した送信間隔の状態と新しいスナップショットのデータを、次のフレームの可視状態に書き込む
            VisibleUnitState updated = buildVisibleUnitState(snap);
            updated.cadence = prev.cadence;
            updated.cooldownTicks = prev.cooldownTicks;
            updated.wasMoving = prev.wasMoving;
            updated.lastPushAtMs = prevBefore.lastPushAtMs;
            updated.lastPushTick = prevBefore.lastPushTick;

            if (verdict != PushVerdict::Skip) {
              const uint64_t msSinceLastPush =
                  prevBefore.lastPushAtMs > 0 ? (nowMs - prevBefore.lastPushAtMs) : 0;
              updated.lastPushAtMs = nowMs;
              updated.lastPushTick = currentAoiTick;
              logAoiEntityTrace(zone_id, inst.mapId, viewerRoleId, currentAoiTick,
                                "updated", networkEntityId, msSinceLastPush,
                                &snap, &prevBefore, &updated, &probe,
                                toString(verdict), motion);
              updatedIds.insert(networkEntityId);
                            // UnitSnapshotData -> UnitDynamicData に変換する
              codec::UnitDynamicData dyn;
              dyn.networkEntityId = snap.networkEntityId;
              dyn.x = snap.x;
              dyn.y = snap.y;
              dyn.vx = snap.vx;
              dyn.vy = snap.vy;
              dyn.facing = snap.facing;
              dyn.curHp = snap.curHp;
              dyn.maxHp = snap.maxHp;
              dyn.lifeStatus = snap.lifeStatus;
              dyn.actionState = snap.actionState;
              updatedDynamics.push(dyn);
            }
            nextVisibleState.pushVisible(networkEntityId, updated);
          }

                    // 変化があるときだけパケットを送る
          if (!enteredIds.empty() || !updatedIds.empty() ||
              !leftNetworkEntityIds.empty()) {
            network::send(connId,
                codec::encodeUpdateAoiSnapshot(
                    0, currentAoiTick, snapshotTimeMs,
                    enteredIdsForEncode, enteredUnitsForEncode,
                    updatedDynamics, leftNetworkEntityIds));
          }

                    // ── 前回の AOI 送信以降に溜まった戦闘イベントをパックする ──
          {
            codec::BattleEventBatchSoA items;
            auto &fbb = codec::beginBattleEventBatch();
            const uint64_t tms = snapshotTimeMsForInstance(inst);
            game::appendBattleEventItems(inst.world,
                                         pendingAoiBattleEvents[instEnt],
                                         fbb, tms,
                                         &newVisibleNetworkEntityIds, items);

            if (!items.empty()) {
              std::string pkt = codec::encodeBattleEventBatch(
                  0, currentAoiTick, items);
              if (!pkt.empty()) network::send(connId, std::move(pkt));
            }
          }

          nextVisibleState.rebuildIndex();
          vis = std::move(nextVisibleState);
        }
        sessionClearMonsterHubEvents(pendingAoiBattleEvents[instEnt]);
      }
    }

        // ── 4. メインシティのインスタンスの GC ──────────────────────────────────────
    {
      cityGcElapsedSec += tickDtSec;
      if (cityGcElapsedSec >= kCityGcIntervalSec) {
        cityGcElapsedSec = 0.f;
        const uint64_t nowMs = timeutil::steady_now_ms();

                // 破棄対象のリストを集める（mainWorld を走査しながら破棄はできない）
        std::vector<std::pair<entt::entity, std::string>> toDestroy;

        auto instView = mainWorld.view<MapInstance, Owner, CityData>();
        for (auto instEnt : instView) {
          auto &inst = instView.get<MapInstance>(instEnt);

                    // アイドルでないインスタンスはスキップする
          if (inst.emptyTimestampMs == 0) continue;
                    // まだ猶予期間内のものはスキップする
          if (nowMs - inst.emptyTimestampMs < kCityIdleGracePeriodMs) continue;
                    // 思いがけず接続が戻ってきたものはスキップする（防御的なチェック）
          if (!inst.connIds.empty()) {
            inst.emptyTimestampMs = 0;
            continue;
          }

          const auto &own = instView.get<Owner>(instEnt);
          toDestroy.emplace_back(instEnt, own.roleId);
        }

        for (const auto &[instEnt, roleId] : toDestroy) {
                    // 1. メイン World で、このインスタンスに付いている閲覧者を片付ける
          auto &inst = mainWorld.get<MapInstance>(instEnt);
          for (entt::entity viewerEntity : inst.viewerEntities) {
            if (mainWorld.valid(viewerEntity) &&
                mainWorld.all_of<MapViewer>(viewerEntity)) {
              const auto &viewer = mainWorld.get<MapViewer>(viewerEntity);
              connToViewer.erase(viewer.connId);
            }
          }
          destroyAllViewers(mainWorld, inst);

                    // 2. sub-world を空にする。NavPathStore は ctx() に置いているので、先にエージェントの経路を明示的に解放してから
                    //    プールをリセットする必要がある。そうしないと clear() がコンポーネント単位の経路の返却を素通りしてしまう。
          game::navClearAllEntityPaths(inst.world);
          inst.world.clear();
          game::navResetPathStore(inst.world);

                    // 3. 索引表から取り除く
          cityByRole.erase(roleId);
          pendingAoiBattleEvents.erase(instEnt);

                    // 4. mainWorld からエンティティを破棄する（MapInstance / Owner / CityData のコンポーネントもあわせて破棄される）
          mainWorld.destroy(instEnt);

        }
      }
    }

    }
    if (simSteps >= kMaxCatchUpTicks) {
      simAccumulatorSec = 0.f;
    }

        // ── 次のフレームを待つ ──
    if (simAccumulatorSec < tickDtSec) {
      const float remainingSec = tickDtSec - simAccumulatorSec;
      const uint64_t sleepMs =
          std::min<uint64_t>(
              tickIntervalMs,
              static_cast<uint64_t>(remainingSec * 1000.f));
      if (sleepMs > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
      }
    }
  }

  auto instView = mainWorld.view<MapInstance, Owner>();
  for (auto instEnt : instView) {
    auto &inst = instView.get<MapInstance>(instEnt);
    const auto &own = instView.get<Owner>(instEnt);
    player_ship_db_writer::flushAllShips(inst.world, own.roleId);
  }
  zone_log::info("game worker %u stopped", zone_id);
}
