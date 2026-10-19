#pragma once
#include "ecs/map_components.h"
#include "game/template_cache.h"
#include <cstdint>
#include <entt/entt.hpp>
#include <string>
#include <vector>

/**
 * @file boss_challenge_system.h
 * @brief ボス挑戦の生成、フィールドの片付け、戦闘後の後処理
 *
 * ボスに専用の設定テーブルはない。ボスは unit_templates の camp_id=2 のテンプレートで、
 * 生成も通常モンスターと同じ city_monster_factory の処理を通る。
 */
namespace boss_challenge_system {

/**
 * @brief ボスであることを示すタグコンポーネント
 *
 * @note
 * - 現在生存しているボスのエンティティにだけ付ける。1 つのマップインスタンスに同時にいるボスは最大 1 体。
 * - 撃破時の処理はこれでボスを識別し、報酬の付与と通常モンスターの再配置を行う。
 */
struct CBossTag {
  uint32_t bossUnitId = 0;
    // ボスがいるエリア。ボスを倒した後は編成をここへ割り当て直して雑魚との戦闘を続けさせるため、
    // エリアをボスのエンティティに記録しておく必要がある（エンティティが破棄された後では調べられない）。
  uint32_t areaId = 0;
};

/**
 * @brief 挑戦に失敗した理由
 *
 * @note
 * - 呼び出し側はこれをもとにプレイヤー向けの文言を作る。システム内部で UI のテキストを組み立てないこと。
 */
enum class ChallengeError : uint8_t {
  None = 0,
  UnknownBoss,      // テンプレートが存在しない
  NotABoss,         // テンプレートはあるがボス陣営ではない
  BossAlive,        // 生存中のボスがいるため、重ねて挑戦できない
  NoSpawnPoint      // このマップにメインシティ以外の使用可能な出現地点がない
};

/**
 * @brief ボス挑戦を開始する
 *
 * @param inst 対象のマップインスタンス。フィールドを片付けてボスのエンティティを作る。
 * @param tc テンプレートキャッシュ。読み取りのみ。
 * @param bossUnitId 挑戦するボスのテンプレート ID。
 * @param[out] outRemovedNetworkIds 片付けたモンスターのプロトコル ID を追記する。AOI の left で配信するために使う。
 * @param[out] outBossAreaId 成功時、ボスがいるエリア ID を書き込む。
 *
 * @return ChallengeError::None なら挑戦が始まった。
 *
 * @note
 * - 失敗時はマップインスタンスを変更せず、outRemovedNetworkIds にも追記しない。
 * - 成功後、呼び出し側は outRemovedNetworkIds をそのインスタンスの全接続に配信し、
 *   プレイヤーの編成を outBossAreaId へ割り当てること。
 */
ChallengeError startChallenge(MapInstance &inst, const cache::TemplateCache &tc,
                              uint32_t bossUnitId,
                              std::vector<uint32_t> &outRemovedNetworkIds,
                              uint32_t &outBossAreaId);

/**
 * @brief 倒されたユニットがボスかどうかを判定し、そのテンプレート ID を返す
 *
 * @param world マップの ECS world。読み取りのみ。
 * @param victim 倒されたエンティティ。既に無効になっていることもある。
 *
 * @return ボスのテンプレート ID。
 * @retval 0 ボスではない、または既に無効なエンティティ。
 */
uint32_t bossUnitIdOf(const entt::registry &world, entt::entity victim);

/**
 * @brief 倒されたボスがいたエリアを取得する
 *
 * @param world マップの ECS world。読み取りのみ。
 * @param victim 倒されたエンティティ。
 *
 * @return ボスがいたエリアの ID。
 * @retval 0 ボスではない、既に無効、または挑戦時にエリアが記録されていない。
 *
 * @note
 * - ボスのエンティティが破棄される前に読むこと。
 */
uint32_t bossAreaIdOf(const entt::registry &world, entt::entity victim);

/**
 * @brief ボスを倒した後に通常モンスターを元に戻す
 *
 * @param inst 対象のマップインスタンス。固定スポーン設定に従って通常モンスターを作り直す。
 * @param tc テンプレートキャッシュ。読み取りのみ。
 *
 * @note
 * - ボスの死亡を確認した後に 1 回だけ呼ぶこと。複数回呼ぶとモンスターが重複して出現する。
 */
void respawnNormalMonsters(MapInstance &inst, const cache::TemplateCache &tc);

} // namespace boss_challenge_system
