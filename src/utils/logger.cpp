/**
* @copyright 2025 - Max Bebök
* @license MIT
*/
#include "logger.h"

#include <atomic>
#include <mutex>
#include <chrono>
#include <ctime>
#include <cstdio>

namespace
{
  std::mutex mtx{};
  std::string buff{};
  std::atomic_bool logChanged{false};
  constexpr size_t MAX_BUFF_SIZE = 1024 * 100; // 100kb

  constinit Utils::Logger::LogOutputFunc outputFunc = nullptr;
  std::string logStripped{};
}

std::string Utils::Logger::timestamp()
{
  using namespace std::chrono;
  auto now = system_clock::now();
  auto t = system_clock::to_time_t(now);
  auto ms = (int)(duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000);
  std::tm tmLocal{};
  #if defined(_WIN32)
    localtime_s(&tmLocal, &t);
  #else
    localtime_r(&t, &tmLocal);
  #endif
  char res[32];
  snprintf(res, sizeof(res), "%02d:%02d:%02d.%03d ", tmLocal.tm_hour, tmLocal.tm_min, tmLocal.tm_sec, ms);
  return res;
}

void Utils::Logger::setOutput(LogOutputFunc outFunc) {
  outputFunc = outFunc;
}

void Utils::Logger::log(const std::string&msg, int level)
{
  auto ts = timestamp();
  std::lock_guard lock{mtx};
  switch(level) {
    default:
    case LEVEL_INFO:
      buff += "[INF] ";
      break;
    case LEVEL_WARN:
      buff += "[WRN] ";
      break;
    case LEVEL_ERROR:
      buff += "[ERR] ";
      break;
  }
  buff += ts + msg + "\n";
  logChanged = true;

  if (outputFunc) {
    outputFunc(buff);
    buff = "";
  }
}

void Utils::Logger::logRaw(const std::string&msg, int level) {
  std::lock_guard lock{mtx};
  buff += msg;
  logChanged = true;

  if (outputFunc) {
    outputFunc(buff);
    buff = "";
  }
}

void Utils::Logger::clear() {
  std::lock_guard lock{mtx};
  buff = "";
  logChanged = true;
}

std::string Utils::Logger::getLog() {
  std::lock_guard lock{mtx};
  if (buff.length() > MAX_BUFF_SIZE) {
    buff = buff.substr(buff.length() - MAX_BUFF_SIZE);
    logChanged = true;
  }
  return buff;
}

const std::string& Utils::Logger::getLogStripped()
{
  if(!logChanged)return logStripped;
  auto log = getLog();

  logStripped = "";

  bool inAnsi = false;
  for(char c : log)
  {
    if(c == '\x1b') {
      inAnsi = true;
    } else if(inAnsi && (c == 'm' || c == 'K')) {
      inAnsi = false;
    } else if(!inAnsi)
    {
      if(c < 32 && c != '\n' && c != '\t') {
        c = '?';
      }
      logStripped += c;
    }
  }
  logChanged = false;
  return logStripped;
}
