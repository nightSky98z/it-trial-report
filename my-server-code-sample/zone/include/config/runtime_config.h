#pragma once

#include <cstdint>
namespace runtime_config {

/**
 * @brief サーバーとクライアントで取り決めたワールド座標の単位比率
 *
 * @note
 * - 1 world_unit = 1 Cocos pixel。原点はマップ／シーンの中心で、Y 軸は上向きが正。
 * - クライアントと固定で合わせている。変更する場合はプロトコル、マップ設定、
 *   フロントエンドの座標変換もあわせて修正すること。
 */
inline constexpr float kWorldUnitsPerPixel = 1.0f;

/**
 * @brief MySQL 接続の TLS モード
 *
 * @note
 * - Disabled は内部ネットワークでの調査用途にとどめること。
 * - Required はクライアント側で TLS を必須にする。本番環境ではこちらを優先する。
 */
enum class MySqlXTlsMode {
  Disabled = 0,
  Required = 1,
};

/**
 * @brief ビルド時に決まる MySQL の TLS モード
 *
 * @note
 * - この定数を変えると、すべての MySQL Session の作成に影響する。
 *   （名前の X は X DevAPI 時代の名残で、現在は従来プロトコル接続に適用される）
 */
inline constexpr MySqlXTlsMode kMySqlXTlsMode = MySqlXTlsMode::Disabled;

/**
 * @brief ネットワークワーカーの数
 *
 * @note
 * - 現在の game_loop / zone_http はワーカー 1 つを前提にしている。増やす前に共有状態を見直すこと。
 */
inline static constexpr uint8_t NUM_NET_WORKERS = 1;

/**
 * @brief ゲームロジックワーカーの数
 *
 * @note
 * - 現在のマップのスケジューリングと ctx の状態は、ゲームワーカー 1 つを前提に設計している。
 */
inline static constexpr uint8_t NUM_GAME_WORKERS = 1;

/**
 * @brief passToken の有効期間（秒）
 *
 * @note
 * - 再接続やマップ入場の認可に使う。現在は 5 時間。
 */
inline static constexpr int32_t PASS_TOKEN_SEC = 5 * 60 * 60;

} // namespace runtime_config
