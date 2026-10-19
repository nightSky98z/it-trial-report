#pragma once
#include "protocol/codec.h"
#include "config/runtime_config.h"
#include "ecs/events.h"
#include "game/role_buildings.h"
#include "game/role_equipment.h"
#include "game/role_inventory.h"
#include <concurrentqueue.h>
#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

/**
 * @brief zone_http から game_loop へ送るセッション登録メッセージ
 *
 * @note
 * - zone_http が C2S_RequestEnterZone の処理を終えたら、対応する game_worker のキューに書き込む。
 * - passToken は今回の EnterZone 成功時に新しく発行したセッショントークンで、再接続に使った古いトークンではない。
 * - game_loop がこれを処理した後でなければ、その接続の C2S_RequestEnterMap は処理できない。
 */
struct SessionRegister {
  uint64_t connId = 0;
  std::string roleId;
  std::string passToken;
  uint32_t currentMapId = 0;
  codec::RoleInfoDto roleDto;
  bool isReconnect = false;

    // ログイン時に role_items / unit_equips からまとめて読み込む。登録メッセージと一緒に GameSession へムーブし、
    // それ以降 zone_http はこのキャラクターのバッグと装備の状態を持たない。
  inventory::RoleInventory inventory;
  equipment::RoleEquipment equipment;
  buildings::RoleBuildings buildings;

    // role_units にある、このキャラクターが所持する全精霊（待機中も含む）。レベル・経験値・エリア付き。
    // マップ上には所持している精霊すべてのエンティティを生成する。フロントエンドの「マイ精霊」一覧は
    // マップ上の PlayerUnit エンティティそのものなので、待機中の精霊もその場にいないと出撃させるために選べない。
  std::vector<codec::TeamUnitInfoDto> ownedUnits;
};

inline std::array<moodycamel::ConcurrentQueue<SessionRegister>,
                  runtime_config::NUM_GAME_WORKERS>
    gSessionRegister;

/**
 * @brief game_worker ローカルのセッション状態
 *
 * @note
 * - 読み書きは、そのワーカーの tick を持つ game_loop のスレッドからのみ行う。
 * - pendingMapId は C2S_RequestEnterMap の検証を通過し、C2S_EnterMapReady で消費されるのを
 *   待っている目的地のマップ。hasPendingMap がないときに、Ready が roleDto.currentMapId を
 *   暗黙に使ってはならない。
 * - roleDto/currentMapId はキャラクターの永続状態のスナップショット。Ready が pending map を
 *   消費したときに初めて、今回の入場先に更新される。
 * - roleDto.gold と inventory は、ゴールドとバッグの実行時の正となる値。
 *   ショップなどの経済操作はここを直接書き換えてから非同期で DB に保存する。DB の現在値を読み直さないこと。
 */
struct GameSession {
  uint64_t connId = 0;
  std::string roleId;
  std::string passToken;
  codec::RoleInfoDto roleDto;
  bool isReconnect = false;
  bool hasPendingMap = false;
  uint32_t pendingMapId = 0;

  inventory::RoleInventory inventory;
  equipment::RoleEquipment equipment;
  buildings::RoleBuildings buildings;

    // role_units にある、このキャラクターが所持する全精霊（待機中も含む）。レベル・経験値・エリア付き。
    // マップ上には所持している精霊すべてのエンティティを生成する。フロントエンドの「マイ精霊」一覧は
    // マップ上の PlayerUnit エンティティそのものなので、待機中の精霊もその場にいないと出撃させるために選べない。
  std::vector<codec::TeamUnitInfoDto> ownedUnits;
};

/**
 * @brief 検証済みのマップ入場リクエストを記録し、EnterMapReady による消費を待つ
 *
 * @param sess 現在の接続が所有するセッション。pending map のフィールドだけを変更する。
 * @param mapId テンプレートの存在確認が済んだマップ ID。
 *
 * @note
 * - 呼び出し前に role_id/passToken/map_id の検証を済ませておくこと。
 */
inline void sessionSetPendingMap(GameSession &sess, uint32_t mapId) {
  sess.hasPendingMap = true;
  sess.pendingMapId = mapId;
}

/**
 * @brief EnterMapReady に対応する pending map を消費する
 *
 * @param sess 現在の接続が所有するセッション。pending map をクリアする。
 * @param outMapId 消費したマップ ID を書き出す。
 *
 * @retval true pending map があり、outMapId に書き込んだ。
 * @retval false RequestEnterMap による pending map がない。呼び出し側は Ready を拒否すること。
 */
inline bool sessionConsumePendingMap(GameSession &sess, uint32_t &outMapId) {
  if (!sess.hasPendingMap) {
    return false;
  }
  outMapId = sess.pendingMapId;
  sess.hasPendingMap = false;
  sess.pendingMapId = 0;
  return true;
}

/**
 * @brief 現在の接続が、まだそのキャラクターの有効な接続かどうかを確認する
 *
 * @param roleToConn ワーカーローカルの「role -> 現在の接続」索引。借用するだけ。
 * @param sess msg.connId に対応するセッション。読み取りのみ。
 * @param connId 現在のネットワークメッセージの送信元接続。
 *
 * @retval true roleToConn がまだ connId を指しており、データを処理してよい。
 * @retval false 同じキャラクターが新しい接続に引き継がれている。古い接続からのデータは拒否すること。
 */
inline bool sessionIsCurrentRoleConnection(
    const std::unordered_map<std::string, uint64_t> &roleToConn,
    const GameSession &sess, uint64_t connId) {
  const auto it = roleToConn.find(sess.roleId);
  return it != roleToConn.end() && it->second == connId;
}

/**
 * @brief 現在の role->conn がまだ connId を指している場合にだけ対応を削除する
 *
 * @param roleToConn ワーカーローカルの「role -> 現在の接続」索引。
 * @param sess 切断しようとしているセッション。roleId だけを読む。
 * @param connId 切断しようとしている接続。
 *
 * @retval true 現在の対応を削除した。
 * @retval false 対応はすでに新しい接続を指している。呼び出し側は誤って削除しないこと。
 */
inline bool sessionEraseCurrentRoleConnection(
    std::unordered_map<std::string, uint64_t> &roleToConn,
    const GameSession &sess, uint64_t connId) {
  const auto it = roleToConn.find(sess.roleId);
  if (it == roleToConn.end() || it->second != connId) {
    return false;
  }
  roleToConn.erase(it);
  return true;
}

template <typename T>
inline void appendTickPrev(std::vector<T> &dst, const std::vector<T> &src) {
  dst.insert(dst.end(), src.begin(), src.end());
}

/**
 * @brief prev にローテート済みの戦闘／AI イベントを AOI 送信用バッファに追記する
 *
 * @param dst MapInstance ごとの、AOI で送信待ちのイベントバッファ。prev に追記する。
 * @param src この sim tick で curr -> prev のローテートが済んだイベントハブ。prev だけを読む。
 *
 * @note
 * - 呼び出し位置は、その sim tick のイベントの swap/clear の後、かつ AOI 送信の判定より前であること。
 * - AOI の送信が終わったら、重複送信を防ぐために呼び出し側で dst をクリアすること。
 */
inline void sessionAppendMonsterHubPrevForAoi(MonsterAIEventHub &dst,
                                              const MonsterAIEventHub &src) {
  appendTickPrev(dst.damage_taken.prev, src.damage_taken.prev);
  appendTickPrev(dst.target_spotted.prev, src.target_spotted.prev);
  appendTickPrev(dst.target_lost.prev, src.target_lost.prev);
  appendTickPrev(dst.state_changed.prev, src.state_changed.prev);
  appendTickPrev(dst.action_started.prev, src.action_started.prev);
  appendTickPrev(dst.action_finished.prev, src.action_finished.prev);
  appendTickPrev(dst.skill_cast.prev, src.skill_cast.prev);
  appendTickPrev(dst.combat_result.prev, src.combat_result.prev);
  appendTickPrev(dst.phase_changed.prev, src.phase_changed.prev);
  appendTickPrev(dst.death.prev, src.death.prev);
  appendTickPrev(dst.dangerous_projectile.prev, src.dangerous_projectile.prev);
  appendTickPrev(dst.heal.prev, src.heal.prev);
  appendTickPrev(dst.hp_changed.prev, src.hp_changed.prev);
  appendTickPrev(dst.buff_changed.prev, src.buff_changed.prev);
  appendTickPrev(dst.revive.prev, src.revive.prev);
  appendTickPrev(dst.aggro_clear.prev, src.aggro_clear.prev);
  appendTickPrev(dst.unit_killed.prev, src.unit_killed.prev);
  appendTickPrev(dst.level_up.prev, src.level_up.prev);
  appendTickPrev(dst.stats_changed.prev, src.stats_changed.prev);
}

/**
 * @brief AOI の送信待ちイベントバッファをクリアする
 *
 * @param hub MapInstance ごとの AOI イベントバッファ。prev/curr の vector だけをクリアする。
 *
 * @note
 * - 今回の AOI 送信について、すべての閲覧者分のパックが終わってから呼ぶこと。
 */
inline void sessionClearMonsterHubEvents(MonsterAIEventHub &hub) {
  hub.damage_taken.prev.clear();
  hub.damage_taken.curr.clear();
  hub.target_spotted.prev.clear();
  hub.target_spotted.curr.clear();
  hub.target_lost.prev.clear();
  hub.target_lost.curr.clear();
  hub.state_changed.prev.clear();
  hub.state_changed.curr.clear();
  hub.action_started.prev.clear();
  hub.action_started.curr.clear();
  hub.action_finished.prev.clear();
  hub.action_finished.curr.clear();
  hub.skill_cast.prev.clear();
  hub.skill_cast.curr.clear();
  hub.combat_result.prev.clear();
  hub.combat_result.curr.clear();
  hub.phase_changed.prev.clear();
  hub.phase_changed.curr.clear();
  hub.death.prev.clear();
  hub.death.curr.clear();
  hub.dangerous_projectile.prev.clear();
  hub.dangerous_projectile.curr.clear();
  hub.heal.prev.clear();
  hub.heal.curr.clear();
  hub.hp_changed.prev.clear();
  hub.hp_changed.curr.clear();
  hub.buff_changed.prev.clear();
  hub.buff_changed.curr.clear();
  hub.revive.prev.clear();
  hub.revive.curr.clear();
  hub.aggro_clear.prev.clear();
  hub.aggro_clear.curr.clear();
  hub.unit_killed.prev.clear();
  hub.unit_killed.curr.clear();
  hub.level_up.prev.clear();
  hub.level_up.curr.clear();
  hub.stats_changed.prev.clear();
  hub.stats_changed.curr.clear();
}
