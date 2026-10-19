#pragma once
#include "ecs/aoi_grid.h"
#include "ecs/events.h"
#include "network/msg_types.h"
#include <cstdint>
#include <entt/entt.hpp>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/**
 * @brief 感知層からヘイト層へ伝える、ターゲットの可視状態イベントの種類
 *
 * @note
 * - Enter はターゲットが感知範囲に入ったこと、Leave は外れたことを表す。
 */
enum class ThreatEventType : uint8_t {
  Enter = 0,
  Leave = 1,
};

/**
 * @brief モンスターがプレイヤーを感知したときのイベントバスメッセージ
 *
 * @note
 * - monster_inst_id/player_id は業務上の文字列 ID。消費側で現在の world のエンティティに解決する必要がある。
 * - map_id でイベントが属するマップを限定し、マップ間で混線しないようにする。
 */
struct ThreatEvent {
  ThreatEventType type = ThreatEventType::Enter;
  uint32_t map_id = 0;
  std::string monster_inst_id;
  std::string player_id;
};

/**
 * @brief プレイヤーがモンスターを感知したときのイベントの種類
 *
 * @note
 * - Enter/Leave の意味は ThreatEventType と同じで、向きが逆になる。
 */
enum class PlayerThreatEventType : uint8_t {
  Enter = 0,
  Leave = 1,
};

/**
 * @brief プレイヤーがモンスターを感知したときのイベントバスメッセージ
 *
 * @note
 * - player_id/monster_inst_id は業務上の ID で、entt::entity ではない。
 * - 消費側は map_id と現在の registry をもとにエンティティを解決し直すこと。
 */
struct PlayerThreatEvent {
  PlayerThreatEventType type = PlayerThreatEventType::Enter;
  uint32_t map_id = 0;
  std::string player_id;
  std::string monster_inst_id;
};

/**
 * @brief プレイヤーの攻撃によってモンスターにヘイトを加算するイベント
 *
 * @note
 * - 被弾時に強制的にヘイトを向けさせるために使う。他のシステムから UnitAggro を直接書き換えないようにするため。
 * - force_target=true の場合、消費側はそのプレイヤーを直ちに現在のターゲットにすること。
 */
struct MonsterThreatInjectEvent {
  uint32_t map_id = 0;
  std::string monster_inst_id;
  std::string player_id;
  float threat_delta = 1.f;
  bool force_target = false;
};

/**
 * @brief ゲームワーカー 1 つ分のメイン world とインデックス群
 *
 * @note
 * - registry は第 1 層の ECS で、接続・キャラクター・MapInstance のエンティティを持つ。
 * - conn_entity/player_entity_by_role_id は派生インデックス。エンティティの作成・破棄時に必ず同期して更新すること。
 * - イベントバスは担当するシステムが書き込み、tick の終わりにクリアする。ワーカー間では共有しない。
 */
struct ZoneRegistry {
  entt::registry registry;
  std::unordered_map<uint64_t, entt::entity> conn_entity;
  std::unordered_map<std::string, entt::entity> player_entity_by_role_id;
    // ゾーンごとに独立した AOI グリッド
  AoiGrid grid;

    // モンスターエンティティの索引：key = "map_id:inst_id"
  std::unordered_map<std::string, entt::entity> monster_entity_by_key;
    // モンスターのスナップショットを読み込み済みのスコープ：key = map_id
  std::unordered_set<uint32_t> loaded_monster_scopes;
    // イベントバス：感知層が書き込み、評価層が読む
  std::vector<ThreatEvent> threat_event_bus;
  std::vector<PlayerThreatEvent> player_threat_event_bus;
  std::vector<MonsterThreatInjectEvent> monster_threat_inject_bus;
  MonsterAIEventHub monster_ai_events;
  PlayerAIEventHub player_ai_events;
};

/**
 * @brief 全ゲームワーカーのメイン registry の配列
 *
 * @note
 * - 添字はワーカー ID に対応する。他のワーカーの registry を直接読み書きしないこと。
 */
inline std::array<ZoneRegistry, runtime_config::NUM_GAME_WORKERS> g_zones;
