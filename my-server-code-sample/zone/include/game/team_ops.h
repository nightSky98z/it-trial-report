#pragma once
#include "db/role_economy_db_writer.h"
#include "game/game_session.h"
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

/**
 * @file team_ops.h
 * @brief 編成操作をサーバー側で確定させる（サーバー権威）
 *
 * 編成状態の実行時の正となる値は GameSession::roleDto.teams と selectedTeamId。
 * ここではメモリを書き換えて永続化タスクを投入するだけで、マップ上のエンティティの作り直しは
 * 呼び出し側がマップ入場と同じ処理を流用して行う。ここでは ECS に触れない。
 */
namespace team_ops {

/// 編成番号の有効範囲（両端を含む）。Java 版の MAX_TEAM_COUNT と同じ。
inline constexpr uint8_t kMinTeamId = 1;
inline constexpr uint8_t kMaxTeamId = 3;

/// 1 編成あたりのスロット数。スロットのインデックスは [0, kMaxSlotCount)。
inline constexpr uint8_t kMaxSlotCount = 5;

/**
 * @brief 指定した編成を探す（なければ作る）
 *
 * @param[in,out] role キャラクター DTO。
 * @param teamId 編成番号。
 *
 * @return その編成への可変参照。存在しなければ必要に応じて作成する。
 *
 * @note
 * - role.teams に追加・削除があると、返した参照は無効になる。
 */
inline codec::TeamInfoDto &teamRef(codec::RoleInfoDto &role, uint8_t teamId) {
  for (auto &t : role.teams) {
    if (t.teamId == teamId) return t;
  }
  role.teams.push_back(codec::TeamInfoDto{teamId, {}, {}});
  return role.teams.back();
}

/**
 * @brief 出撃：精霊を指定した編成のスロットに入れる
 *
 * @param[in,out] sess セッション。成功時は roleDto.teams を更新して永続化タスクを投入する。
 * @param teamId 対象の編成。
 * @param slotIndex 対象のスロット。
 * @param unitId 精霊のテンプレート ID。
 *
 * @retval false 引数が不正。この場合 sess は変更されない。
 *
 * @note
 * - 1 体の精霊が占められるスロットは 1 つだけ。既に別のスロットにいる場合は、先に元のスロットから外す
 *   （編成をまたぐ移動も含む）。
 * - 呼び出し前に、その精霊がこのキャラクターのものであることを確認しておくこと。
 */
inline bool deploy(GameSession &sess, uint8_t teamId, uint8_t slotIndex,
                   uint32_t unitId) {
  if (teamId < kMinTeamId || teamId > kMaxTeamId) return false;
  if (slotIndex >= kMaxSlotCount || unitId == 0) return false;

    // 1) まず、どの編成にあるかにかかわらずこの精霊の古いスロットを外す。同じ精霊が重複して出撃しないようにするため。
  for (auto &t : sess.roleDto.teams) {
    for (std::size_t i = 0; i < t.units.size(); ++i) {
      if (t.units[i].unitId != unitId) continue;
      const uint8_t oldTeam = t.teamId;
      const uint8_t oldSlot = t.units[i].slotIndex;
      t.units.erase(t.units.begin() + static_cast<std::ptrdiff_t>(i));
      role_economy_db_writer::enqueueTeamSlot(sess.roleId, oldTeam, oldSlot, 0);
      break;
    }
  }

    // 2) 対象のスロットを占有する。既に誰かがいれば入れ替える（入れ替えられた精霊はそのまま待機になる）。
  auto &team = teamRef(sess.roleDto, teamId);
  for (auto &u : team.units) {
    if (u.slotIndex != slotIndex) continue;
    u.unitId = unitId;
    role_economy_db_writer::enqueueTeamSlot(sess.roleId, teamId, slotIndex,
                                            unitId);
    return true;
  }

  codec::TeamUnitInfoDto entry{};
  entry.unitId = unitId;
  entry.slotIndex = slotIndex;
  team.units.push_back(entry);
  std::sort(team.units.begin(), team.units.end(),
            [](const auto &a, const auto &b) {
              return a.slotIndex < b.slotIndex;
            });
  role_economy_db_writer::enqueueTeamSlot(sess.roleId, teamId, slotIndex,
                                          unitId);
  return true;
}

/**
 * @brief 指定スロットにいる精霊を取得する
 *
 * @param sess セッション。編成状態を読むだけ。
 * @param teamId 編成番号。
 * @param slotIndex スロットのインデックス。
 *
 * @return そのスロットの精霊のテンプレート ID。
 * @retval 0 スロットが空。
 *
 * @note
 * - 待機に戻す前に、必ずこれで精霊 ID を取り出しておくこと。undeploy した後ではこの情報は失われるが、
 *   呼び出し側はこの精霊をメインシティへ戻すためにまだ ID を使う。
 */
inline uint32_t unitAtSlot(const GameSession &sess, uint8_t teamId,
                           uint8_t slotIndex) {
  for (const auto &t : sess.roleDto.teams) {
    if (t.teamId != teamId) continue;
    for (const auto &u : t.units) {
      if (u.slotIndex == slotIndex) return u.unitId;
    }
  }
  return 0;
}

/**
 * @brief 待機：指定した編成のスロットを空にする
 *
 * @param[in,out] sess セッション。成功時は roleDto.teams を更新して永続化タスクを投入する。
 * @param teamId 対象の編成。
 * @param slotIndex 対象のスロット。
 *
 * @retval false スロットはもともと空だった。この場合 sess は変更されない。
 */
inline bool undeploy(GameSession &sess, uint8_t teamId, uint8_t slotIndex) {
  if (teamId < kMinTeamId || teamId > kMaxTeamId) return false;
  if (slotIndex >= kMaxSlotCount) return false;

  for (auto &t : sess.roleDto.teams) {
    if (t.teamId != teamId) continue;
    for (std::size_t i = 0; i < t.units.size(); ++i) {
      if (t.units[i].slotIndex != slotIndex) continue;
      t.units.erase(t.units.begin() + static_cast<std::ptrdiff_t>(i));
      role_economy_db_writer::enqueueTeamSlot(sess.roleId, teamId, slotIndex, 0);
      return true;
    }
  }
  return false;
}

/**
 * @brief 出撃する編成を切り替える
 *
 * @param[in,out] sess セッション。成功時は roleDto.selectedTeamId を更新して永続化タスクを投入する。
 * @param teamId 対象の編成。
 *
 * @retval false 編成番号が不正、または現在出撃中の編成と同じ。この場合 sess は変更されない。
 *
 * @note
 * - 現在の編成と同じ場合は失敗として返す。そうしないと、呼び出し側がマップ上のエンティティを無駄に作り直してしまう。
 */
inline bool switchTeam(GameSession &sess, uint8_t teamId) {
  if (teamId < kMinTeamId || teamId > kMaxTeamId) return false;
  if (sess.roleDto.selectedTeamId == teamId) return false;

  sess.roleDto.selectedTeamId = teamId;
    // 出撃する編成が変わったので、操作中の精霊も新しい編成の先頭に切り替える。
  sess.roleDto.selectedUnitId = 0;
  for (const auto &t : sess.roleDto.teams) {
    if (t.teamId != teamId || t.units.empty()) continue;
    sess.roleDto.selectedUnitId = t.units.front().unitId;
    break;
  }

  role_economy_db_writer::enqueueSelectedTeam(sess.roleId, teamId);
  return true;
}

/**
 * @brief 現在出撃中の編成に含まれる精霊 ID の一覧を取り出す
 *
 * @param role 借用するキャラクター DTO。
 *
 * @return 現在の selectedTeamId の編成に含まれる全精霊のテンプレート ID。空の編成なら空のリスト。
 *
 * @note
 * - エリアの割り当てでは、必ずこれを使って「出撃中／待機中」を区別すること。マップ上にはキャラクターの
 *   全精霊がいるので、待機中の精霊まで一緒に出撃させてはならない。
 */
inline std::vector<uint32_t> deployedUnitIds(const codec::RoleInfoDto &role) {
  std::vector<uint32_t> out;
  for (const auto &t : role.teams) {
    if (t.teamId != role.selectedTeamId) continue;
    out.reserve(t.units.size());
    for (const auto &u : t.units) out.push_back(u.unitId);
    break;
  }
  return out;
}

/**
 * @brief 現在の編成を、クライアントがそのまま上書きできる JSON にシリアライズする
 *
 * @param role 借用するキャラクター DTO。
 *
 * @return {"teams":[{"teamId":1,"teamName":"","units":[{"unitId":1,"slotIndex":0}]}]} の形式。
 *
 * @note
 * - フィールド名はフロントエンドの TeamInfoData / TeamUnitInfoData と 1 対 1 で対応している。名前を変えるとクライアントが読めなくなる。
 * - 編成名は現在ゲームプレイに関係しないため、一律で空文字列を送る。
 */
inline std::string teamsToJson(const codec::RoleInfoDto &role) {
  std::string json = "{\"teams\":[";
  bool firstTeam = true;
  for (const auto &t : role.teams) {
    if (!firstTeam) json += ",";
    firstTeam = false;
    json += "{\"teamId\":" + std::to_string(static_cast<uint32_t>(t.teamId));
    json += ",\"teamName\":\"\",\"units\":[";
    bool firstUnit = true;
    for (const auto &u : t.units) {
      if (!firstUnit) json += ",";
      firstUnit = false;
      json += "{\"unitId\":" + std::to_string(u.unitId) +
              ",\"slotIndex\":" +
              std::to_string(static_cast<uint32_t>(u.slotIndex)) + "}";
    }
    json += "]}";
  }
  json += "]}";
  return json;
}

} // namespace team_ops
