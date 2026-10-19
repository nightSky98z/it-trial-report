#pragma once
#include <cstdint>

/**
 * @brief 建物エンティティのコンポーネント
 *
 * @note
 * - buildingId は DB の buildings.id であり、プロトコルの building_id や BuildingKind の値と同一。
 *   フロントエンドの building_visual_catalog も同じ番号体系を使うため、別の対応表は作らないこと。
 * - networkEntityId は現在の MapInstance 内でのプロトコル／AOI 用の主キー。
 * - x/y はワールド座標。state の意味は role_buildings.state と同じ（0=ロック、1=解放済み）。
 * - 解放済みの建物だけがエンティティ化されるため、エンティティが存在する時点で state=1 である。
 */
struct CBuilding {
  uint32_t buildingId = 0;
  uint32_t networkEntityId = 0;
  float x = 0.f;
  float y = 0.f;
  uint32_t level = 1;
  uint8_t  state = 1;
};
