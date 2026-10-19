#pragma once
#include <algorithm>
#include <cstdint>

namespace ai_utils {

/**
 * @brief AI の状態を切り替え、直前の状態を記録する
 *
 * @param ai AI 状態オブジェクト。state・prev_state・dirty の 3 フィールドを持つこと。
 * @param next 新しい状態。uint8_t に変換できる型であること。
 *
 * @note
 * - 同じ状態を再設定した場合は dirty をクリアする（＝今回は状態遷移なし）。
 */
template <typename AIState, typename AIStateType>
inline void set_state(AIState &ai, AIStateType next) {
  const uint8_t n = static_cast<uint8_t>(next);
  if (ai.state == n) {
    ai.dirty = false;
    return;
  }
  ai.prev_state = ai.state;
  ai.state      = n;
  ai.dirty      = true;
}

} // namespace ai_utils

namespace ai_utils {

/**
 * @brief 攻撃を開始する距離（コミット距離）を計算する
 *
 * @param attack_range 元の攻撃範囲。1 未満は 1 として扱う。
 * @param commit_buffer 攻撃範囲より手前で攻撃を始めるためのバッファ距離。妥当な範囲に丸める。
 *
 * @return 1 以上のコミット距離。
 */
inline float attack_commit_range(float attack_range, float commit_buffer) {
  const float safe_attack_range = std::max(1.f, attack_range);
  const float safe_buffer =
      std::clamp(commit_buffer, 0.f, std::max(0.f, safe_attack_range - 1.f));
  return std::max(1.f, safe_attack_range - safe_buffer);
}

} // namespace ai_utils
