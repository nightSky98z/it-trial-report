#pragma once

#include <cstdint>
#include <string_view>

namespace runtime_shutdown {

/**
 * @brief プロセスの正常終了処理を要求する
 *
 * @note
 * - コンソール入力スレッドや、シグナルを転送するスレッドから呼ぶ。
 * - プロセス全体の終了フラグを立てるだけで、各ワーカーが持つリソースを直接解放することはない。
 */
void requestShutdown();

/**
 * @brief 正常終了が要求済みかどうかを返す
 *
 * @retval true ワーカーは新しい処理の受け付けをやめ、自分の担当範囲で後始末をすること。
 * @retval false 通常どおり動作を続けてよい。
 */
bool isShutdownRequested();

/**
 * @brief コンソール入力が終了コマンドかどうかを判定する
 *
 * @param line stdin から読んだ 1 行。呼び出し中だけ借用し、前後の ASCII 空白は無視する。
 *
 * @retval true line が q・exit・quit のいずれか。
 * @retval false 終了コマンドではない。呼び出し側は入力待ちを続けること。
 */
bool isConsoleShutdownCommand(std::string_view line);

/**
 * @brief 書き戻しキューへまだタスクを投入する可能性のある producer の数
 *
 * @retval 0 game/redis などの producer は後始末を終え、今後タスクを投入しない。
 * @return 0 以外: consumer スレッドは終了前にキューを処理し続ける必要がある。
 */
uint32_t activeProducerCount();

/**
 * @brief 書き戻し producer の生存期間を表すガード
 *
 * @note
 * - 保持している間は、このスレッドが MySQL 書き戻しキューへタスクを投入する可能性があることを示す。
 * - デストラクタが走る前に、このスレッドが持つリソースの最終 flush を済ませておくこと。
 */
class ProducerGuard {
public:
  ProducerGuard();
  ProducerGuard(const ProducerGuard &) = delete;
  ProducerGuard &operator=(const ProducerGuard &) = delete;
  ~ProducerGuard();
};

} // namespace runtime_shutdown
