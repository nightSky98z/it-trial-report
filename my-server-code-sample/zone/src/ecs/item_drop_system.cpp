#include "ecs/item_drop_system.h"
#include "ecs/components.h"
#include "utils/log.h"
#include <random>

namespace item_drop_system {
namespace {

/**
 * @brief ドロップ判定用の乱数源
 *
 * @note
 * - ゲームワーカーごとに専用のインスタンスを持つ（thread_local）。ドロップ判定はそのワーカーの
 *   tick 内でしか行われずスレッドをまたがないため、ロックは不要。
 */
float nextRoll01() {
  static thread_local std::mt19937 rng{std::random_device{}()};
  static thread_local std::uniform_real_distribution<float> dist(0.f, 1.f);
  return dist(rng);
}

} // namespace

/**
 * @brief 重みに従ってドロップアイテムを選ぶ
 *
 * @note
 * - 純粋関数。乱数値を呼び出し側から受け取るので、テストで結果を固定できる。
 */
uint32_t pickDropItem(const std::vector<cache::DropEntry> &drops,
                      float roll01) {
  if (drops.empty()) return 0;

  float total = 0.f;
  for (const auto &d : drops) total += d.rate;
  if (total <= 0.f) return 0;

    // 重みの合計が 1 未満なら「何も落とさない」区間を残す。roll が [total, 1) に入ったらドロップなし。
  const float scale = total < 1.f ? 1.f : total;
  const float roll = roll01 * scale;

  float cumulative = 0.f;
  for (const auto &d : drops) {
    cumulative += d.rate;
    if (roll <= cumulative) return d.itemId;
  }
  return 0;
}

/**
 * @brief ドロップアイテムのエンティティを生成する
 *
 * @note
 * - 既存の AOI 配信経路を再利用するため UnitBase を付けている。戦闘コンポーネントは持たないので、
 *   索敵対象にならず、衝突判定や戦闘計算にも関わらない。
 */
void spawnDropsForKills(MapInstance &inst, const cache::TemplateCache &tc,
                        const std::vector<UnitKilledEvent> &killed,
                        uint64_t nowMs) {
  for (const auto &ev : killed) {
    const auto tplIt = tc.units.find(ev.victimUnitId);
    if (tplIt == tc.units.end()) continue;
    const auto &tpl = tplIt->second;
    if (tpl.drops.empty()) continue;

    const uint32_t itemId = pickDropItem(tpl.drops, nextRoll01());
    if (itemId == 0) continue;

    const auto ent = inst.world.create();
    const uint32_t netId = inst.networkEntityIdAlloc.alloc();
    inst.world.emplace<NetworkId>(ent, NetworkId{netId});
    inst.world.emplace<UnitBase>(ent, UnitBase{
        .unitId = itemId,
        .campId = kDropItemCampId,
        .unitKind = kDropItemUnitKind,
        .x = ev.death_x,
        .y = ev.death_y,
        .curHp = 1,
        .maxHp = 1,
        .lifeStatus = 0,
        .actionState = 0,
        .level = 1,
    });
    inst.world.emplace<CPickableItem>(ent, CPickableItem{
        .itemId = itemId,
        .count = 1,
        .expireAtMs = nowMs + kDropExpireMs,
    });

    zone_log::debug("drop spawned item=%u at (%g,%g) netId=%u", itemId,
                    ev.death_x, ev.death_y, netId);
  }
}

/**
 * @brief 期限切れのドロップアイテムを片付ける
 *
 * @note
 * - いったん集めてから破棄する。view を走査しながら destroy するとイテレータが無効になるため。
 */
void cleanupExpiredDrops(MapInstance &inst, uint64_t nowMs,
                         std::vector<uint32_t> &outRemovedNetworkIds) {
  std::vector<entt::entity> expired;
  auto view = inst.world.view<NetworkId, CPickableItem>();
  for (auto e : view) {
    const auto &pick = view.get<CPickableItem>(e);
    if (pick.expireAtMs > nowMs) continue;
    expired.push_back(e);
    outRemovedNetworkIds.push_back(view.get<NetworkId>(e).value);
  }
  for (const auto e : expired) {
    inst.world.destroy(e);
  }
}

} // namespace item_drop_system
