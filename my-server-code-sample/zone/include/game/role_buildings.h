#pragma once
#include <cstdint>
#include <vector>

/**
 * @file role_buildings.h
 * @brief キャラクターのメインシティにある建物の実行時状態
 *
 * DB の role_buildings に対応する。(role_id, building_id) は一意。
 * ログイン時に GameSession に読み込み、シティ生成時にこれをもとに建物エンティティを作る。
 */
namespace buildings {

/// role_buildings.state：0=ロック 1=解放済み
inline constexpr uint8_t kStateLocked = 0;
inline constexpr uint8_t kStateUnlocked = 1;

/**
 * @brief キャラクターが所有する建物 1 つ
 *
 * @note
 * - buildingId は buildings.id であり、プロトコルの building_kind でもある。
 * - state == kStateUnlocked の建物だけがエンティティ化され、クライアントに送られる。
 */
struct RoleBuilding {
  uint32_t buildingId = 0;
  float    x = 0.f;
  float    y = 0.f;
  uint32_t level = 1;
  uint8_t  state = kStateLocked;
};

/**
 * @brief キャラクターの全建物の状態
 *
 * @note
 * - GameSession が所有する。読み書きは、その接続を担当するゲームワーカーのスレッドからのみ行う。
 * - entries 内で buildingId は一意。
 */
struct RoleBuildings {
  std::vector<RoleBuilding> entries;
};

/**
 * @brief キャラクターが所有する建物を検索する
 *
 * @param rb 借用する建物の状態。
 * @param buildingId buildings.id。
 *
 * @retval nullptr そのキャラクターにこの建物のレコードがない。
 *
 * @note
 * - 返すポインタは rb 内部の要素を指す。要素を追加・削除すると無効になる。
 */
inline const RoleBuilding *find(const RoleBuildings &rb, uint32_t buildingId) {
  for (const auto &e : rb.entries) {
    if (e.buildingId == buildingId) return &e;
  }
  return nullptr;
}

/**
 * @brief 建物が解放済みかどうかを判定する
 *
 * @retval false レコードがない、またはロック状態。
 */
inline bool isUnlocked(const RoleBuildings &rb, uint32_t buildingId) {
  const RoleBuilding *const e = find(rb, buildingId);
  return e != nullptr && e->state == kStateUnlocked;
}

/**
 * @brief 建物の状態を書き込む（なければ追加、あれば更新）
 *
 * @param[in,out] rb 建物の状態。
 * @param entry 書き込むレコード。同じ buildingId のレコードを上書きする。
 */
inline void upsert(RoleBuildings &rb, const RoleBuilding &entry) {
  for (auto &e : rb.entries) {
    if (e.buildingId != entry.buildingId) continue;
    e = entry;
    return;
  }
  rb.entries.push_back(entry);
}

} // namespace buildings
