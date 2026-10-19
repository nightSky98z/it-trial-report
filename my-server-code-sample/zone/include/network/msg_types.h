#pragma once
#include <concurrentqueue.h>
#include "config/runtime_config.h"
#include <cstdint>
#include <shared_mutex>
#include <string>
#include <unordered_map>

/**
 * @brief プロセス内の接続 ID を生成する
 *
 * @param worker_id ネットワークワーカーの ID。上位 8 ビットに書き込む。
 * @param seq ワーカー内の連番。下位 56 ビットだけを使う。
 *
 * @return conn_id。現在のプロセスが動いている間だけ有効で、業務上の主キーとして永続化してはならない。
 */
inline uint64_t makeConnId(uint8_t worker_id, uint64_t seq) {
  return ((uint64_t)worker_id << 56) | (seq & 0x00FFFFFFFFFFFFFFull);
}

/**
 * @brief conn_id が属するネットワークワーカーを取り出す
 *
 * @param conn_id makeConnId で生成した接続 ID。
 *
 * @return 上位 8 ビットのワーカー ID。
 */
inline uint8_t netWorkerOf(uint64_t conn_id) {
  return (uint8_t)(conn_id >> 56);
}

/**
 * @brief map_id に応じて担当するゲームワーカーを選ぶ
 *
 * @param map_id マップテンプレートまたはインスタンスのルーティング ID。
 *
 * @return ゲームワーカーの添字。範囲は [0, NUM_GAME_WORKERS)。
 */
inline uint8_t zoneOfMap(uint32_t map_id) {
  return static_cast<uint8_t>(map_id % runtime_config::NUM_GAME_WORKERS);
}

/**
 * @brief ネットワーク層からロジック層への受信メッセージの種類
 *
 * @note
 * - Connect/Disconnect はネットワークスレッドが生成し、Data はクライアントのペイロードを運ぶ。
 */
enum class MsgType : uint8_t {
  Connect,    // 接続確立
  Disconnect, // 接続切断
  Data,       // 通常のデータフレーム
};

/**
 * @brief ネットワーク層からロジック層への受信メッセージ
 *
 * @note
 * - payload はメッセージオブジェクトが所有する。消費側はムーブしてもコピーしてもよい。
 * - gracefulDisconnect=true は正常な切断手順であることを表し、異常切断として扱ってはならない。
 */
struct InboundMsg {
  uint64_t connId;
  uint32_t seq;
  MsgType type;
  std::string payload;
  bool gracefulDisconnect = false;
};

/**
 * @brief ロジック層からネットワーク層への送信メッセージ
 *
 * @note
 * - payload はメッセージオブジェクトが所有し、network::send が対応する接続に渡す。
 * - connId は現在のプロセスで有効な接続のものであること。
 */
struct OutboundMsg {
  uint64_t connId;
  uint32_t seq;
  std::string payload;
};

/**
 * @brief WebSocket 接続 1 本分の実行時メタデータ
 *
 * @note
 * - uid/roleId は join チケットまたは passToken の検証後にだけ書き込む。
 * - passToken は再接続の upgrade 時に一時保存する値で、EnterZone が成功すると新しく発行したトークンに置き換わる。
 * - zoneId=0 は未バインドまたはクリア済みを表す。countedOnline=true の場合、切断時に Redis の online を必ずデクリメントする。
 * - enteredZone=false は、WebSocket が upgrade の事前チェック後で RequestEnterZone が完了する前の段階にあることを表す。
 *   この間は業務メッセージを転送してはならない。
 */
struct SessionData {
  uint64_t connId = 0;
  std::string uid;       // ユーザー ID（join チケットまたは passToken の検証後に格納。読み取り専用）
  std::string roleId;   // zone DB から検索／作成したもの
  std::string passToken; // 再接続の upgrade 後は古いトークンを一時保存。EnterZone 成功後に新しく発行したトークンに置き換える
  uint32_t currentMapId = 0; // 0: None
  uint32_t zoneId = 0; // WebSocket がバインドされている gateway の zones.id。0 は未バインドまたはクリア済み
  bool countedOnline = false; // true なら切断時に zone:<id>:online を必ずデクリメントする
  bool isReconnect = false;
  bool enteredZone = false;
};

/**
 * @brief 受信メッセージのキュー
 *
 * @note
 * - ゲームワーカーごとに分かれており、ネットワークスレッドが生成し、ロジックスレッドが消費する。
 * - ConcurrentQueue がメッセージオブジェクトを所有する。payload の寿命はキューの要素と一緒に移動する。
 */
inline std::array<moodycamel::ConcurrentQueue<InboundMsg>, runtime_config::NUM_GAME_WORKERS>
    gInbound;

/**
 * @brief conn_id からゲームワーカーへのルーティング索引
 *
 * @note
 * - gConnZoneMtx がこの map を保護する。読み書きの際は対応する shared/unique ロックを必ず取ること。
 * - 接続が閉じたら必ず削除すること。古い connId が解放済みのセッションへルーティングされるのを防ぐ。
 */
inline std::unordered_map<uint64_t, uint8_t> gConnZone;
inline std::shared_mutex gConnZoneMtx;

namespace network {
/**
 * @brief 指定したクライアント接続へペイロードを送信する
 *
 * @param connId 現在のプロセスで有効な接続 ID。
 * @param payload 呼び出し側からネットワーク層に所有権を移す送信バイト列。
 *
 * @note
 * - 実装は zone_http/uWS が提供する。呼び出し側は、ソケットへの書き込みが同期的に行われると仮定しないこと。
 */
void send(uint64_t connId, std::string payload);
} // namespace network
