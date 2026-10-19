#pragma once
#include <array>
#include <cstdint>
#include <entt/entt.hpp>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ═════════════════════════════════════════════════════════════════════════════
// 第 1 層 ECS：ネットワーク／セッション／マップインスタンスの管理
// ═════════════════════════════════════════════════════════════════════════════

/**
 * @brief メイン world における接続エンティティのネットワーク上の識別子
 *
 * @note
 * - connId は network::makeConnId で作られ、現在のプロセスが動いている間だけ有効。
 * - 切断時の後始末では、ZoneRegistry::conn_entity の逆引き索引も必ず同時に削除すること。
 */
struct NetConn {
  uint64_t connId;
};

/**
 * @brief ゾーンサーバーに入ったキャラクターの識別情報
 *
 * @note
 * - roleId はデータベースと業務の主キー。roleName は表示専用。
 * - mapId はキャラクターが現在いるマップのテンプレート ID で、MapInstance のエンティティを表すものではない。
 */
struct Role {
  std::string roleId;
  std::string roleName;
  uint32_t mapId = 0;
};

/**
 * @brief WebSocket 接続が join の検証を終えた後にキャッシュするキャラクター情報
 *
 * @note
 * - メイン world の接続エンティティに付け、EnterMap と再接続の処理で使う。
 * - passToken は zone 内部の 2 段階のマップ入場用の資格情報。有効期限は壁時計のミリ秒で判定する。
 * - isReconnect は、この接続が古い接続を置き換えたことを表す。キャラクターの初回作成として扱ってはならない。
 */
struct ConnRole {
  std::string uid;
  std::string roleId;
  std::string roleName;
  uint32_t mapId = 1;
  std::string passToken;
  uint64_t passTokenExpireAtMs = 0;
  bool isReconnect = false;
};

/**
 * @brief ログイン／マップ入場のデータを読み込み中であることを示すタグコンポーネント
 *
 * @note
 * - 状態を表すだけで、外部のリソースは所有しない。
 * - これが付いている間は、接続をマップインスタンスの配信対象に入れないこと。
 */
struct PendingLoad {};

/**
 * @brief マップ入場のリクエストは受け取ったが、DB の検証とマップの読み込みがまだ終わっていない
 *
 * @note
 * - roleId/roleName/mapId は検証済みの接続コンテキストから取る。
 * - 現在の実装ではフレームをまたいで使わない。非同期読み込みを有効にする場合は、タイムアウトと失敗時のロールバックを明確にすること。
 */
// HINT: バッグやスキルなどのプレイヤーデータをフレームをまたいで非同期に読み込むようになったら有効にする。
// その代わり、PendingEnter が読み込み状態、失敗時のロールバックの範囲、タイムアウト時の後始末の責任を明確に持つ必要がある。
struct PendingEnter {
  std::string roleId;
  std::string roleName;
  uint32_t mapId = 0;
};

/**
 * @brief マップインスタンスの動作モード
 *
 * @note
 * - City はメインシティの常駐ロジックを使う。Dungeon はダンジョンのスケジューリング用に予約している。
 * - モードを追加する場合は、game_loop のインスタンス作成・GC・tick の振り分けもあわせて修正すること。
 */
enum class MapMode : uint8_t {
  City    = 0,
  Dungeon = 1,
};

/**
 * @brief マップインスタンス内のプロトコル／AOI 用 ID のアロケータ
 *
 * @note
 * - 同じ MapInstance.world 内の NetworkId にだけ番号を振る。マップインスタンスごとに 1 から始まる。
 * - 0 は不正な ID として予約している。network_entity_id は現在の MapInstance 内でだけ有効。
 * - NetworkId を、サーバー全体やマップをまたいで変わらない ID として保存しないこと。
 */
struct NetworkEntityIdAllocator {
  uint32_t next = 1;

    /**
     * @brief マップ内のネットワーク用エンティティ ID を新しく 1 つ割り当てる
     *
     * @return 0 以外の ID なら NetworkId に書き込める。
     * @retval 0 uint32_t の ID 空間を使い切った。呼び出し側はエンティティの生成を拒否すること。
     */
  uint32_t alloc() {
    if (next == 0) {
      return 0;
    }
    const uint32_t id = next;
    ++next;
    return id;
  }
};

/**
 * @brief マップインスタンスの固定ステップのシミュレーション時計
 *
 * @note
 * - tick は MapInstance.world の正となるシミュレーションのフレーム番号。
 * - tickRate が 0 は無効。変更は必ず setMapSimTickRate で行うこと。
 */
struct MapSimClock {
  uint64_t tick = 0;
  uint32_t tickRate = 30;
  float fixedDtSec = 1.f / 30.f;
};

/**
 * @brief マップのシミュレーションの tick rate を設定する
 *
 * @param clock マップインスタンスのシミュレーション時計。
 * @param tickRate 1 秒あたりのロジックフレーム数。0 は無効で、元の値のままにする。
 *
 * @note
 * - 変更すると fixedDtSec も同時に更新される。呼び出し側は fixedDtSec に直接書き込まないこと。
 */
inline void setMapSimTickRate(MapSimClock &clock, uint32_t tickRate) {
  if (tickRate == 0) {
    return;
  }
  clock.tickRate = tickRate;
  clock.fixedDtSec = 1.f / static_cast<float>(tickRate);
}

/**
 * @brief シミュレーションを 1 tick 進める
 *
 * @param clock マップインスタンスのシミュレーション時計。
 *
 * @note
 * - 固定ステップ 1 回分の処理が終わるたびに 1 回呼ぶ。
 */
inline void advanceMapSimTick(MapSimClock &clock) {
  ++clock.tick;
}

/**
 * @brief シミュレーションの tick をロジック時刻（ミリ秒）に変換する
 *
 * @param clock マップインスタンスのシミュレーション時計。
 *
 * @return 現在の tick に対応するミリ秒。tickRate が 0 なら 0 を返す。
 */
inline uint64_t mapSimTimeMs(const MapSimClock &clock) {
  if (clock.tickRate == 0) {
    return 0;
  }
  return (clock.tick * 1000ull) / static_cast<uint64_t>(clock.tickRate);
}

/**
 * @brief マップインスタンスの AOI 送信用の時計
 *
 * @note
 * - tick は AOI の送信番号で、シミュレーションの tick とは別物。
 * - pushElapsedSec は game_loop が積算し、送信間隔に達したら 0 に戻すか差し引く。
 */
struct MapAoiClock {
  uint64_t tick = 0;
  float pushElapsedSec = 0.f;
};

/**
 * @brief マップの閲覧者エンティティ
 *
 * @note
 * - メイン world の viewer エンティティに付け、MapInstance.world には入れない。
 * - mapInstance はメイン world の MapInstance エンティティを指す。connId/roleId は配信時の絞り込みに使う。
 */
struct MapViewer {
  entt::entity mapInstance = entt::null;
  std::string roleId;
  uint64_t connId = 0;
};

/**
 * @brief AOI 閲覧者のカメラ矩形
 *
 * @note
 * - center/halfWidth/halfHeight は、プレイヤーの位置やビューポートの設定に応じて更新する。
 * - dirty は、閲覧対象の集合を計算し直す必要があることを表す。
 */
struct ViewerCamera {
  struct Center {
    float x = 0.f;
    float y = 0.f;
  };
  Center center;
  float halfWidth = 500.f;
  float halfHeight = 1000.f;
  bool dirty = false;
};

/**
 * @brief マップインスタンスのコンポーネント
 *
 * @note
 * - メイン world の MapInstance エンティティに付ける。
 * - world は第 2 層 ECS の所有者で、マップ内のゲームの正となるオブジェクトを格納する。
 * - connIds/viewerEntities は借用的なハンドルを保持するだけ。接続や viewer を破棄したら必ず同時に削除すること。
 * - emptyTimestampMs は connIds が空のときだけ有効で、アイドル状態のインスタンスの GC に使う。
 */
struct MapInstance {
  uint32_t mapId = 0;           // マップテンプレート ID（map_templates に対応）
  MapMode  mapMode = MapMode::City;
  uint8_t  maxPlayers = 1;      // このマップに入れる最大プレイヤー数
  entt::registry world = {};    // 第 2 層 ECS：マップ／ダンジョン内のゲームの正となるオブジェクトだけを置く
  NetworkEntityIdAllocator networkEntityIdAlloc; // このインスタンス専用のプロトコル／AOI 用 ID 空間
  MapSimClock simClock;         // 第 2 層 ECS のマップインスタンスのシミュレーション時刻
  MapAoiClock aoiClock;         // このマップインスタンス専用の AOI 送信リズム

    // このインスタンス内の全プレイヤーの接続（配信用）
  std::vector<uint64_t> connIds;
  std::vector<entt::entity> viewerEntities; // メイン world でこのインスタンスを閲覧している viewer エンティティ

    // ── アイドル時の回収 ──
    // 最後のプレイヤーが退出した時刻。connIds が空でないときは 0（「アイドルではない」ことを表す）
  uint64_t emptyTimestampMs = 0;
};

// ═════════════════════════════════════════════════════════════════════════════
// 第 2 層 ECS：ゲーム内のオブジェクト（ユニット、AI、戦闘など）
// map_components.h に置いている
// ═════════════════════════════════════════════════════════════════════════════
