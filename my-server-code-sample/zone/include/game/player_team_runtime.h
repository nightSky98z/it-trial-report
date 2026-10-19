#pragma once
#include "game/equip_stats.h"

#include "ecs/components.h"
#include <cstdint>
#include <string>
#include <vector>

namespace cache {
struct TemplateCache;
}

namespace codec {
struct RoleInfoDto;
struct TeamUnitInfoDto;
}

namespace game {

/**
 * @brief プレイヤーの編成がマップに入る前の、エンティティ再利用／後始末の結果
 *
 * @note
 * - removedNetworkEntityIds は結果オブジェクトが所有する。クライアントに古いエンティティの削除を通知するのに使う。
 * - reusedExistingUnits=true は、今回プレイヤー船のエンティティを作り直さなかったことを表す。
 */
struct PlayerTeamEnterMapResult {
  std::vector<uint32_t> removedNetworkEntityIds;
  bool reusedExistingUnits = false;
};

/**
 * @brief 指定キャラクターが現在所有している networkEntityId を集める
 *
 * @param world マップの ECS world。registry を借用するだけ。
 * @param roleId キャラクター ID。文字列は呼び出し中だけ借用する。
 *
 * @return networkEntityId の一覧（値で返す）。空ならそのキャラクターのエンティティはまだ生成されていない。
 */
std::vector<uint32_t>
collectNetworkEntityIdsForRole(entt::registry &world, const std::string &roleId);

/**
 * @brief 指定キャラクターの、現在のマップ上の全エンティティを破棄する
 *
 * @param world マップの ECS world。registry を構造的に変更する。
 * @param roleId キャラクター ID。文字列は呼び出し中だけ借用する。
 *
 * @note
 * - エンティティのコンポーネントに依存する永続化やログアウト通知は、呼び出し前に済ませておくこと。
 * - 呼び出し後、それまでに保持していたそのキャラクターのエンティティハンドルはすべて無効になる。
 */
void destroyEntitiesForRole(entt::registry &world, const std::string &roleId);

/**
 * @brief キャラクターの編成設定に従ってプレイヤー船のエンティティを生成する
 *
 * @param world マップの ECS world。エンティティを生成し、実行時コンポーネントを付与する。
 * @param alloc networkEntityId のアロケータ。新しいプロトコル用エンティティ ID を消費する。
 * @param roleId キャラクター ID。フレームをまたいで保持する必要のある文字列はコンポーネントにコピーする。
 * @param roleDto ゲートウェイ／DB から組み立てたキャラクターの編成 DTO。呼び出し中だけ借用する。
 * @param tc テンプレートキャッシュ。読み取りのみで、参照は保持しない。
 * @param spawnX 初期位置のワールド X 座標。
 * @param spawnY 初期位置のワールド Y 座標。
 *
 * @note
 * - 呼び出し前に、roleDto 内のユニットテンプレートがすべて tc に存在することを保証すること。
 * - 生成したエンティティは world が所有する。解放は destroyEntitiesForRole か world の破棄で行われる。
 */
void spawnPlayerTeamUnits(entt::registry &world,
                          NetworkEntityIdAllocator &alloc,
                          const std::string &roleId,
                          const codec::RoleInfoDto &roleDto,
                          const cache::TemplateCache &tc,
                          const cache::ItemCache &items,
                          const equipment::RoleEquipment &equipment,
                          const std::vector<codec::TeamUnitInfoDto> &ownedUnits,
                          float spawnX,
                          float spawnY,
                          uint32_t maxLevel);

/**
 * @brief プレイヤーの精霊エンティティを 1 体生成する
 *
 * @param world マップの ECS world。エンティティを生成し、実行時コンポーネントを付与する。
 * @param alloc networkEntityId のアロケータ。
 * @param roleId 所有キャラクターの ID。
 * @param unit その精霊の実行時データ（レベル／経験値／エリア）。
 * @param tc テンプレートキャッシュ。
 * @param items アイテム設定。装備ボーナスの計算に使う。
 * @param equipment キャラクターの装備状態。
 * @param spawnX 初期位置のワールド X 座標。
 * @param spawnY 初期位置のワールド Y 座標。
 *
 * @note
 * - 募集後は新しく加わった 1 体だけを生成し、全体を作り直さないこと。
 *   作り直すと他の精霊のプロトコル上のエンティティ ID が変わり、クライアントの静的キャッシュに古いエントリが溜まっていく。
 */
void spawnSinglePlayerUnit(entt::registry &world,
                           NetworkEntityIdAllocator &alloc,
                           const std::string &roleId,
                           const codec::TeamUnitInfoDto &unit,
                           const cache::TemplateCache &tc,
                           const cache::ItemCache &items,
                           const equipment::RoleEquipment &equipment,
                           float spawnX, float spawnY, bool isDeployed,
                           uint32_t maxLevel);

/**
 * @brief プレイヤー船の戦闘用の基礎ステータス（装備ボーナスを含まない）
 *
 * @note
 * - unit_templates の基礎値にレベルによる成長を加えたもので、装備ボーナスを加算する土台になる。
 */
struct PlayerShipBaseStats {
  uint32_t hp = 100;
  uint32_t atk = 10;
  float    speed = 120.f;
  float    attackRange = 1.f;
  uint32_t campId = 1;
};

/**
 * @brief プレイヤー船の基礎ステータスを計算する
 *
 * @param tc テンプレートキャッシュ。読み取りのみ。
 * @param unitId unit_templates.unit_id。
 * @param level 現在のレベル（最小 1）。
 *
 * @return そのレベルでの基礎ステータス。テンプレートがなければ組み込みのデフォルト値を返す。
 *
 * @note
 * - エンティティの生成時と、装備変更後のステータス再計算は必ずこの関数を共用すること。2 か所で成長式がずれるのを防ぐ。
 */
PlayerShipBaseStats baseStatsForUnit(const cache::TemplateCache &tc,
                                     uint32_t unitId, uint32_t level);

/**
 * @brief 現在の装備に基づいて、プレイヤー船の戦闘ステータスを再計算する
 *
 * @param world マップの ECS world。そのユニットのステータスコンポーネントをその場で更新する。
 * @param roleId ユニットを所有するキャラクター。このキャラクターのユニットだけを処理する。
 * @param unitId 対象の精霊のテンプレート ID。
 * @param tc テンプレートキャッシュ。読み取りのみ。
 * @param items 読み取り専用のアイテム設定。
 * @param equipment キャラクターの現在の装備状態。
 *
 * @retval false そのキャラクターの該当するユニットのエンティティが見つからない（プレイヤーがマップにいない場合など）。
 *
 * @note
 * - 最大 HP が変わったら現在 HP も同じだけ増減させ、常に [1, maxHp] に収める。
 *   装備を外しただけでユニットが倒れてしまうことがあってはならない。
 * - 変更するのはステータスだけで、レベルアップや死亡などの状態遷移は起こさない。
 */
bool refreshUnitCombatStats(entt::registry &world, const std::string &roleId,
                            uint32_t unitId, const cache::TemplateCache &tc,
                            const cache::ItemCache &items,
                            const equipment::RoleEquipment &equipment);

/**
 * @brief プレイヤーの編成をマップに入場させる準備をする
 *
 * @param world マップの ECS world。エンティティを再利用・破棄・生成することがある。
 * @param alloc networkEntityId のアロケータ。新しいエンティティを作るときだけ ID を消費する。
 * @param roleId キャラクター ID。文字列は呼び出し中だけ借用する。
 * @param roleDto 現在のキャラクターの編成 DTO。呼び出し中だけ借用する。
 * @param tc テンプレートキャッシュ。読み取りのみで、参照は保持しない。
 * @param spawnX 新しく作るエンティティの初期位置のワールド X 座標。
 * @param spawnY 新しく作るエンティティの初期位置のワールド Y 座標。
 * @param reuseExistingUnits true なら既存のエンティティの再利用を優先する。false なら先に片付けてから作り直す。
 *
 * @return removedNetworkEntityIds: クライアントが削除すべき古いプロトコル用エンティティ ID。
 * @return reusedExistingUnits: 既存のエンティティを再利用したかどうか。
 *
 * @note
 * - removedNetworkEntityIds を対応するクライアントに送る、または AOI の後始末に使うのは呼び出し側の責任。
 */
PlayerTeamEnterMapResult preparePlayerTeamUnitsForEnterMap(
    entt::registry &world,
    NetworkEntityIdAllocator &alloc,
    const std::string &roleId,
    const codec::RoleInfoDto &roleDto,
    const cache::TemplateCache &tc,
    const cache::ItemCache &items,
    const equipment::RoleEquipment &equipment,
    const std::vector<codec::TeamUnitInfoDto> &ownedUnits,
    float spawnX,
    float spawnY,
    bool reuseExistingUnits,
    uint32_t maxLevel);

} // namespace game
