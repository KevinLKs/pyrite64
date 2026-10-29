/**
* @license MIT
*/
#include "mcpBridge.h"
#include "script/scriptHost.h"
#include "../utils/logger.h"
#include "json.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  using SocketT = SOCKET;
  constexpr SocketT INVALID_SOCK = INVALID_SOCKET;
  inline void closeSock(SocketT s) { closesocket(s); }
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <unistd.h>
  using SocketT = int;
  constexpr SocketT INVALID_SOCK = -1;
  inline void closeSock(SocketT s) { ::close(s); }
#endif

namespace
{
  constexpr int DEFAULT_PORT = 6464;
  constexpr auto EVAL_TIMEOUT = std::chrono::seconds(30);
  constexpr size_t MAX_LINE = 8 * 1024 * 1024;

  struct Job
  {
    std::string code{};
    std::promise<Editor::ScriptHost::EvalResult> result{};
  };

  std::mutex jobMutex{};
  std::deque<std::shared_ptr<Job>> jobs{};

  std::thread netThread{};
  std::atomic_bool running{false};
  std::atomic<SocketT> listenSock{INVALID_SOCK};
  std::atomic<SocketT> clientSock{INVALID_SOCK};
  int port{0};

  bool sendAll(SocketT s, const std::string &data)
  {
    size_t sent = 0;
    while(sent < data.size()) {
      auto n = ::send(s, data.data() + sent, (int)(data.size() - sent), 0);
      if(n <= 0)return false;
      sent += (size_t)n;
    }
    return true;
  }

  std::string handleRequest(const std::string &line)
  {
    nlohmann::json res{};
    auto req = nlohmann::json::parse(line, nullptr, false);
    if(req.is_discarded() || !req.is_object()) {
      res["ok"] = false;
      res["result"] = "invalid JSON request";
      return res.dump() + "\n";
    }
    res["id"] = req.contains("id") ? req["id"] : nlohmann::json{};

    if(req.value("op", "") == "ping") {
      res["ok"] = true;
      res["result"] = "pong";
      return res.dump() + "\n";
    }

    auto job = std::make_shared<Job>();
    job->code = req.value("code", "");
    auto future = job->result.get_future();
    {
      std::lock_guard lock{jobMutex};
      jobs.push_back(job);
    }

    if(future.wait_for(EVAL_TIMEOUT) != std::future_status::ready) {
      res["ok"] = false;
      res["result"] = "timed out waiting for the editor (is it minimized, frozen or showing a modal?)";
    } else {
      auto evalRes = future.get();
      res["ok"] = evalRes.success;
      res["result"] = evalRes.text;
    }
    return res.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
  }

  void serveClient(SocketT s)
  {
    std::string buffer{};
    char chunk[4096];
    while(running) {
      auto n = ::recv(s, chunk, sizeof(chunk), 0);
      if(n <= 0)break;
      buffer.append(chunk, (size_t)n);
      if(buffer.size() > MAX_LINE) {
        sendAll(s, R"({"ok":false,"result":"request too large"})" "\n");
        break;
      }

      size_t pos;
      while((pos = buffer.find('\n')) != std::string::npos) {
        std::string line = buffer.substr(0, pos);
        buffer.erase(0, pos + 1);
        if(!line.empty() && line.back() == '\r')line.pop_back();
        if(line.empty())continue;
        if(!sendAll(s, handleRequest(line)))return;
      }
    }
  }

  void netLoop()
  {
    while(running) {
      sockaddr_in addr{};
      socklen_t addrLen = sizeof(addr);
      SocketT s = ::accept(listenSock, (sockaddr*)&addr, &addrLen);
      if(s == INVALID_SOCK)continue; // stop() closes the listen socket to break out
      clientSock = s;
      serveClient(s);
      clientSock = INVALID_SOCK;
      closeSock(s);
    }
  }
}

namespace Editor::McpBridge
{
  void start()
  {
    if(running)return;

    port = DEFAULT_PORT;
    if(const char *env = std::getenv("PYRITE_MCP_PORT")) {
      port = std::atoi(env);
    }
    if(port <= 0) {
      Utils::Logger::log("MCP bridge disabled (PYRITE_MCP_PORT=0)");
      return;
    }

    #ifdef _WIN32
      WSADATA wsa{};
      if(WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        Utils::Logger::log("MCP bridge: WSAStartup failed", Utils::Logger::LEVEL_ERROR);
        return;
      }
    #endif

    SocketT s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if(s == INVALID_SOCK) {
      Utils::Logger::log("MCP bridge: could not create socket", Utils::Logger::LEVEL_ERROR);
      return;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // local machine only

    if(::bind(s, (sockaddr*)&addr, sizeof(addr)) != 0 || ::listen(s, 1) != 0) {
      Utils::Logger::log("MCP bridge: port " + std::to_string(port)
        + " is in use (another editor open?), bridge disabled", Utils::Logger::LEVEL_WARN);
      closeSock(s);
      return;
    }

    listenSock = s;
    running = true;
    netThread = std::thread(netLoop);
    Utils::Logger::log("MCP bridge listening on 127.0.0.1:" + std::to_string(port));
  }

  void stop()
  {
    if(!running)return;
    running = false;

    // unblock accept() / recv()
    SocketT ls = listenSock.exchange(INVALID_SOCK);
    if(ls != INVALID_SOCK) {
      #ifndef _WIN32
        ::shutdown(ls, SHUT_RDWR);
      #endif
      closeSock(ls);
    }
    SocketT cs = clientSock.load();
    if(cs != INVALID_SOCK) {
      #ifdef _WIN32
        ::shutdown(cs, SD_BOTH);
      #else
        ::shutdown(cs, SHUT_RDWR);
      #endif
    }

    // fail pending jobs so the network thread isn't left waiting
    {
      std::lock_guard lock{jobMutex};
      for(auto &job : jobs)job->result.set_value({"editor is shutting down", false});
      jobs.clear();
    }

    if(netThread.joinable())netThread.join();

    #ifdef _WIN32
      WSACleanup();
    #endif
  }

  void poll()
  {
    if(!running)return;

    std::deque<std::shared_ptr<Job>> pending{};
    {
      std::lock_guard lock{jobMutex};
      pending.swap(jobs);
    }

    for(auto &job : pending) {
      if(!ScriptHost::isRunning()) {
        job->result.set_value({"No project is open in the editor", false});
        continue;
      }
      job->result.set_value(ScriptHost::eval(job->code, "<mcp>"));
    }
  }

  bool isListening() { return running; }
  int getPort() { return port; }
}
