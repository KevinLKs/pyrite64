/**
* @copyright 2025 - Max Bebök
* @license MIT
*/
#include "logWindow.h"
#include <vector>
#include "imgui.h"
#include "../../../context.h"
#include "../../../utils/logger.h"
#include "../../imgui/theme.h"
#include "imgui_internal.h"
#include "../../../utils/fs.h"
#include "../../../utils/time.h"
#include "../../imgui/notification.h"
#include "../../script/scriptHost.h"
#include "misc/cpp/imgui_stdlib.h"

namespace
{
  constinit uint32_t lastLen{0};
  constinit int scrollFrames{0};
  constinit int inputFocusFrames{0};
  std::string shellInput{};

  std::vector<std::string> cmdHistory{};
  size_t historyIdx{0};

  int onShellInput(ImGuiInputTextCallbackData *data)
  {
    size_t oldIdx = historyIdx;
    if(data->EventKey == ImGuiKey_UpArrow && historyIdx > 0)--historyIdx;
    if(data->EventKey == ImGuiKey_DownArrow && historyIdx < cmdHistory.size())++historyIdx;
    if(historyIdx == oldIdx)return 0;

    data->DeleteChars(0, data->BufTextLen);
    if(historyIdx < cmdHistory.size()) {
      data->InsertChars(0, cmdHistory[historyIdx].c_str());
    }
    return 0;
  }

  void runShellCommand()
  {
    if(shellInput.empty())return;
    if(cmdHistory.empty() || cmdHistory.back() != shellInput)cmdHistory.push_back(shellInput);
    if(cmdHistory.size() > 64)cmdHistory.erase(cmdHistory.begin());
    historyIdx = cmdHistory.size();

    Utils::Logger::log("> " + shellInput);
    auto res = Editor::ScriptHost::eval(shellInput);
    Utils::Logger::log(res.text, res.success ? Utils::Logger::LEVEL_INFO : Utils::Logger::LEVEL_ERROR);
    shellInput.clear();
  }
}

void Editor::LogWindow::draw()
{
  auto log = Utils::Logger::getLog();

  //ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 4));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0)); // Remove space between children

  ImGui::BeginChild("TOP", ImVec2(0, 22_px),
    ImGuiChildFlags_AlwaysUseWindowPadding,
    ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
  );

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4_px, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0);
    float btnWidth = 64_px;
    if(ImGui::Button("Clear", {btnWidth, 0})) {
      Utils::Logger::clear();
    }

    ImGui::SameLine();
    if(ImGui::Button("Copy", {btnWidth, 0})) {
      ImGui::SetClipboardText(log.c_str());
    }

    ImGui::SameLine();
    if(ImGui::Button("Save", {btnWidth, 0}))
    {
      std::string dateTimeStr = Utils::Time::getDateTimeStr();
      auto path = fs::path{ctx.project->getPath()} / ("log_" + dateTimeStr + ".txt");
      if(Utils::FS::saveTextFile(path, log)) {
        Editor::Noti::add(Noti::Type::SUCCESS, "Log saved to: " + path.string());
      } else {
        Editor::Noti::add(Noti::Type::ERROR, "Failed to save log to: " + path.string());
      }
    }

    ImGui::PopStyleVar(2);

  ImGui::EndChild();

  float shellHeight = ImGui::GetFrameHeight();
  ImGui::BeginChild("LOG", ImVec2(0, -shellHeight), ImGuiChildFlags_Borders);
  ImGui::PushFont(ImGui::Theme::getFontMono(), 16_px);

  ImGui::PushID("LOG");
  ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::Theme::getColor("logBackground", {0.05f, 0.05f, 0.06f, 1.0f}));

  const char* child_window_name = NULL;
  ImFormatStringToTempBuffer(&child_window_name, NULL, "Log/LOG_43838E2B/%08X", ImGui::GetID(""));

  auto &logStripped = Utils::Logger::getLogStripped();
  ImGui::InputTextMultiline("", (char*)logStripped.data(), logStripped.size()+1, {
    ImGui::GetWindowSize().x-6,
    ImGui::GetWindowSize().y-6
  }, ImGuiInputTextFlags_ReadOnly);


  if(lastLen != logStripped.length()) {
    lastLen = logStripped.length();
    scrollFrames = 2;
  }

  if(scrollFrames > 0) 
  {
    --scrollFrames;
    if(auto *childWindow = ImGui::FindWindowByName(child_window_name)) {
      ImGui::SetScrollY(childWindow, childWindow->ScrollMax.y);
    }
  }

  ImGui::PopStyleColor();
  ImGui::PopID();

  ImGui::PopFont();
  ImGui::EndChild();

  ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0)); // avoids blue outline the refocus does
  ImGui::SetNextItemWidth(-1);

  bool submitted = ImGui::InputTextWithHint(
    "##Shell", "Run JS command...", &shellInput,
    ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory, 
    onShellInput
  );
  ImGui::PopStyleColor();

  if(submitted) {
    runShellCommand();
    inputFocusFrames = 2;
  }

  if(inputFocusFrames > 0) {
    --inputFocusFrames;
    ImGui::SetKeyboardFocusHere(-1);
  }

  ImGui::PopStyleVar(2);
}
