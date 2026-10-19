#pragma once
#include <cstdint>

namespace player_ship_constants {

/**
 * @brief 経験値の進捗がこのパーセント単位の区切りをまたいだときに書き戻す
 *
 * @note
 * - player_ship_db_writer はこのしきい値で経験値の書き込み頻度を減らす。
 */
inline constexpr uint32_t kExpWriteThresholdPct       = 50;

/**
 * @brief 巡回中のプレイヤー船がモンスターを再スキャンする間隔
 *
 * @note
 * - player_ship_ai_system はフレーム数ではなく、ミリ秒のタイムスタンプで比較する。
 */
inline constexpr uint64_t kRetargetIntervalMs         = 200;

/**
 * @brief 新しいプレイヤー船が最初に入るエリア
 *
 * @note
 * - エリア ID は 1 始まり。0 は無効なエリアを表す。
 */
inline constexpr uint32_t kDefaultAreaId              = 1;

/**
 * @brief プレイヤー船が瀕死になった後、死亡演出の状態にとどまる時間
 *
 * @note
 * - この時間が過ぎてから、AI は出現地点への帰還／治療の流れに切り替わる。
 */
inline constexpr uint64_t kDeathStayMs                = 1000;

/**
 * @brief MovingToAnchor にどれだけ滞留したら異常とみなすか
 *
 * @note
 * - 警告に使うだけで、状態を強制的に解除はしない。実際のマップでは経路が非常に長いこともあるので、
 *   強制解除すると「歩くのが遅い」だけのものを「止まっている」と誤判定して、通常の移動を中断してしまう。
 * - この状態の精霊はモンスターのターゲットにならず、止まると永久に無敵になってしまうので、必ずログで見えるようにする。
 */
inline constexpr uint64_t kMovingToAnchorStuckMs      = 30'000;

/**
 * @brief レベル 1 から 2 に上がるのに必要な基礎経験値
 *
 * @note
 * - city_combat_helpers::xpRequiredForLevel が、指数式の基数としてこの値を使う。
 */
inline constexpr uint64_t kBaseXpPerLevel             = 100;

/**
 * @brief レベルごとの必要経験値の増加倍率
 *
 * @note
 * - この値を変えると、以降のすべてのレベルの成長曲線が変わる。
 */
inline constexpr float    kXpGrowthRate               = 1.25f;

/**
 * @brief 精霊がモンスターより 1 レベル高いごとに、撃破経験値をどれだけ残すか（パーセント）
 *
 * @note
 * - 減衰は**レベルごとに掛け算**で効く。N レベル高ければ (kXpKeepPctPerLevel/100)^N が残る。
 *   1 レベル差で 85%、5 レベル差で約 44%、10 レベル差で約 20%。
 * - 減衰するのは精霊のレベルがモンスターより**高い**ときだけ。モンスターのレベルが精霊以上なら満額を与え、
 *   減衰もボーナスもない（格上のモンスターを倒して損をするべきではない）。
 * - 計算はすべて整数で行い、浮動小数点は使わない。プラットフォームによる丸めの違いが数値に影響しないようにするため。
 */
inline constexpr uint64_t kXpKeepPctPerLevel          = 85;

/**
 * @brief 1 回の撃破で得られる経験値の下限
 *
 * @note
 * - 減衰後もこの値を下回らない。**経験値が 0 や負になることは絶対にない**。
 *   計算は終始符号なし型で行い、レベル差には飽和減算を使い、最後にこの下限に丸める。
 * - 下限を 0 ではなく 1 にしているのは、格下狩りでも目に見える進捗を残すため。
 *   そうしないとプレイヤーには「いくら倒しても経験値バーが動かない」ように見えてしまう。
 */
inline constexpr uint64_t kMinKillXp                  = 1;

/**
 * @brief プレイヤー船のレベル上限の**フォールバック用デフォルト値**
 *
 * @note
 * - 正となる値は env.zone の `MAX_UNIT_LEVEL` で、いつでも変更してホットリロードで反映できる。
 *   この定数は設定を取得できない場面（ユニットテストや起動初期）でだけ使う。
 * - レベルアップの判定に直接使わないこと。レベルアップとマップ入場時の丸めは、必ず実行時に渡される上限を受け取ること。
 *   そうしないと、ホットリロードで上限を変えた後も一部のコードが古い上限のまま動いてしまう。
 */
inline constexpr uint32_t kDefaultMaxLevel            = 60;

/**
 * @brief 攻撃のためのナビゲーションで、到着とみなす追加の距離
 *
 * @note
 * - nav の目標がターゲットの当たり判定付近に落ちたときに、行ったり来たりするのを防ぐための値。
 */
inline constexpr float    kAttackArriveEpsilon        = 4.f;

/**
 * @brief レベルアップごとに増える最大 HP
 *
 * @note
 * - レベルアップ後、curHp は新しい maxHp まで回復する。
 */
inline constexpr uint32_t kHpPerLevel                 = 20;

/**
 * @brief レベルアップごとに増える攻撃力
 *
 * @note
 * - 対象は UnitCombat.attack_power だけ。プロトコルで攻撃力を表示する際は戦闘コンポーネントから同期する必要がある。
 */
inline constexpr uint32_t kAtkPerLevel                = 2;

/**
 * @brief 治療施設の固定 X 座標
 *
 * @note
 * - メインシティの entitiesLayer のローカル座標を使う。現在はシーンの中心に合わせている。
 */
inline constexpr float    kHealingStationX            = 0.f;

/**
 * @brief 治療施設の固定 Y 座標
 *
 * @note
 * - メインシティの entitiesLayer のローカル座標を使う。現在はシーンの中心に合わせている。
 */
inline constexpr float    kHealingStationY            = 0.f;

} // namespace player_ship_constants
