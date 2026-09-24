/**
* @copyright 2026 - Max Bebök
* @license MIT
*/
#include "fileWatcher.h"

#include <cstdio>

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0600
    #undef _WIN32_WINNT
    #define _WIN32_WINNT 0x0600 // CancelIoEx
  #endif
  #include <windows.h>
#elif defined(__linux__)
  #include <sys/inotify.h>
  #include <poll.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <cerrno>
#endif

namespace fs = std::filesystem;

// =======================================================================================
// Windows: one thread per root, overlapped ReadDirectoryChangesW with a shared stop event.
// =======================================================================================
#if defined(_WIN32)

struct Utils::FileWatcher::Backend
{
  HANDLE stopEvent{nullptr};
  std::vector<std::thread> threads{};

  static void watchRoot(FileWatcher *owner, fs::path root, HANDLE stopEvent)
  {
    HANDLE dir = CreateFileW(
      root.wstring().c_str(),
      FILE_LIST_DIRECTORY,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
      nullptr
    );
    if(dir == INVALID_HANDLE_VALUE) {
      printf("FileWatcher: cannot open '%s' (err %lu)\n", root.string().c_str(), GetLastError());
      owner->pushOverflow();
      return;
    }

    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    // 64KB is the documented maximum for network shares, and plenty locally.
    // DWORD storage keeps the FILE_NOTIFY_INFORMATION records aligned.
    std::vector<DWORD> buffer(64 * 1024 / sizeof(DWORD));
    constexpr DWORD FILTER = FILE_NOTIFY_CHANGE_FILE_NAME
                           | FILE_NOTIFY_CHANGE_DIR_NAME
                           | FILE_NOTIFY_CHANGE_LAST_WRITE
                           | FILE_NOTIFY_CHANGE_SIZE
                           | FILE_NOTIFY_CHANGE_CREATION;

    for(;;)
    {
      ResetEvent(ov.hEvent);
      BOOL ok = ReadDirectoryChangesW(
        dir, buffer.data(), (DWORD)(buffer.size() * sizeof(DWORD)),
        TRUE, FILTER, nullptr, &ov, nullptr
      );
      if(!ok) {
        printf("FileWatcher: ReadDirectoryChangesW failed (err %lu)\n", GetLastError());
        owner->pushOverflow();
        break;
      }

      HANDLE waitOn[2] = {ov.hEvent, stopEvent};
      DWORD res = WaitForMultipleObjects(2, waitOn, FALSE, INFINITE);
      if(res != WAIT_OBJECT_0) {
        // stop requested (or wait failed): cancel the pending read and wait for it to finish
        CancelIoEx(dir, &ov);
        DWORD dummy = 0;
        GetOverlappedResult(dir, &ov, &dummy, TRUE);
        break;
      }

      DWORD bytes = 0;
      if(!GetOverlappedResult(dir, &ov, &bytes, FALSE)) {
        // ERROR_NOTIFY_ENUM_DIR: the buffer overflowed, changes were lost
        owner->pushOverflow();
        continue;
      }
      if(bytes == 0) {
        owner->pushOverflow();
        continue;
      }

      auto *ptr = reinterpret_cast<const uint8_t*>(buffer.data());
      for(;;)
      {
        auto *info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(ptr);
        std::wstring name(info->FileName, info->FileNameLength / sizeof(WCHAR));
        owner->push(root / name);

        if(info->NextEntryOffset == 0)break;
        ptr += info->NextEntryOffset;
      }
    }

    CloseHandle(ov.hEvent);
    CloseHandle(dir);
  }

  bool start(FileWatcher *owner, const std::vector<fs::path> &roots)
  {
    stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if(!stopEvent)return false;
    for(const auto &root : roots) {
      threads.emplace_back(watchRoot, owner, root, stopEvent);
    }
    return true;
  }

  void stop()
  {
    if(stopEvent)SetEvent(stopEvent);
    for(auto &t : threads) {
      if(t.joinable())t.join();
    }
    threads.clear();
    if(stopEvent)CloseHandle(stopEvent);
    stopEvent = nullptr;
  }
};

bool Utils::FileWatcher::isSupported() { return true; }

// =======================================================================================
// Linux: one inotify instance, one watch per directory (inotify is not recursive).
// =======================================================================================
#elif defined(__linux__)

struct Utils::FileWatcher::Backend
{
  int fd{-1};
  int stopPipe[2]{-1, -1};
  std::thread thread{};
  std::unordered_map<int, fs::path> wdToDir{};

  static constexpr uint32_t MASK = IN_CLOSE_WRITE | IN_MODIFY | IN_CREATE | IN_DELETE
                                 | IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF;

  void addWatch(FileWatcher *owner, const fs::path &dir)
  {
    // one watch per directory reports changes to the files directly inside it
    int wd = inotify_add_watch(fd, dir.c_str(), MASK | IN_ONLYDIR);
    if(wd < 0) {
      // ENOSPC: out of watches (fs.inotify.max_user_watches), changes below will be missed
      printf("FileWatcher: cannot watch '%s' (errno %d)\n", dir.c_str(), errno);
      owner->pushOverflow();
      return;
    }
    wdToDir[wd] = dir;
  }

  void addWatchRecursive(FileWatcher *owner, const fs::path &dir)
  {
    addWatch(owner, dir);
    std::error_code ec{};
    for(auto it = fs::recursive_directory_iterator{dir, ec}; !ec && it != fs::recursive_directory_iterator{}; it.increment(ec)) {
      if(it->is_directory(ec))addWatch(owner, it->path());
    }
  }

  void run(FileWatcher *owner)
  {
    alignas(inotify_event) char buf[64 * 1024];
    pollfd fds[2] = {{fd, POLLIN, 0}, {stopPipe[0], POLLIN, 0}};

    for(;;)
    {
      int pr = poll(fds, 2, -1);
      if(pr < 0) {
        if(errno == EINTR)continue;
        break;
      }
      if(fds[1].revents)break; // stop requested

      ssize_t len = read(fd, buf, sizeof(buf));
      if(len <= 0) {
        if(len < 0 && (errno == EAGAIN || errno == EINTR))continue;
        break;
      }

      for(char *p = buf; p < buf + len; )
      {
        auto *ev = reinterpret_cast<inotify_event*>(p);
        p += sizeof(inotify_event) + ev->len;

        if(ev->mask & IN_Q_OVERFLOW) {
          owner->pushOverflow();
          continue;
        }
        if(ev->mask & IN_IGNORED) {
          wdToDir.erase(ev->wd);
          continue;
        }

        auto it = wdToDir.find(ev->wd);
        if(it == wdToDir.end())continue;

        fs::path path = ev->len > 0 ? it->second / ev->name : it->second;
        owner->push(path);

        // new sub-directory (created or moved in): watch it and everything below it
        if((ev->mask & IN_ISDIR) && (ev->mask & (IN_CREATE | IN_MOVED_TO))) {
          addWatchRecursive(owner, path);
        }
      }
    }
  }

  bool start(FileWatcher *owner, const std::vector<fs::path> &roots)
  {
    fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if(fd < 0)return false;
    if(pipe2(stopPipe, O_CLOEXEC) != 0) {
      close(fd); fd = -1;
      return false;
    }
    for(const auto &root : roots)addWatchRecursive(owner, root);
    thread = std::thread([this, owner]{ run(owner); });
    return true;
  }

  void stop()
  {
    if(stopPipe[1] >= 0) {
      char c = 1;
      [[maybe_unused]] auto r = write(stopPipe[1], &c, 1);
    }
    if(thread.joinable())thread.join();
    if(fd >= 0)close(fd);
    if(stopPipe[0] >= 0)close(stopPipe[0]);
    if(stopPipe[1] >= 0)close(stopPipe[1]);
    fd = stopPipe[0] = stopPipe[1] = -1;
    wdToDir.clear();
  }
};

bool Utils::FileWatcher::isSupported() { return true; }

// =======================================================================================
// Other platforms (macOS): no native backend yet, the AssetManager falls back to polling.
// =======================================================================================
#else

struct Utils::FileWatcher::Backend
{
  bool start(FileWatcher*, const std::vector<fs::path>&) { return false; }
  void stop() {}
};

bool Utils::FileWatcher::isSupported() { return false; }

#endif

// =======================================================================================
// Shared
// =======================================================================================

Utils::FileWatcher::FileWatcher() = default;

Utils::FileWatcher::~FileWatcher()
{
  stop();
}

bool Utils::FileWatcher::start(const std::vector<fs::path> &roots)
{
  stop();
  backend = std::make_unique<Backend>();
  if(!backend->start(this, roots)) {
    backend.reset();
    return false;
  }
  running = true;
  return true;
}

void Utils::FileWatcher::stop()
{
  if(backend) {
    backend->stop();
    backend.reset();
  }
  running = false;
  std::lock_guard lock{mtx};
  pending.clear();
  overflowed = false;
}

void Utils::FileWatcher::push(const fs::path &path)
{
  std::lock_guard lock{mtx};
  pending[path.string()] = Clock::now();
}

std::vector<std::string> Utils::FileWatcher::takeSettled(std::chrono::milliseconds settle, bool &overflow)
{
  overflow = overflowed.exchange(false);

  std::vector<std::string> res{};
  auto now = Clock::now();
  std::lock_guard lock{mtx};
  if(overflow) {
    // a full rescan follows, the individual paths are redundant
    pending.clear();
    return res;
  }
  for(auto it = pending.begin(); it != pending.end(); ) {
    if(now - it->second >= settle) {
      res.push_back(it->first);
      it = pending.erase(it);
    } else {
      ++it;
    }
  }
  return res;
}
