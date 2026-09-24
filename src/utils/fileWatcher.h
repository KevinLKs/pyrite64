/**
* @copyright 2026 - Max Bebök
* @license MIT
*/
#pragma once
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Utils
{
  /**
   * Recursive directory watcher backed by OS change notifications
   * (ReadDirectoryChangesW on Windows, inotify on Linux).
   *
   * Events are collected on background threads. The owner drains them from the main
   * thread with takeSettled(), which only hands out paths that have been quiet for a
   * while, so a file that is still being written is not picked up half-finished.
   *
   * Reported paths can be files or directories, and may no longer exist (deleted or
   * renamed away). The caller decides what that means by checking the disk.
   */
  class FileWatcher
  {
    public:
      using Clock = std::chrono::steady_clock;

      FileWatcher();
      ~FileWatcher();

      FileWatcher(const FileWatcher&) = delete;
      FileWatcher& operator=(const FileWatcher&) = delete;

      // False on platforms without a native backend (the caller should fall back to polling).
      static bool isSupported();

      // Starts watching all roots recursively. Stops any previous watch first.
      bool start(const std::vector<std::filesystem::path> &roots);
      void stop();
      [[nodiscard]] bool isRunning() const { return running; }

      /**
       * Returns every path whose most recent event is at least `settle` old, and forgets it.
       * `overflow` is set if the OS dropped events since the last call, in which case the
       * caller must do one full rescan because some changes are unknown.
       */
      std::vector<std::string> takeSettled(std::chrono::milliseconds settle, bool &overflow);

    private:
      struct Backend;

      std::mutex mtx{};
      std::unordered_map<std::string, Clock::time_point> pending{};
      std::atomic<bool> overflowed{false};
      std::atomic<bool> running{false};
      std::unique_ptr<Backend> backend{};

      void push(const std::filesystem::path &path);
      void pushOverflow() { overflowed = true; }
  };
}
