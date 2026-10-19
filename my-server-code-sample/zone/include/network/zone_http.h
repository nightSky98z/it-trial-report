#pragma once
#include "config/zone_config.h"
#include <cstdint>
// ── ゾーンサーバーの HTTP API + WebSocket サーバー ─────────────────
// 1) HTTP API：ゲートウェイからのキャラクター照会・作成、マップのリスポーン地点照会。
//    内部トークン必須。
// 2) WebSocket：クライアントが直接接続してゲームをプレイする。
//
// HTTP エンドポイント：
//   GET  /api/role?user_id=123          キャラクター照会
//   POST /api/role/create {user_id:123} キャラクター作成
//   GET  /api/map-respawn?map_id=1      マップのリスポーン地点照会
//   Header: X-Zone-Internal-Token は ZONE_INTERNAL_API_TOKEN と一致する必要がある
//
// WebSocket エンドポイント：
//   /game/ws?methodId=3&ticket=hex      初回接続（gateway の /join が発行）
//   /game/ws?passToken=xxx              再接続（upgrade 時は事前チェックのみ、EnterZone で消費）

namespace zone_http {

/**
 * @brief HTTP + WebSocket サーバーを実行する（呼び出したスレッドをブロックする）
 *
 * @param workerId 現在のネットワークワーカー ID。conn_id の上位ビットに埋め込む。
 * @param cfg loadFromEnvFile で初期化済みの実行時設定。listen・zoneId・
 *            チケット鍵・MySQL 関連の項目は必須。Redis や内部トークンは
 *            それぞれの認証・API 経路の動作に影響する。
 *
 * @note
 * - 呼び出し前に cfg.zoneId が gateway の zones.id として有効であることを保証すること。
 * - Redis の DB/キー名前空間は gateway の /join がチケットを書き込む設定と一致させること。
 *   ずれていると、実行時に join チケットと passToken の経路がリクエストを拒否する。
 */
void run(uint8_t workerId, const zone::ZoneConfig &cfg);

} // namespace zone_http
