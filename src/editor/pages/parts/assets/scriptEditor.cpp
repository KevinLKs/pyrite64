/**
* @copyright 2026 - Max Bebök
* @license MIT
*/
#include "scriptEditor.h"
#include "../../../script/scriptHost.h"
#include "../../../../project/assetManager.h"
#include "../../../../utils/fs.h"
#include "../../../../utils/logger.h"
#include "../../../imgui/helper.h"
#include "../../../imgui/notification.h"
#include "../../../imgui/theme.h"
#include "imgui.h"
#include "misc/cpp/imgui_stdlib.h"

#include <algorithm>
#include <unordered_map>

namespace
{
  struct ScriptEdit
  {
    std::string text{};
    uint64_t fileAge{0};
    bool dirty{false};
  };
  std::unordered_map<uint64_t, ScriptEdit> scriptEdits{};

  void loadScript(ScriptEdit &edit, const Project::AssetManagerEntry &asset)
  {
    edit.text = Utils::FS::loadTextFile(asset.path);
    edit.fileAge = asset.fileAge;
    edit.dirty = false;
  }
}

void Editor::ScriptEditor::draw(Project::AssetManagerEntry &asset)
{
  bool isNew = !scriptEdits.contains(asset.getUUID());
  auto &edit = scriptEdits[asset.getUUID()];
  if(isNew || asset.fileAge != edit.fileAge)loadScript(edit, asset);

  bool doRun = ImGui::Button(ICON_MDI_PLAY " Run");
  ImGui::SameLine();
  ImGui::BeginDisabled(!edit.dirty);
  bool doSave = ImGui::Button(ICON_MDI_CONTENT_SAVE " Save") || doRun;
  ImGui::EndDisabled();

  if(doSave) {
    if(Utils::FS::saveTextFile(asset.path, edit.text)) {
      edit.fileAge = Utils::FS::getFileAge(asset.path);
      asset.fileAge = edit.fileAge;
      edit.dirty = false;
    } else {
      Editor::Noti::add(Editor::Noti::Type::ERROR, "Failed to save script: " + asset.path);
    }
  }

  if(doRun) {
    Utils::Logger::log("Running script: " + asset.name);
    auto res = Editor::ScriptHost::eval(edit.text, asset.name);
    if(!res.success)Utils::Logger::log(res.text, Utils::Logger::LEVEL_ERROR);
  }

  ImGui::PushFont(ImGui::Theme::getFontMono(), 16_px);
  ImVec2 size{-1, std::max(ImGui::GetContentRegionAvail().y - 4_px, 200_px)};
  if(ImGui::InputTextMultiline("##Script", &edit.text, size, ImGuiInputTextFlags_AllowTabInput)) {
    edit.dirty = true;
  }
  ImGui::PopFont();
}
