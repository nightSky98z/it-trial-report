#pragma once
#include "game/template_cache.h"
#include "protocol/codec.h"
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

/**
 * @file roster_codec.h
 * @brief 募集リスト／ボスリスト／プレイヤー情報パネルの S2C エンコード
 *
 * 3 つのメッセージはいずれもマップ入場後に一度だけ送るもので、データは unit_templates とキャラクターのスナップショットから作る。
 * 経済システムと名簿系のシステムが互いに依存しないよう、ショップのエンコードとはファイルを分けている。
 */
namespace codec {

/**
 * @brief 陣営の意味（unit_templates.camp_id）
 *
 * @note
 * - 値は Java 版の取り決めを踏襲しており、DB の既存データもこれに従っている：
 *   0=募集可能な精霊、1=通常モンスター、2=ボス。
 * - 募集リストには Recruitable だけ、ボスリストには Boss だけを入れる。
 */
namespace camp {
inline constexpr uint32_t kRecruitable = 0;
inline constexpr uint32_t kMonster = 1;
inline constexpr uint32_t kBoss = 2;
} // namespace camp

/**
 * @brief 募集コストの倍率
 *
 * @note
 * - unit_templates には cost 列がないため、コストは base_hp から換算する：cost = base_hp × この倍率。
 *   ルールは Java 版の GameConfig.RECRUIT_COST_HP_MULTIPLIER と同じ。
 *   募集リストの表示と実際の支払いは必ず recruitCostOf を共用し、別々に計算しないこと。
 */
inline constexpr uint32_t kRecruitCostHpMultiplier = 5;

/**
 * @brief 精霊の募集コストを計算する
 *
 * @param tpl 借用するユニットテンプレート。
 *
 * @return 募集に必要なゴールド。
 *
 * @note
 * - リストの送信と募集リクエストの処理は、どちらも必ずこの関数を通すこと（2 か所で計算式がずれるのを防ぐ）。
 */
inline uint64_t recruitCostOf(const cache::UnitTemplate &tpl) {
  return static_cast<uint64_t>(tpl.baseHp) *
         static_cast<uint64_t>(kRecruitCostHpMultiplier);
}

/**
 * @brief キャラクターが指定した精霊を既に所持しているかを判定する
 *
 * @param ownedUnits 借用する所持精霊のリスト。
 * @param unitId 精霊のテンプレート ID。
 */
inline bool ownsUnit(const std::vector<TeamUnitInfoDto> &ownedUnits,
                     uint32_t unitId) {
  for (const auto &u : ownedUnits) {
    if (u.unitId == unitId) return true;
  }
  return false;
}

/**
 * @brief 募集可能な精霊のリストをエンコードする
 *
 * @param seq プッシュのシーケンス番号。
 * @param tc テンプレートキャッシュ。呼び出し中だけ借用する。
 * @param ownedUnits キャラクターが所持している精霊。owned フラグを埋めるのに使う。
 *
 * @return エンコード済みバイト列を持つ Envelope。
 *
 * @note
 * - camp_id == kRecruitable のテンプレートだけを送る。表示順が安定するよう unit_id の昇順に並べる。
 */
inline std::string encodeRecruitUnitList(
    uint32_t seq, const cache::TemplateCache &tc,
    const std::vector<TeamUnitInfoDto> &ownedUnits) {
  std::vector<uint32_t> unitIds;
  unitIds.reserve(tc.units.size());
  for (const auto &[unitId, tpl] : tc.units) {
    if (tpl.campId != camp::kRecruitable) continue;
    unitIds.push_back(unitId);
  }
  std::sort(unitIds.begin(), unitIds.end());

  tlFbb.Clear();
  std::vector<flatbuffers::Offset<Game::RecruitUnitInfo>> offsets;
  offsets.reserve(unitIds.size());

  for (const uint32_t unitId : unitIds) {
    const auto &tpl = tc.units.at(unitId);
    offsets.push_back(Game::CreateRecruitUnitInfoDirect(
        tlFbb, tpl.unitId, tpl.unitName.c_str(),
        static_cast<int32_t>(tpl.baseHp), static_cast<int32_t>(tpl.baseAtk),
        static_cast<int32_t>(recruitCostOf(tpl)),
        ownsUnit(ownedUnits, unitId)));
  }

  auto vec = tlFbb.CreateVector(offsets);
  auto body = Game::CreateS2C_RecruitUnitList(tlFbb, vec);
  return wrapEnvelope(Game::MsgType_S2C_RecruitUnitList,
                      Game::MsgBody_S2C_RecruitUnitList, body.Union(), seq,
                      nowServerTimeMs());
}

/**
 * @brief ボスリストをエンコードする
 *
 * @param seq プッシュのシーケンス番号。
 * @param tc テンプレートキャッシュ。呼び出し中だけ借用する。
 *
 * @return エンコード済みバイト列を持つ Envelope。
 *
 * @note
 * - ボスとは camp_id == kBoss の unit_templates の行のことで、専用のボス設定テーブルはない。
 * - spawn_x/spawn_y は挑戦時に決まる出現地点に依存するため、リストの段階では一律 0 を送る。
 */
inline std::string encodeBossList(uint32_t seq, const cache::TemplateCache &tc) {
  std::vector<uint32_t> bossIds;
  for (const auto &[unitId, tpl] : tc.units) {
    if (tpl.campId != camp::kBoss) continue;
    bossIds.push_back(unitId);
  }
  std::sort(bossIds.begin(), bossIds.end());

  tlFbb.Clear();
  std::vector<flatbuffers::Offset<Game::BossInfo>> offsets;
  offsets.reserve(bossIds.size());

  for (const uint32_t bossId : bossIds) {
    const auto &tpl = tc.units.at(bossId);
    offsets.push_back(Game::CreateBossInfoDirect(
        tlFbb, tpl.unitId, tpl.unitName.c_str(), tpl.unitId,
        static_cast<int32_t>(tpl.baseHp), static_cast<int32_t>(tpl.baseAtk),
        0.f, 0.f, 0, nullptr));
  }

  auto vec = tlFbb.CreateVector(offsets);
  auto body = Game::CreateS2C_BossList(tlFbb, vec);
  return wrapEnvelope(Game::MsgType_S2C_BossList, Game::MsgBody_S2C_BossList,
                      body.Union(), seq, nowServerTimeMs());
}

/**
 * @brief プレイヤー情報パネルをエンコードする
 *
 * @param seq プッシュのシーケンス番号。
 * @param role 借用するキャラクター DTO。必要なフィールドをコピーする。
 *
 * @return エンコード済みバイト列を持つ Envelope。
 *
 * @note
 * - パネルはマップ入場時点のスナップショット。その後のゴールドの変化は S2C_GoldChanged で個別に送る。
 */
inline std::string encodePlayerProfile(uint32_t seq, const RoleInfoDto &role) {
  tlFbb.Clear();
  auto body = Game::CreateS2C_PlayerProfileDirect(
      tlFbb, role.roleId.c_str(), role.roleName.c_str(), role.gold,
      role.stamina, role.crystal, role.combatPower,
      static_cast<uint32_t>(role.currentMapId),
      static_cast<uint32_t>(role.selectedTeamId));
  return wrapEnvelope(Game::MsgType_S2C_PlayerProfile,
                      Game::MsgBody_S2C_PlayerProfile, body.Union(), seq,
                      nowServerTimeMs());
}

} // namespace codec
