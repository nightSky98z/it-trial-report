#pragma once
#include "ecs/components.h"
#include "game/template_cache.h"
#include <cstdint>
#include <entt/entt.hpp>

namespace city_monster_factory {

/**
 * @brief 指定したスポーン地点にシティモンスターを 1 体生成する
 *
 * @param world マップの ECS world。モンスターを生成し、実行時コンポーネントを付与する。
 * @param alloc networkEntityId のアロケータ。生成に成功するとプロトコル用 ID を 1 つ消費する。
 * @param tc テンプレートキャッシュ。読み取りのみで、参照は保持しない。
 * @param spawnPointId area_spawn_points.id。
 * @param unitId unit_templates.unit_id。
 * @param campId 陣営 ID。
 * @param mapId 現在のマップ ID。spawnPointId が属するマップと一致している必要がある。
 *
 * @return 生成したエンティティ。
 * @retval entt::null spawnPointId・unitId・mapId、またはエリア境界の設定が見つからない場合。
 *
 * @note
 * - マップ初期化時と、monster_lifecycle_system のリスポーン処理の両方から呼ばれる。
 * - entt::null が返った場合、呼び出し側はそのエンティティにコンポーネントを書き込まないこと。
 */
entt::entity spawnAtPoint(entt::registry &world,
                          NetworkEntityIdAllocator &alloc,
                          const cache::TemplateCache &tc,
                          uint64_t spawnPointId,
                          uint32_t unitId,
                          uint32_t campId,
                          uint32_t mapId);

} // namespace city_monster_factory
