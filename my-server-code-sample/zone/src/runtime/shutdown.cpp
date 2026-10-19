#include "runtime/shutdown.h"

#include <atomic>
#include <cctype>

namespace runtime_shutdown {
namespace {

std::atomic_bool gShutdownRequested{false};
std::atomic_uint32_t gActiveProducers{0};

/**
 * @brief コンソール入力の前後の ASCII 空白を取り除く
 *
 * @return 元の line を指す借用ビュー（メモリは所有しない）。
 */
std::string_view trimAscii(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
    s.remove_prefix(1);
  }
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
    s.remove_suffix(1);
  }
  return s;
}

} // namespace

/**
 * @brief プロセスの安全な終了処理を要求する
 *
 * @note
 * - コンソールスレッドやシグナル処理の経路から呼ばれる。アトミックなフラグを立てるだけ。
 */
void requestShutdown() {
  gShutdownRequested.store(true, std::memory_order_release);
}

/**
 * @brief 安全な終了が要求済みかどうかを返す
 *
 * @retval true 終了が要求されている。
 */
bool isShutdownRequested() {
  return gShutdownRequested.load(std::memory_order_acquire);
}

/**
 * @brief コンソール入力が終了コマンドかどうかを判定する
 *
 * @retval true 空白を除いた結果が q・exit・quit のいずれか。
 */
bool isConsoleShutdownCommand(std::string_view line) {
  line = trimAscii(line);
  return line == "q" || line == "exit" || line == "quit";
}

/**
 * @brief まだタスクを投入する可能性のある producer の数を返す
 *
 * @return 現在の ProducerGuard の数。
 */
uint32_t activeProducerCount() {
  return gActiveProducers.load(std::memory_order_acquire);
}

/**
 * @brief 動作中の producer を 1 つ登録する
 *
 * @note
 * - 構築したら、ワーカーが終了する前に必ず破棄すること。shutdown 時のキュー排出の境界を正しく保つため。
 */
ProducerGuard::ProducerGuard() {
  gActiveProducers.fetch_add(1, std::memory_order_acq_rel);
}

/**
 * @brief 動作中の producer の登録を 1 つ解除する
 *
 * @note
 * - 手動で呼ばないこと。RAII により寿命に合わせて自動で実行される。
 */
ProducerGuard::~ProducerGuard() {
  gActiveProducers.fetch_sub(1, std::memory_order_acq_rel);
}

} // namespace runtime_shutdown
