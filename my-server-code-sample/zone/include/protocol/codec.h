#pragma once
#include "generated/game_generated.h"
#include "utils/time_utils.h"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

/**
 * @brief 現在のスレッドで使い回す FlatBuffers の builder
 *
 * @note
 * - エンコード関数はこの builder を Clear() して再利用する。呼び出し側は builder 内部へのポインタを保持しないこと。
 * - 戻り値の std::string はエンコード済みのバイト列を所有しているので、次の Clear() の後も使える。
 */
inline thread_local flatbuffers::FlatBufferBuilder tlFbb(4096);

namespace codec {

/**
 * @brief プロトコルの Envelope に載せる、サーバーの WallClock（ミリ秒）を返す
 *
 * @return 現実世界の現在時刻（ミリ秒）。ログ・パケットキャプチャ・RTT の計測に使い、ゲームプレイの tick 同期には使わない。
 */
inline uint64_t nowServerTimeMs() {
  return timeutil::wall_now_ms();
}

/**
 * @brief Envelope を検証してビューを取得する
 *
 * @param raw クライアントから届いた FlatBuffers のバイト列全体。呼び出し中だけ借用する。
 *
 * @retval 非nullptr raw 内部のバイト列を指す、ゼロコピーの Envelope ビュー。
 * @retval nullptr FlatBuffers の検証に失敗した。
 *
 * @note
 * - 返したポインタを raw の寿命を超えて保持しないこと。raw を変更した後にも使わないこと。
 */
inline const Game::Envelope *decode(const std::string &raw) {
  flatbuffers::Verifier v(reinterpret_cast<const uint8_t *>(raw.data()),
                          raw.size());
  if (!Game::VerifyEnvelopeBuffer(v))
    return nullptr;
  return Game::GetEnvelope(raw.data());
}

/**
 * @brief プロトコルの Envelope で包む
 *
 * @param msgType 外側のメッセージ種別。
 * @param bodyType FlatBuffers の union の body の型。
 * @param body 現在の tlFbb で作成した union の offset。
 * @param seq クライアントのリクエストのシーケンス番号、またはサーバーのプッシュのシーケンス番号。
 * @param sendTimeMs Envelope の送信時刻。0 は呼び出し側が明示的に時刻なしを指定したことを表す。
 *
 * @return エンコード済みのバイト列を所有する std::string。
 *
 * @note
 * - body は、呼び出し前に現在のスレッドの tlFbb で作成したものであること。
 */
inline std::string wrapEnvelope(Game::MsgType msgType,
                                Game::MsgBody bodyType,
                                flatbuffers::Offset<void> body,
                                uint32_t seq,
                                uint64_t sendTimeMs = 0) {
  auto env = Game::CreateEnvelope(tlFbb, msgType, seq, sendTimeMs, bodyType, body);
  tlFbb.Finish(env);
  return {reinterpret_cast<const char *>(tlFbb.GetBufferPointer()),
          tlFbb.GetSize()};
}

/**
 * @brief エラーメッセージをエンコードする
 *
 * @param seq 対応するリクエストのシーケンス番号。
 * @param errorCode プロトコルのエラー種別。
 * @param message エラーメッセージ。FlatBuffers の文字列にコピーする。
 *
 * @return エンコード済みバイト列を持つ Envelope。
 */
inline std::string encodeErrorMessage(uint32_t seq, Game::ErrorType errorCode, const std::string &message) {
  tlFbb.Clear();
  auto body = Game::CreateS2C_ErrorMessageDirect(tlFbb,
    errorCode,
    message.c_str());
  return wrapEnvelope(Game::MsgType_S2C_ErrorMessage,
    Game::MsgBody_S2C_ErrorMessage,
    body.Union(), seq, nowServerTimeMs());
}

/**
 * @brief 編成内のユニットの DTO
 *
 * @note
 * - 普通の値のフィールドを持つ型。runtime* のフィールドは永続化された状態から取り、永続化されていなければデフォルト値を使う。
 */
struct TeamUnitInfoDto {
  uint32_t unitId = 0;
    // 表示用のフィールド：unit_templates の値で、ログイン時に編成と一緒に検索する。
    // プロトコルの TeamUnitInfo に必要で、クライアントの編成パネルはこれをそのまま表示し、別途問い合わせることはない。
  std::string unitName;
  uint32_t baseHp = 0;
  uint32_t baseAtk = 0;
  uint8_t slotIndex = 0;
  uint32_t runtimeLevel = 1;
  uint64_t runtimeExp = 0;
  uint32_t runtimeAreaId = 1;
  bool hasRuntimePersistence = false;
};

/**
 * @brief 編成の DTO
 *
 * @note
 * - units は DTO が所有する。エンコード関数は借用して FlatBuffers にコピーするだけ。
 */
struct TeamInfoDto {
  uint8_t teamId = 0;
  std::string teamName;
  std::vector<TeamUnitInfoDto> units;
};

/**
 * @brief キャラクター情報の DTO
 *
 * @note
 * - 文字列と編成の配列は DTO が所有する。エンコード関数は参照を保持しない。
 * - currentMapId/selectedTeamId はプロトコルのフィールド幅を使う。切り詰めや検証は呼び出し側が事前に行うこと。
 */
struct RoleInfoDto {
  std::string roleId;
  std::string roleName;

  uint32_t gold = 0;
  uint32_t stamina = 0;
  uint32_t crystal = 0;
  uint32_t combatPower = 0;

  uint8_t currentMapId = 0;
  uint8_t selectedTeamId = 0;
  uint32_t selectedUnitId = 0;

  std::vector<TeamInfoDto> teams;
};

/**
 * @brief TeamInfo の FlatBuffers ノードを構築する
 *
 * @param fbb 現在のエンコードで使う builder。
 * @param team 借用する編成の DTO。
 *
 * @return fbb 内部のオブジェクトを指す offset。
 *
 * @note
 * - 返した offset は同じ fbb でしか使えない。builder をまたいで保持しないこと。
 */
inline flatbuffers::Offset<Game::TeamInfo>
buildTeamInfoFromDto(flatbuffers::FlatBufferBuilder &fbb, const TeamInfoDto &team) {
  std::vector<flatbuffers::Offset<Game::TeamUnitInfo>> unitOffsets;
  unitOffsets.reserve(team.units.size());

  for (const auto& u : team.units) {
        // レベルは実行時に永続化された値を使い、名前と基礎ステータスはテンプレートのスナップショットから取る。
    unitOffsets.push_back(Game::CreateTeamUnitInfoDirect(
        fbb, u.unitId, u.slotIndex,
        u.unitName.empty() ? nullptr : u.unitName.c_str(), u.runtimeLevel,
        u.baseHp, u.baseAtk));
  }

  flatbuffers::Offset<flatbuffers::String> team_name = 0;
  if (!team.teamName.empty()) {
    team_name = fbb.CreateString(team.teamName);
  }

  flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<Game::TeamUnitInfo>>> units = 0;
  if (!unitOffsets.empty()) {
    units = fbb.CreateVector(unitOffsets);
  }

  return Game::CreateTeamInfo(fbb, team.teamId, team_name, units);
}

/**
 * @brief RoleInfo の FlatBuffers ノードを構築する
 *
 * @param fbb 現在のエンコードで使う builder。
 * @param role 借用するキャラクターの DTO。
 *
 * @return fbb 内部のオブジェクトを指す offset。
 */
inline flatbuffers::Offset<Game::RoleInfo>
buildRoleInfoFromDto(flatbuffers::FlatBufferBuilder &fbb, const RoleInfoDto& role) {
  std::vector<flatbuffers::Offset<Game::TeamInfo>> teamOffsets;
  teamOffsets.reserve(role.teams.size());

  for (const auto& team : role.teams) {
    teamOffsets.push_back(buildTeamInfoFromDto(fbb, team));
  }

  flatbuffers::Offset<flatbuffers::String> roleId = 0;
  flatbuffers::Offset<flatbuffers::String> roleName = 0;
  flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<Game::TeamInfo>>> teams = 0;

  if (!role.roleId.empty()) {
    roleId = fbb.CreateString(role.roleId);
  }

  if (!role.roleName.empty()) {
    roleName = fbb.CreateString(role.roleName);
  }

  if (!teamOffsets.empty()) {
    teams = fbb.CreateVector(teamOffsets);
  }

  return Game::CreateRoleInfo(
    fbb,
    roleId,
    roleName,
    role.gold,
    role.stamina,
    role.crystal,
    role.combatPower,
    role.currentMapId,
    role.selectedTeamId,
    role.selectedUnitId,
    teams
  );
}

/**
 * @brief EnterZone の成功／失敗の応答をエンコードする
 *
 * @param seq リクエストのシーケンス番号。
 * @param roleDto 借用するキャラクターの DTO。FlatBuffers にコピーする。
 * @param passToken ゾーン入場後に発行した passToken。NUL 終端の文字列であること。
 * @param ok true ならゾーン入場に成功、false なら業務上の理由で拒否。
 *
 * @return エンコード済みバイト列を持つ Envelope。
 */
inline std::string encodeEnterZoneAccepted(const uint32_t seq, const RoleInfoDto& roleDto, const char* passToken, const bool ok) {
  tlFbb.Clear();
  auto roleOff = buildRoleInfoFromDto(tlFbb, roleDto);
  auto passTokenOff = tlFbb.CreateString(passToken);
  auto body = Game::CreateS2C_EnterZoneAccepted(tlFbb,
    roleOff,
    passTokenOff,
    ok);
  return wrapEnvelope(Game::MsgType_S2C_EnterZoneAccepted,
    Game::MsgBody_S2C_EnterZoneAccepted,
    body.Union(), seq, nowServerTimeMs());
}

/**
 * @brief EnterMap の受理応答をエンコードする
 *
 * @param seq リクエストのシーケンス番号。
 * @param ok true ならリクエストを受理、false なら業務上の理由で拒否。
 *
 * @return エンコード済みバイト列を持つ Envelope。
 */
inline std::string encodeEnterMapAccepted(const uint32_t seq, const bool ok) {
  tlFbb.Clear();
  auto body = Game::CreateS2C_EnterMapAccepted(tlFbb,
    ok);
  return wrapEnvelope(Game::MsgType_S2C_EnterMapAccepted,
    Game::MsgBody_S2C_EnterMapAccepted,
    body.Union(), seq, nowServerTimeMs());
}

/**
 * @brief MapInstance 内のユニットの完全なスナップショットの DTO
 *
 * @note
 * - 文字列は DTO が所有する。エンコード関数は借用してコピーするだけ。
 * - networkEntityId は現在の MapInstance 内でのプロトコル／AOI 用の主キー。
 */
struct UnitSnapshotData {
  uint32_t networkEntityId = 0; // 現在の MapInstance 内でのプロトコル／AOI 用の主キー
  std::string ownerRoleId;      // 所有するキャラクター。所有者がいなければ ""
  uint32_t campId = 0;          // 陣営
  uint32_t unitId = 0;          // 精霊のテンプレート ID
  Game::UnitKind unitKind = Game::UnitKind_Unknown; // 種別
  std::string unitName;         // ユニット名（プレイヤーの精霊には "P." の接頭辞が付く）
  float x = 0.f;
  float y = 0.f;
  float vx = 0.f;
  float vy = 0.f;
  uint8_t facing = 0;
  uint32_t curHp = 0;
  uint32_t maxHp = 0;
    // 次の 2 項目は UnitSnapshot の通信上の構造には含めない。同じ走査の中で静的データ（UnitStaticData）も
    // まとめて埋めるためのもので、entered のエンティティのために world をもう一度検索せずに済むようにしている。
  uint32_t level = 1;           // 実行時のレベル。UnitBase.level から取る
  uint32_t attackPower = 0;     // 攻撃力。UnitCombat.attack_power から取る
  Game::LifeStatus lifeStatus = Game::LifeStatus_Alive;
  Game::ActionState actionState = Game::ActionState_Idle;
};

/**
 * @brief ユニットの静的スナップショットの DTO
 *
 * @note
 * - game.fbs の UnitStaticData に対応する。マップ入場時の全件送信と、AOI の entered に使う。
 * - 文字列は DTO が所有する。エンコード関数は借用してコピーするだけ。
 */
struct UnitStaticData {
  uint32_t networkEntityId = 0;
  std::string ownerRoleId;
  uint32_t campId = 0;
  uint32_t unitId = 0;
  Game::UnitKind unitKind = Game::UnitKind_Unknown;
  std::string unitName;
  uint32_t level = 1;                // 実行時のレベル。UnitBase.level から取る
    // 現在のレベルと装備ボーナスを反映した実際の値で、テンプレートの基礎値ではない。UnitCombat から取る。
    // サーバー権威：クライアントはそのまま表示し、レベルや装備から独自に計算し直さない。
  uint32_t maxHp = 0;                // 最大 HP。UnitCombat.max_hp から取る
  uint32_t attackPower = 0;          // 攻撃力。UnitCombat.attack_power から取る
};

/**
 * @brief 建物の静的スナップショットの DTO
 *
 * @note
 * - game.fbs の BuildingSnapshot に対応する。マップ入場時の静的データの全件送信に使う。
 */
struct BuildingStaticData {
  uint32_t networkEntityId = 0;
  uint32_t buildingId = 0;           // 建物のテンプレート ID
  Game::BuildingKind buildingKind = Game::BuildingKind_Unknown;
  std::string ownerRoleId;
  float x = 0.f;
  float y = 0.f;
  uint32_t level = 1;
  uint8_t state = 0;                 // 0=ロック 1=解放済み
  std::string buildingName;          // 表示名。buildings.building_name から取る
  uint32_t unlockCost = 0;           // 解放価格。ロック中の建物はこれで価格を表示する
};

/**
 * @brief ユニットの動的スナップショットの DTO
 *
 * @note
 * - game.fbs の UnitDynamicData に対応する。リアルタイムの AOI 差分更新に使う。
 * - この型は AoS の一時的な値。高頻度の一括エンコードでは UnitDynamicDataSoA への書き込みを優先する。
 */
struct UnitDynamicData {
  uint32_t networkEntityId = 0;
  float x = 0.f, y = 0.f;
  float vx = 0.f, vy = 0.f;
  uint8_t facing = 0;
  uint32_t curHp = 0, maxHp = 0;
  Game::LifeStatus lifeStatus = Game::LifeStatus_Alive;
  Game::ActionState actionState = Game::ActionState_Idle;
};

/**
 * @brief AOI の動的スナップショットをパックするためのバッファ
 *
 * @note
 * - すべての vector のメモリはこの構造体が所有する。呼び出し側は clear()/push() をまたいで内部要素への参照を保持しないこと。
 * - すべてのフィールドの配列は同じ添字空間を共有する。フィールドごとの長さがずれないよう、書き込みは push() 経由でのみ行う。
 * - 通信上のレイアウトは引き続き FlatBuffers の UnitDynamicData で決まる。この構造体が変えるのはサーバー内部の一時的なレイアウトだけ。
 */
struct UnitDynamicDataSoA {
    /**
     * @brief 現在の動的スナップショットの件数を返す
     *
     * @return すべての SoA 列で共通の要素数。
     */
  std::size_t size() const { return networkEntityIds.size(); }

    /**
     * @brief 動的スナップショットのバッファが空かどうかを返す
     *
     * @retval true エンコード待ちの動的スナップショットがない。
     * @retval false エンコード待ちの動的スナップショットが 1 つ以上ある。
     */
  bool empty() const { return networkEntityIds.empty(); }

    /**
     * @brief 各列の容量を予約する
     *
     * @param count 書き込む予定の動的スナップショットの件数。容量にだけ影響し、size() は変わらない。
     *
     * @note
     * - この tick の AOI の変化を集める前に呼べば、列ごとにばらばらに拡張されるのを避けられる。
     */
  void reserve(std::size_t count) {
    networkEntityIds.reserve(count);
    xs.reserve(count);
    ys.reserve(count);
    vxs.reserve(count);
    vys.reserve(count);
    facings.reserve(count);
    curHps.reserve(count);
    maxHps.reserve(count);
    lifeStatuses.reserve(count);
    actionStates.reserve(count);
  }

    /**
     * @brief すべての動的スナップショットの列をクリアする
     *
     * @note
     * - clear() の後は、それまで内部の列の添字で得ていた位置の意味はすべて無効になる。
     * - 確保済みの容量は残し、次のフレームで再利用する。
     */
  void clear() {
    networkEntityIds.clear();
    xs.clear();
    ys.clear();
    vxs.clear();
    vys.clear();
    facings.clear();
    curHps.clear();
    maxHps.clear();
    lifeStatuses.clear();
    actionStates.clear();
  }

    /**
     * @brief 動的スナップショットを 1 件追加する
     *
     * @param value 借用する AoS の一時的な値。普通の値のフィールドをコピーするだけで、参照は保持しない。
     *
     * @note
     * - value.networkEntityId が現在の AOI の視界集合に含まれていることは、呼び出し側が保証すること。
     */
  void push(const UnitDynamicData &value) {
    reserve(size() + 1);

    networkEntityIds.push_back(value.networkEntityId);
    xs.push_back(value.x);
    ys.push_back(value.y);
    vxs.push_back(value.vx);
    vys.push_back(value.vy);
    facings.push_back(value.facing);
    curHps.push_back(value.curHp);
    maxHps.push_back(value.maxHp);
    lifeStatuses.push_back(value.lifeStatus);
    actionStates.push_back(value.actionState);
  }

private:
  friend flatbuffers::Offset<Game::UnitDynamicData>
  buildUnitDynamicData(flatbuffers::FlatBufferBuilder &fbb,
                       const UnitDynamicDataSoA &data,
                       std::size_t index);

  std::vector<uint32_t> networkEntityIds;
  std::vector<float> xs;
  std::vector<float> ys;
  std::vector<float> vxs;
  std::vector<float> vys;
  std::vector<uint8_t> facings;
  std::vector<uint32_t> curHps;
  std::vector<uint32_t> maxHps;
  std::vector<Game::LifeStatus> lifeStatuses;
  std::vector<Game::ActionState> actionStates;
};

/**
 * @brief UnitStaticData の FlatBuffers ノードを 1 つ構築する
 *
 * @param fbb 現在のエンコードで使う builder。
 * @param u 借用するユニットの静的 DTO。
 *
 * @return fbb 内部のオブジェクトを指す offset。
 */
inline flatbuffers::Offset<Game::UnitStaticData>
buildUnitStaticData(flatbuffers::FlatBufferBuilder &fbb, const UnitStaticData &u) {
  auto ownerOff = fbb.CreateString(u.ownerRoleId);
  auto nameOff = fbb.CreateString(u.unitName);
  return Game::CreateUnitStaticData(
      fbb, u.networkEntityId, ownerOff, u.campId, u.unitId, u.unitKind, nameOff,
      u.level, u.maxHp, u.attackPower);
}

/**
 * @brief BuildingSnapshot の FlatBuffers ノードを 1 つ構築する
 *
 * @param fbb 現在のエンコードで使う builder。
 * @param b 借用する建物の静的 DTO。
 *
 * @return fbb 内部のオブジェクトを指す offset。
 */
inline flatbuffers::Offset<Game::BuildingSnapshot>
buildBuildingSnapshot(flatbuffers::FlatBufferBuilder &fbb, const BuildingStaticData &b) {
  auto ownerOff = fbb.CreateString(b.ownerRoleId);
  auto nameOff = fbb.CreateString(b.buildingName);
  Game::Vec2 pos{b.x, b.y};
  return Game::CreateBuildingSnapshot(
      fbb, b.networkEntityId, b.buildingId, b.buildingKind,
      ownerOff, &pos, b.level, b.state, nameOff, b.unlockCost);
}

/**
 * @brief UnitDynamicData の FlatBuffers ノードを 1 つ構築する
 *
 * @param fbb 現在のエンコードで使う builder。
 * @param d 借用するユニットの動的 DTO。
 *
 * @return fbb 内部のオブジェクトを指す offset。
 */
inline flatbuffers::Offset<Game::UnitDynamicData>
buildUnitDynamicData(flatbuffers::FlatBufferBuilder &fbb, const UnitDynamicData &d) {
  return Game::CreateUnitDynamicData(
      fbb, d.networkEntityId, d.x, d.y, d.vx, d.vy, d.facing,
      d.curHp, d.maxHp, d.lifeStatus, d.actionState);
}

/**
 * @brief SoA の動的スナップショットから UnitDynamicData の FlatBuffers ノードを 1 つ構築する
 *
 * @param fbb 現在のエンコードで使う builder。
 * @param data 借用する SoA の動的スナップショットの列。
 * @param index エンコードする列の添字。data.size() より小さいこと。
 *
 * @return fbb 内部のオブジェクトを指す offset。
 *
 * @note
 * - debug ビルドでは index の範囲を assert で確認する。release ビルドでは呼び出し側が保証すること。
 */
inline flatbuffers::Offset<Game::UnitDynamicData>
buildUnitDynamicData(flatbuffers::FlatBufferBuilder &fbb,
                     const UnitDynamicDataSoA &data,
                     std::size_t index) {
  assert(index < data.size());
  return Game::CreateUnitDynamicData(
      fbb,
      data.networkEntityIds[index],
      data.xs[index],
      data.ys[index],
      data.vxs[index],
      data.vys[index],
      data.facings[index],
      data.curHps[index],
      data.maxHps[index],
      data.lifeStatuses[index],
      data.actionStates[index]);
}

/**
 * @brief マップ入場時の初期スナップショットをエンコードする
 *
 * @param seq リクエストのシーケンス番号。
 * @param snapshotTick サーバーのスナップショットの tick。
 * @param snapshotTimeMs サーバーのスナップショットの時刻（ミリ秒）。
 * @param roleDto 借用するキャラクター情報の DTO。
 * @param units 借用する全ユニットの静的スナップショット。
 * @param buildings 借用する全建物の静的スナップショット。
 *
 * @return エンコード済みバイト列を持つ Envelope。
 */
inline std::string encodeJoinMapSnapshot(uint32_t seq,
                                         uint64_t snapshotTick,
                                         uint64_t snapshotTimeMs,
                                         const RoleInfoDto &roleDto,
                                         const std::vector<UnitStaticData> &units,
                                         const std::vector<BuildingStaticData> &buildings) {
  tlFbb.Clear();
  auto roleOff = buildRoleInfoFromDto(tlFbb, roleDto);

  std::vector<flatbuffers::Offset<Game::UnitStaticData>> unitOffsets;
  unitOffsets.reserve(units.size());
  for (const auto &u : units) {
    unitOffsets.push_back(buildUnitStaticData(tlFbb, u));
  }
  auto unitsVec = tlFbb.CreateVector(unitOffsets);

  std::vector<flatbuffers::Offset<Game::BuildingSnapshot>> buildingOffsets;
  buildingOffsets.reserve(buildings.size());
  for (const auto &b : buildings) {
    buildingOffsets.push_back(buildBuildingSnapshot(tlFbb, b));
  }
  auto buildingsVec = tlFbb.CreateVector(buildingOffsets);

  auto aoiOff = Game::CreateAoiGlobal(tlFbb, unitsVec, buildingsVec);

  auto body = Game::CreateS2C_JoinMapSnapshot(
      tlFbb, snapshotTick, snapshotTimeMs, roleOff, aoiOff);
  return wrapEnvelope(Game::MsgType_S2C_JoinMapSnapshot,
    Game::MsgBody_S2C_JoinMapSnapshot,
    body.Union(), seq, nowServerTimeMs());
}

/**
 * @brief マップへの正式な入場の応答をエンコードする
 *
 * @param seq リクエストのシーケンス番号。
 * @param ok true ならクライアントはマップ内の状態に移ってよい。
 *
 * @return エンコード済みバイト列を持つ Envelope。
 */
inline std::string encodeEnterMapDone(uint32_t seq, bool ok) {
  tlFbb.Clear();
  auto body = Game::CreateS2C_EnterMapDone(tlFbb, ok);
  return wrapEnvelope(Game::MsgType_S2C_EnterMapDone,
    Game::MsgBody_S2C_EnterMapDone,
    body.Union(), seq, nowServerTimeMs());
}

/**
 * @brief 時刻同期用の Pong をエンコードする
 *
 * @param seq リクエストのシーケンス番号。
 * @param clientWallTimeMs クライアントが Ping を送ったときの WallClock。RTT の計算に使う。
 * @param serverSimTimeMs サーバーが Pong を送るときの SimClock。Wall から Sim への対応付けに使う。
 *
 * @return エンコード済みバイト列を持つ Envelope。Envelope の sendTimeMs にはサーバーの WallClock を使う。
 */
inline std::string encodePong(uint32_t seq, uint64_t clientWallTimeMs,
                              uint64_t serverSimTimeMs) {
  tlFbb.Clear();
  auto body = Game::CreateS2C_Pong(tlFbb, clientWallTimeMs, serverSimTimeMs);
  return wrapEnvelope(Game::MsgType_S2C_Pong,
    Game::MsgBody_S2C_Pong,
    body.Union(), seq, nowServerTimeMs());
}

/**
 * @brief AOI の差分スナップショットをエンコードする
 *
 * @param seq プッシュのシーケンス番号。
 * @param snapshotTick サーバーのスナップショットの tick。
 * @param snapshotTimeMs サーバーのスナップショットの時刻（ミリ秒）。
 * @param enteredIds 新しく視界に入ったユニットの ID 一覧。enteredUnits と添字が 1 対 1 で対応する。
 * @param enteredUnits 新しく視界に入ったユニットの静的データ。
 * @param updated リアルタイムの動的データの SoA 列。
 * @param leftNetworkEntityIds 視界から外れたユニットの ID 一覧。
 *
 * @return エンコード済みバイト列を持つ Envelope。
 *
 * @note
 * - enteredIds と enteredUnits は同じ意味の順序を保つこと。
 */
inline std::string encodeUpdateAoiSnapshot(uint32_t seq,
                                            uint64_t snapshotTick,
                                            uint64_t snapshotTimeMs,
                                            const std::vector<uint32_t> &enteredIds,
                                            const std::vector<UnitStaticData> &enteredUnits,
                                            const UnitDynamicDataSoA &updated,
                                            const std::vector<uint32_t> &leftNetworkEntityIds) {
  tlFbb.Clear();

  auto enteredIdsVec = tlFbb.CreateVector(enteredIds);

  std::vector<flatbuffers::Offset<Game::UnitStaticData>> enteredUnitOffsets;
  enteredUnitOffsets.reserve(enteredUnits.size());
  for (const auto &u : enteredUnits) {
    enteredUnitOffsets.push_back(buildUnitStaticData(tlFbb, u));
  }
  auto enteredUnitsVec = tlFbb.CreateVector(enteredUnitOffsets);

  std::vector<flatbuffers::Offset<Game::UnitDynamicData>> updatedOffsets;
  updatedOffsets.reserve(updated.size());
  for (std::size_t index = 0; index < updated.size(); ++index) {
    updatedOffsets.push_back(buildUnitDynamicData(tlFbb, updated, index));
  }
  auto updatedVec = tlFbb.CreateVector(updatedOffsets);

  auto leftVec = tlFbb.CreateVector(leftNetworkEntityIds);

  auto body = Game::CreateS2C_UpdateAoiSnapshot(tlFbb,
      snapshotTick, snapshotTimeMs,
      enteredIdsVec, updatedVec, leftVec,
      /*ack_input_seq=*/0,
      enteredUnitsVec);
  return wrapEnvelope(Game::MsgType_S2C_UpdateAoiSnapshot,
    Game::MsgBody_S2C_UpdateAoiSnapshot,
    body.Union(), seq, nowServerTimeMs());
}

/**
 * @brief 戦闘イベントの batch をパックするためのバッファ
 *
 * @note
 * - イベント種別の列とイベントの offset の列はこの構造体が所有する。呼び出し側は内部要素への参照を保持しないこと。
 * - offset は、現在の beginBattleEventBatch() が返した FlatBufferBuilder で作ったものであること。
 * - push() は NONE や空の offset を捨てるので、size() はエンコード可能なイベントだけを数える。
 */
struct BattleEventBatchSoA {
    /**
     * @brief エンコード可能なイベントの件数を返す
     *
     * @return eventTypes と eventOffsets で共通の要素数。
     */
  std::size_t size() const { return eventTypes.size(); }

    /**
     * @brief batch が空かどうかを返す
     *
     * @retval true エンコード可能なイベントがない。
     * @retval false エンコード可能なイベントが 1 つ以上ある。
     */
  bool empty() const { return eventTypes.empty(); }

    /**
     * @brief 各列の容量を予約する
     *
     * @param count 見込みのイベント件数。容量にだけ影響し、size() は変わらない。
     */
  void reserve(std::size_t count) {
    eventTypes.reserve(count);
    eventOffsets.reserve(count);
  }

    /**
     * @brief イベントの列をクリアする
     *
     * @note
     * - clear() の後は、それまでのイベントの添字はすべて無効になる。
     */
  void clear() {
    eventTypes.clear();
    eventOffsets.clear();
  }

    /**
     * @brief 戦闘イベントを 1 件追加する
     *
     * @param type FlatBuffers の union の型。NONE はイベントなしを表し、拒否する。
     * @param offset 現在の FlatBufferBuilder で作成した union の offset。0 はイベントなしを表し、拒否する。
     *
     * @retval true イベントを 2 つの列に書き込んだ。
     * @retval false type または offset が無効で、書き込んでいない。
     */
  bool push(Game::BattleEvent type, flatbuffers::Offset<void> offset) {
    if (type == Game::BattleEvent_NONE || offset.o == 0) {
      return false;
    }

    reserve(size() + 1);

    eventTypes.push_back(static_cast<uint8_t>(type));
    eventOffsets.push_back(offset);
    return true;
  }

private:
  friend std::string encodeBattleEventBatch(uint32_t seq,
                                            uint64_t tick,
                                            const BattleEventBatchSoA &items);

  std::vector<uint8_t> eventTypes;
  std::vector<flatbuffers::Offset<void>> eventOffsets;
};

/**
 * @brief 戦闘イベントの batch の構築を始める
 *
 * @return 現在のスレッドで使い回す FlatBufferBuilder への参照（Clear() 済み）。
 *
 * @note
 * - この後の build* によるイベントの構築と BattleEventBatchSoA::push には、必ず同じ builder を使うこと。
 */
inline flatbuffers::FlatBufferBuilder &beginBattleEventBatch() {
  tlFbb.Clear();
  return tlFbb;
}

/**
 * @brief 状態切り替えの戦闘イベントを構築する
 *
 * @param fbb 現在の戦闘イベント batch の builder。
 * @param serverTimeMs イベントが発生した時刻（ミリ秒）。
 * @param entityId 状態が変化したエンティティのプロトコル ID。
 * @param prevState 切り替え前の状態。
 * @param newState 切り替え後の状態。
 *
 * @return fbb 内部のオブジェクトを指す offset。
 */
inline flatbuffers::Offset<Game::S2C_StateChanged>
buildStateChanged(flatbuffers::FlatBufferBuilder &fbb,
                  uint64_t serverTimeMs,
                  uint32_t entityId,
                  Game::ActionState prevState,
                  Game::ActionState newState) {
  return Game::CreateS2C_StateChanged(fbb, serverTimeMs, entityId, prevState,
                                      newState);
}

/**
 * @brief アクション開始の戦闘イベントを構築する
 *
 * @param fbb 現在の戦闘イベント batch の builder。
 * @param serverTimeMs イベントが発生した時刻（ミリ秒）。
 * @param entityId アクションを実行するエンティティのプロトコル ID。
 * @param actionType プロトコルのアクション種別。
 * @param durationSec クライアントでの演出の継続時間。
 * @param targetX アクションの目標の X 座標。
 * @param targetY アクションの目標の Y 座標。
 * @param skillId スキル ID。255 はスキルなしを表す。
 *
 * @return fbb 内部のオブジェクトを指す offset。
 */
inline flatbuffers::Offset<Game::S2C_ActionStarted>
buildActionStarted(flatbuffers::FlatBufferBuilder &fbb,
                   uint64_t serverTimeMs,
                   uint32_t entityId,
                   uint8_t actionType,
                   float durationSec,
                   float targetX,
                   float targetY,
                   uint8_t skillId = 255) {
  return Game::CreateS2C_ActionStarted(fbb, serverTimeMs, entityId, actionType,
                                       durationSec, targetX, targetY, skillId);
}

/**
 * @brief アクション終了の戦闘イベントを構築する
 *
 * @param fbb 現在の戦闘イベント batch の builder。
 * @param serverTimeMs イベントが発生した時刻（ミリ秒）。
 * @param entityId アクションを終えたエンティティのプロトコル ID。
 * @param actionType プロトコルのアクション種別。
 *
 * @return fbb 内部のオブジェクトを指す offset。
 */
inline flatbuffers::Offset<Game::S2C_ActionFinished>
buildActionFinished(flatbuffers::FlatBufferBuilder &fbb,
                    uint64_t serverTimeMs,
                    uint32_t entityId,
                    uint8_t actionType) {
  return Game::CreateS2C_ActionFinished(fbb, serverTimeMs, entityId,
                                        actionType);
}

/**
 * @brief ダメージ結果の戦闘イベントを構築する
 *
 * @param fbb 現在の戦闘イベント batch の builder。
 * @param serverTimeMs イベントが発生した時刻（ミリ秒）。
 * @param sourceId 攻撃者のプロトコル上のエンティティ ID。
 * @param targetId 被弾者のプロトコル上のエンティティ ID。
 * @param damage 今回のダメージ値。
 * @param delta HP の変化量。減少は負の値。
 * @param finalHp 結果確定後の現在 HP。
 * @param maxHp 結果確定時の最大 HP。
 * @param isCrit クリティカルかどうか。
 * @param isMiss 攻撃が外れたかどうか。
 * @param skillId スキル ID。
 * @param hitX 命中点の X 座標。
 * @param hitY 命中点の Y 座標。
 *
 * @return fbb 内部のオブジェクトを指す offset。
 */
inline flatbuffers::Offset<Game::S2C_CombatResult>
buildCombatResult(flatbuffers::FlatBufferBuilder &fbb,
                  uint64_t serverTimeMs,
                  uint32_t sourceId,
                  uint32_t targetId,
                  int damage,
                  int delta,
                  uint32_t finalHp,
                  uint32_t maxHp,
                  bool isCrit,
                  bool isMiss,
                  uint8_t skillId,
                  float hitX,
                  float hitY) {
  return Game::CreateS2C_CombatResult(fbb, serverTimeMs, sourceId, targetId,
                                      damage, delta, finalHp, maxHp,
                                      isCrit, isMiss, skillId, hitX, hitY);
}

/**
 * @brief スキル発動の戦闘イベントを構築する
 *
 * @param fbb 現在の戦闘イベント batch の builder。
 * @param serverTimeMs イベントが発生した時刻（ミリ秒）。
 * @param casterId 発動者のプロトコル上のエンティティ ID。
 * @param skillId スキル ID。
 * @param originX 起点の X 座標。
 * @param originY 起点の Y 座標。
 * @param targetX 目標の X 座標。
 * @param targetY 目標の Y 座標。
 *
 * @return fbb 内部のオブジェクトを指す offset。
 */
inline flatbuffers::Offset<Game::S2C_SkillCast>
buildSkillCast(flatbuffers::FlatBufferBuilder &fbb,
               uint64_t serverTimeMs,
               uint32_t casterId,
               uint8_t skillId,
               float originX,
               float originY,
               float targetX,
               float targetY) {
  return Game::CreateS2C_SkillCast(fbb, serverTimeMs, casterId, skillId,
                                   originX, originY, targetX, targetY);
}

/**
 * @brief HP 変化の戦闘イベントを構築する
 *
 * @param fbb 現在の戦闘イベント batch の builder。
 * @param serverTimeMs イベントが発生した時刻（ミリ秒）。
 * @param entityId HP が変化したエンティティのプロトコル ID。
 * @param curHp 現在 HP。
 * @param maxHp 最大 HP。
 * @param delta HP の変化量。回復は正、減少は負の値。
 *
 * @return fbb 内部のオブジェクトを指す offset。
 */
inline flatbuffers::Offset<Game::S2C_HpChanged>
buildHpChanged(flatbuffers::FlatBufferBuilder &fbb,
               uint64_t serverTimeMs,
               uint32_t entityId,
               uint32_t curHp,
               uint32_t maxHp,
               int delta) {
  return Game::CreateS2C_HpChanged(fbb, serverTimeMs, entityId, curHp, maxHp,
                                   delta);
}

/**
 * @brief 死亡の戦闘イベントを構築する
 *
 * @param fbb 現在の戦闘イベント batch の builder。
 * @param serverTimeMs イベントが発生した時刻（ミリ秒）。
 * @param entityId 死亡したエンティティのプロトコル ID。
 * @param killerId 撃破者のプロトコル上のエンティティ ID。0 は撃破者なしを表す。
 *
 * @return fbb 内部のオブジェクトを指す offset。
 */
inline flatbuffers::Offset<Game::S2C_Death>
buildDeath(flatbuffers::FlatBufferBuilder &fbb,
           uint64_t serverTimeMs,
           uint32_t entityId,
           uint32_t killerId = 0) {
  return Game::CreateS2C_Death(fbb, serverTimeMs, entityId, killerId);
}

/**
 * @brief 復活の戦闘イベントを構築する
 *
 * @param fbb 現在の戦闘イベント batch の builder。
 * @param serverTimeMs イベントが発生した時刻（ミリ秒）。
 * @param entityId 復活したエンティティのプロトコル ID。
 *
 * @return fbb 内部のオブジェクトを指す offset。
 */
inline flatbuffers::Offset<Game::S2C_Revive>
buildRevive(flatbuffers::FlatBufferBuilder &fbb,
            uint64_t serverTimeMs,
            uint32_t entityId) {
  return Game::CreateS2C_Revive(fbb, serverTimeMs, entityId);
}

/**
 * @brief バフ変化の戦闘イベントを構築する
 *
 * @param fbb 現在の戦闘イベント batch の builder。
 * @param serverTimeMs イベントが発生した時刻（ミリ秒）。
 * @param entityId バフが付いているエンティティのプロトコル ID。
 * @param buffId バフの設定 ID。
 * @param op バフの操作種別。
 * @param stacks 現在のスタック数。
 * @param remainSec 残り秒数。
 *
 * @return fbb 内部のオブジェクトを指す offset。
 */
inline flatbuffers::Offset<Game::S2C_BuffEvent>
buildBuffEvent(flatbuffers::FlatBufferBuilder &fbb,
               uint64_t serverTimeMs,
               uint32_t entityId,
               uint32_t buffId,
               Game::BuffOp op,
               uint8_t stacks,
               float remainSec) {
  return Game::CreateS2C_BuffEvent(fbb, serverTimeMs, entityId, buffId, op,
                                   stacks, remainSec);
}

/**
 * @brief フェーズ変化の戦闘イベントを構築する
 *
 * @param fbb 現在の戦闘イベント batch の builder。
 * @param serverTimeMs イベントが発生した時刻（ミリ秒）。
 * @param entityId フェーズが変化したエンティティのプロトコル ID。
 * @param prevPhase 切り替え前のフェーズ。
 * @param newPhase 切り替え後のフェーズ。
 *
 * @return fbb 内部のオブジェクトを指す offset。
 */
inline flatbuffers::Offset<Game::S2C_PhaseChanged>
buildPhaseChanged(flatbuffers::FlatBufferBuilder &fbb,
                  uint64_t serverTimeMs,
                  uint32_t entityId,
                  uint8_t prevPhase,
                  uint8_t newPhase) {
  return Game::CreateS2C_PhaseChanged(fbb, serverTimeMs, entityId, prevPhase,
                                      newPhase);
}

/**
 * @brief レベルアップの戦闘イベントを構築する
 *
 * @param fbb 現在の戦闘イベント batch の builder。
 * @param serverTimeMs イベントが発生した時刻（ミリ秒）。
 * @param entityId レベルアップしたエンティティのプロトコル ID。
 * @param newLevel レベルアップ後のレベル。
 *
 * @return fbb 内部のオブジェクトを指す offset。
 */
inline flatbuffers::Offset<Game::S2C_LevelUp>
buildLevelUp(flatbuffers::FlatBufferBuilder &fbb,
             uint64_t serverTimeMs,
             uint32_t entityId,
             uint32_t newLevel) {
  return Game::CreateS2C_LevelUp(fbb, serverTimeMs, entityId, newLevel);
}

/**
 * @brief ユニットの派生ステータス変化イベントを構築する
 *
 * @param fbb 現在の戦闘イベント batch の builder。
 * @param serverTimeMs イベントが発生した時刻（ミリ秒）。
 * @param entityId プロトコル上のエンティティ ID。
 * @param level 現在のレベル。
 * @param maxHp レベル成長と装備ボーナスを含めた最大 HP。
 * @param attackPower レベル成長と装備ボーナスを含めた攻撃力。
 *
 * @return fbb 内部のオブジェクトを指す offset。
 *
 * @note
 * - 数値は呼び出し側が world から読み直したものを渡すこと。イベントのペイロードにある古い値を使わないこと。
 */
inline flatbuffers::Offset<Game::S2C_UnitStatsChanged>
buildUnitStatsChanged(flatbuffers::FlatBufferBuilder &fbb,
                      uint64_t serverTimeMs,
                      uint32_t entityId,
                      uint32_t level,
                      uint32_t maxHp,
                      uint32_t attackPower) {
  return Game::CreateS2C_UnitStatsChanged(fbb, serverTimeMs, entityId, level,
                                          maxHp, attackPower);
}

/**
 * @brief 建物 1 つの変化イベントをエンコードする
 *
 * @param seq 対応するリクエストのシーケンス番号。
 * @param snapshotTick 現在のマップインスタンスの AOI スナップショット番号。
 * @param snapshotTimeMs 現在のマップインスタンスの SimClock（ミリ秒）。
 * @param b 借用する建物の静的スナップショット。FlatBuffers にコピーする。
 *
 * @return エンコード済みバイト列を持つ Envelope。
 *
 * @note
 * - 建物は AOI の差分配信を通らないので、解放・レベルアップ・見た目の変化があれば必ずこのメッセージを明示的に送ること。
 */
inline std::string encodeBuildingChanged(uint32_t seq, uint64_t snapshotTick,
                                         uint64_t snapshotTimeMs,
                                         const BuildingStaticData &b) {
  tlFbb.Clear();
  auto buildingOff = buildBuildingSnapshot(tlFbb, b);
  auto body = Game::CreateS2C_BuildingChanged(tlFbb, snapshotTick,
                                              snapshotTimeMs, buildingOff);
  return wrapEnvelope(Game::MsgType_S2C_BuildingChanged,
                      Game::MsgBody_S2C_BuildingChanged, body.Union(), seq,
                      nowServerTimeMs());
}

/**
 * @brief 戦闘イベントの batch をエンコードする
 *
 * @param seq プッシュのシーケンス番号。
 * @param tick イベント batch に対応するサーバーの tick。
 * @param items 借用する SoA のイベント列。offset は現在の tlFbb で作ったものであること。
 *
 * @retval 空でない文字列 エンコード済みバイト列を持つ Envelope。
 * @retval 空文字列 items が空なので、パケットは送らない。
 *
 * @note
 * - 呼び出し前に beginBattleEventBatch() で builder をクリアし、同じ builder で offset を作っておくこと。
 */
inline std::string encodeBattleEventBatch(
    uint32_t seq,
    uint64_t tick,
    const BattleEventBatchSoA &items) {
  if (items.empty()) {
    return {};
  }

  auto typesVec = tlFbb.CreateVector(items.eventTypes);
  auto eventsVec = tlFbb.CreateVector(items.eventOffsets);
  auto body = Game::CreateS2C_BattleEventBatch(tlFbb, tick, typesVec,
                                               eventsVec);
  return wrapEnvelope(Game::MsgType_S2C_BattleEventBatch,
                      Game::MsgBody_S2C_BattleEventBatch, body.Union(), seq,
                      nowServerTimeMs());
}

} // namespace codec
