#pragma once
#include <cstdint>

/**
 * @brief 地面に落ちているドロップアイテムのコンポーネント
 *
 * @note
 * - ドロップアイテムのエンティティにだけ付与する。このエンティティは NetworkId と
 *   UnitBase(unitKind=DropItem) も持つため、通常の AOI 差分でクライアントに届く。
 *   ドロップ専用のプロトコルは不要。
 * - expireAtMs はマップインスタンスの SimClock（ミリ秒）基準で、snapshotTimeMsForInstance と同じ時計。
 *   WallClock と混ぜるとマシンの時刻ずれで期限判定が狂うので注意。
 * - 期限切れ・拾得後は所有側がエンティティを破棄し、AOI の left 経由でクライアントに通知する。
 */
struct CPickableItem {
  uint32_t itemId = 0;
  uint32_t count = 1;
  uint64_t expireAtMs = 0;
};
