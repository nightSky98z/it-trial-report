#include "config/runtime_config.h"
#include "config/zone_config.h"
#include "db/mysql_worker.h"
#include "db/redis_worker.h"
#include "game/game_loop.h"
#include "utils/error_utils.h"
#include "utils/log.h"
#include "network/zone_http.h"
#include "runtime/shutdown.h"
#include <sys/stat.h>
#include <ctime>
#include <filesystem>
#include <system_error>
#include <csignal>
#include <iostream>
#include <pthread.h>
#include <string>
#include <thread>
#include <vector>

namespace {

/**
 * @brief コンソールからの正常終了の受け付け
 *
 * @note
 * - このスレッドはユーザー入力を shutdown 要求に変換するだけ。
 * - リソースの解放は、各ワーカーが自分の担当範囲で行う。
 */
void runConsoleShutdownWatcher() {
  zone_log::info("normal shutdown: type 'q', 'exit', or 'quit' then press Enter");
  std::string line;
  while (!runtime_shutdown::isShutdownRequested() &&
         std::getline(std::cin, line)) {
    if (runtime_shutdown::isConsoleShutdownCommand(line)) {
      runtime_shutdown::requestShutdown();
      zone_log::info("normal shutdown requested by console");
      return;
    }
  }
}

/**
 * @brief 安全な終了に使うシグナルをメインスレッドでブロックする
 *
 * @param outSet 成功時、後で sigwait に使うシグナル集合を書き込む。
 *
 * @retval true SIGINT/SIGTERM を現在のスレッドでブロックした。この後に作成するワーカーはこのマスクを引き継ぐ。
 * @retval false シグナルマスクの設定に失敗した。呼び出し側はコンソールからの終了にしか頼れない。
 */
bool blockShutdownSignals(sigset_t &outSet) {
  sigemptyset(&outSet);
  sigaddset(&outSet, SIGINT);
  sigaddset(&outSet, SIGTERM);

  const int rc = pthread_sigmask(SIG_BLOCK, &outSet, nullptr);
  if (rc != 0) {
    zone_log::error("failed to install shutdown signal mask: %d", rc);
    return false;
  }
  return true;
}

/**
 * @brief SIGINT/SIGTERM を通常の shutdown 要求に変換する
 *
 * @param signalSet メインスレッドでブロック済みのシグナル集合。sigwait 用に値で受け取る。
 *
 * @note
 * - sigwait は同期的な待機なので、このスレッドは async-signal-safe の制約を受けず、通常のコードを呼べる。
 * - リソースの解放は、引き続き各ワーカーが shutdown 要求を検知した後に行う。
 */
void runSignalShutdownWatcher(sigset_t signalSet) {
  while (!runtime_shutdown::isShutdownRequested()) {
    int receivedSignal = 0;
    const int rc = sigwait(&signalSet, &receivedSignal);
    if (rc != 0) {
      zone_log::error("signal shutdown watcher failed: %d", rc);
      return;
    }

    runtime_shutdown::requestShutdown();
    zone_log::info("normal shutdown requested by signal %d", receivedSignal);
    return;
  }
}

} // namespace

/**
 * @brief 現在の実行ファイルの更新日時を出力する
 *
 * @param exePath argv[0]。相対パスのこともある。取得できなくても警告を出すだけで起動は続ける。
 *
 * @note
 * - 「最新のビルドで動いているか」を確認するためだけのもので、業務上の判断には一切使わない。
 */
static void logBinaryStamp(const char *exePath) {
  if (!exePath) return;
  std::error_code ec;
  const auto path = std::filesystem::absolute(exePath, ec);
  if (ec) {
    zone_log::warning("zone binary stamp unavailable: %s", ec.message().c_str());
    return;
  }
  struct stat st {};
  if (::stat(path.c_str(), &st) != 0) {
    zone_log::warning("zone binary stamp unavailable: stat failed");
    return;
  }
  char buf[64];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S",
                std::localtime(&st.st_mtime));
  zone_log::info("zone binary %s (mtime %s)", path.c_str(), buf);
}

// argv[0] は実行ファイルの場所を特定して更新日時を表示するために使う（どのビルドが動いているかを確認しやすくするため）。
int main(int /*argc*/, char **argv) {
  sigset_t shutdownSignalSet{};
  const bool signalShutdownEnabled = blockShutdownSignals(shutdownSignalSet);

  zone::ZoneConfig cfg;
  if (!zone::loadFromEnvFile(cfg.envFile, cfg)) {
    zone_log::error("Failed to load env.zone");
    return utils::Error(utils::ErrorCode::LoadEnvFileFailed);
  }

    // バイナリの識別情報：調査時に「今動いているのが、ビルドし直したばかりのファイルか」を確認するために出力する。
    // このプロジェクトでは、古いプロセスがポートを握ったまま残り、リクエストがそちらに流れて
    // 「コードを変えたのに挙動が変わらない」という事象が実際に起きたことがある。
    //
    // __DATE__/__TIME__ ではなく、実行ファイル自体の更新日時を読む。
    // __DATE__/__TIME__ は main.cpp が再コンパイルされたときにしか更新されず、他のファイルを変えたときは
    // 古い値のままになるので、かえって誤った判断材料になってしまう。
  logBinaryStamp(argv[0]);
  zone_log::info("zone MySQL %s:%d db=%s", cfg.mysqlHost, cfg.mysqlPort, cfg.mysqlDb);
  zone_log::info("zone Redis %s:%d/%d", cfg.redisHost, cfg.redisPort, cfg.redisDb);
  zone_log::info("zone Listen %s:%d tick_rate=%d zone_id=%u",
                 cfg.listenHost, cfg.listenPort, cfg.tickRate, cfg.zoneId);
  if (cfg.ticketCryptoUsesDevDefaults) {
    zone_log::warning(
        "zone using dev ticket crypto keys; set TICKET_AES256_GCM_V1_SLOT*_KEY_HEX for non-dev");
  }

  if (!mysql_worker::bootstrap(cfg)) {
    zone_log::error("zone mysql bootstrap failed, exit");
    return 1;
  }

  std::thread(runConsoleShutdownWatcher).detach();
  if (signalShutdownEnabled) {
    std::thread(runSignalShutdownWatcher, shutdownSignalSet).detach();
  }

  std::vector<std::thread> threads;

    // ゲームロジックスレッド（map_id でルーティングして分担する）
  for (uint8_t i = 0; i < runtime_config::NUM_GAME_WORKERS; i++) {
    threads.emplace_back([i, &cfg] { game::run(i, cfg); });
  }

    // MySQL への一括書き込みスレッド
  threads.emplace_back([&cfg] {
    mysql_worker::run(cfg, 30);
  });

    // Redis ワーカースレッド（船の経験値のキャッシュ／転送）
  threads.emplace_back([&cfg] {
    redis_worker::run(cfg);
  });

    // HTTP API + WebSocket サーバー（公開ポート。クライアントの直接接続とゲートウェイからの API 呼び出しを受ける）
  threads.emplace_back([&cfg] {
    zone_http::run(0, cfg);
  });

  for (auto &t : threads)
    t.join();

  zone_log::info("zone stopped");
  return 0;
}
