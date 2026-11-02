/**
* @copyright 2025 - Max Bebök
* @license MIT
*/
#include "projectSettings.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include "imgui.h"
#include "../../../context.h"
#include "../../../utils/logger.h"
#include "../../../project/romMeta.h"
#include "../../actions.h"
#include "misc/cpp/imgui_stdlib.h"
#include "../../imgui/helper.h"
#include "IconsMaterialDesignIcons.h"

namespace
{
  using namespace Project;

  // True if an image asset reference cannot be embedded as-is (missing, wrong format,
  // or larger than the 320x240 / 240x320 limit enforced by n64metadata).
  bool imageRefInvalid(uint64_t uuid, std::string &reason)
  {
    if(uuid == 0) return false;
    auto *e = ctx.project->getAssets().getEntryByUUID(uuid);
    if(!e || e->type != FileType::IMAGE) { reason = "asset missing"; return true; }

    std::string ext = std::filesystem::path(e->path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if(ext != ".png" && ext != ".jpg" && ext != ".jpeg") { reason = "not PNG/JPG"; return true; }

    int w = e->texture ? e->texture->getWidth() : 0;
    int h = e->texture ? e->texture->getHeight() : 0;
    bool okSize = (w <= 320 && h <= 240) || (w <= 240 && h <= 320);
    if(w > 0 && !okSize) {
      reason = std::to_string(w) + "x" + std::to_string(h) + " exceeds 320x240";
      return true;
    }
    return false;
  }

  // Picks an IMAGE asset for `uuid`, with a clear button and an inline warning when invalid.
  // Returns true if the clear (X) button was pressed this frame, in addition to setting uuid=0.
  bool imagePicker(const std::string &label, uint64_t &uuid)
  {
    ImTable::add(label);
    ImGui::PushID(label.c_str());

    const auto &imgs = ctx.project->getAssets().getTypeEntries(FileType::IMAGE);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 28_px);
    ImTable::addAssetVecComboBox("", imgs, uuid, true);
    ImGui::SameLine();
    bool cleared = ImGui::Button(ICON_MDI_CLOSE);
    if(cleared) uuid = 0;

    std::string reason;
    if(imageRefInvalid(uuid, reason)) {
      ImGui::TextColored({1.0f, 0.3f, 0.3f, 1.0f}, ICON_MDI_ALERT " %s", reason.c_str());
    }
    ImGui::PopID();
    return cleared;
  }

  void drawMetaLang(MetaLang &lang)
  {
    ImTable::start("MetaFields");
    ImTable::add("Name", lang.name);
    ImTable::add("Author", lang.author);
    ImTable::add("Release Date", lang.releaseDate); // YYYY-MM-DD
    ImTable::add("OSI License", lang.osiLicense);    // SPDX code, e.g. MIT
    ImTable::add("Website", lang.website);
    ImTable::add("Age Rating", lang.ageRating);      // 0-18
    ImTable::add("Short Desc", lang.shortDesc);

    ImTable::add("Long Desc");
    ImGui::InputTextMultiline("##longdesc", &lang.longDesc, ImVec2(-FLT_MIN, 80_px));

    // Screenshots: variable-length list of image refs, the X removes the whole entry.
    int removeShot = -1;
    for(size_t i = 0; i < lang.screenshots.size(); ++i) {
      ImGui::PushID((int)i);
      if(imagePicker("Screenshot " + std::to_string(i + 1), lang.screenshots[i])) removeShot = (int)i;
      ImGui::PopID();
    }
    if(removeShot >= 0) lang.screenshots.erase(lang.screenshots.begin() + removeShot);
    ImTable::add("");
    if(ImGui::Button(ICON_MDI_PLUS " Screenshot")) lang.screenshots.push_back(0);

    ImTable::end();

    if(ImGui::CollapsingSubHeader("Box Art")) {
      ImTable::start("BoxArt");
      imagePicker("Front", lang.boxFront);
      imagePicker("Back", lang.boxBack);
      imagePicker("Top", lang.boxTop);
      imagePicker("Bottom", lang.boxBottom);
      imagePicker("Left", lang.boxLeft);
      imagePicker("Right", lang.boxRight);
      ImTable::end();
    }
    if(ImGui::CollapsingSubHeader("Cart Art")) {
      ImTable::start("CartArt");
      imagePicker("Front", lang.cartFront);
      imagePicker("Back", lang.cartBack);
      ImTable::end();
    }
  }

  void drawRomHeader()
  {
    auto &h = ctx.project->conf.romHeader;
    ImTable::start("RomHeader");
    ImTable::addComboBox("Category", h.category, RomMeta::labels(RomMeta::CATEGORY));
    ImTable::addComboBox("Region", h.region, RomMeta::labels(RomMeta::REGION));
    ImTable::addComboBox("Save Type", h.saveType, RomMeta::labels(RomMeta::SAVETYPE));
    ImTable::addCheckBox("Region Free", h.regionFree);
    ImTable::addCheckBox("Real-Time Clock", h.rtc);
    auto ctrlLabels = RomMeta::labels(RomMeta::CONTROLLER);
    for(int i = 0; i < 4; ++i) {
      ImTable::addComboBox("Controller " + std::to_string(i + 1), h.controllers[i], ctrlLabels);
    }
    ImTable::end();
  }

  void drawMetadata()
  {
    auto &m = ctx.project->conf.metadata;
    ImTable::start("MetadataTop");
    ImTable::addCheckBox("Embed Metadata", m.enabled);
    ImTable::end();
    if(!m.enabled) return;

    if(ImGui::BeginTabBar("MetaLangs")) {
      int removeLang = -1;
      for(size_t i = 0; i < m.langs.size(); ++i) {
        auto &lang = m.langs[i];
        std::string tabName = lang.lang.empty() ? "Default" : lang.lang;
        if(ImGui::BeginTabItem((tabName + "##" + std::to_string(i)).c_str())) {
          if(i > 0) {
            if(ImGui::SmallButton(ICON_MDI_DELETE " Remove Language")) removeLang = (int)i;
          }
          drawMetaLang(lang);
          ImGui::EndTabItem();
        }
      }

      // "+" tab opens a popup to enter a new language code.
      if(ImGui::TabItemButton(ICON_MDI_PLUS, ImGuiTabItemFlags_Trailing)) {
        ImGui::OpenPopup("AddLang");
      }
      if(ImGui::BeginPopup("AddLang")) {
        static std::string code;
        ImGui::TextUnformatted("Language code (e.g. de, ja):");
        ImGui::InputText("##code", &code);
        if(ImGui::Button("Add") && !code.empty()) {
          MetaLang l{};
          l.lang = code;
          m.langs.push_back(l);
          code.clear();
          ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
      }

      if(removeLang > 0) m.langs.erase(m.langs.begin() + removeLang);
      ImGui::EndTabBar();
    }
  }

  // Validate every language's image refs (not just the visible tab) so Save can be blocked.
  bool metadataHasError()
  {
    if(!ctx.project->conf.metadata.enabled) return false;
    std::string reason;
    auto chk = [&](uint64_t u) { return imageRefInvalid(u, reason); };
    for(const auto &l : ctx.project->conf.metadata.langs) {
      if(chk(l.boxFront) || chk(l.boxBack) || chk(l.boxTop) || chk(l.boxBottom) || chk(l.boxLeft) || chk(l.boxRight)) return true;
      if(chk(l.cartFront) || chk(l.cartBack)) return true;
      for(auto u : l.screenshots) if(chk(u)) return true;
    }
    return false;
  }
  // Short text for the bindings of one action, e.g. "A, Stick Up"
  std::string bindSummary(const InputMap::Action &a)
  {
    std::string res{};
    auto addPart = [&](const char* txt) { if(!res.empty())res += ", "; res += txt; };
    if(a.type == InputMap::Type::STICK) {
      if(a.binds & InputMap::BIND_STICK)addPart("Analog Stick");
      if(a.binds & InputMap::BIND_DPAD)addPart("D-Pad");
      if(a.binds & InputMap::BIND_CBTN)addPart("C-Buttons");
    } else {
      for(size_t i = 0; i < InputMap::BINDS.size(); ++i) {
        if(a.binds & InputMap::bit((int)i))addPart(InputMap::BINDS[i].label);
      }
    }
    return res.empty() ? "(nothing bound)" : res;
  }

  // Checkbox that sets/clears all bits of `mask` together
  void maskCheckBox(const char* label, uint32_t &binds, uint32_t mask)
  {
    bool on = (binds & mask) != 0;
    if(ImGui::Checkbox(label, &on)) {
      binds = on ? (binds | mask) : (binds & ~mask);
    }
  }

  // Graph of stick distance (x) to output strength (y) for a STICK action
  void drawStickCurve(const InputMap::Action &a)
  {
    constexpr int SAMPLES = 64;
    float vals[SAMPLES];
    for(int i = 0; i < SAMPLES; ++i) {
      float dist = 90.0f * (float)i / (float)(SAMPLES - 1); // N64 stick max is about 85-90
      float mag = 0.0f;
      if(dist > (float)a.deadzone) {
        mag = a.range > a.deadzone ? (dist - (float)a.deadzone) / (float)(a.range - a.deadzone) : 1.0f;
        mag = std::min(mag, 1.0f);
        if(a.curve != 1.0f)mag = powf(mag, a.curve);
      }
      vals[i] = mag;
    }
    ImGui::PlotLines("##curve", vals, SAMPLES, 0, nullptr, 0.0f, 1.0f, ImVec2(-FLT_MIN, 60_px));
    ImGui::TextDisabled("Stick distance (0 to 90) left to right, strength (0 to 1) bottom to top");
  }

  void drawInputAction(InputMap::Action &a, int idx, bool &remove)
  {
    ImGui::PushID(idx);
    auto nameErr = InputMap::validateName(a.name);
    std::string header = (nameErr.empty() ? "" : ICON_MDI_ALERT " ")
      + std::string(a.type == InputMap::Type::STICK ? ICON_MDI_GAMEPAD_ROUND_OUTLINE " " : ICON_MDI_GAMEPAD_CIRCLE_OUTLINE " ")
      + a.name + "  -  " + bindSummary(a) + "###action";

    bool open = ImGui::TreeNodeEx(header.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth);
    if(open)
    {
      ImTable::start("Action");

      ImTable::add("Name");
      ImGui::InputText("##name", &a.name);
      if(!nameErr.empty()) {
        ImGui::TextColored({1.0f, 0.3f, 0.3f, 1.0f}, ICON_MDI_ALERT " %s", nameErr.c_str());
      }
      ImTable::add("Code");
      ImGui::TextDisabled("Action::%s", a.name.c_str());

      int type = (int)a.type;
      if(ImTable::addComboBox("Type", type, {"Button", "Stick"})) {
        if((InputMap::Type)type != a.type) {
          a.type = (InputMap::Type)type;
          a.binds = a.type == InputMap::Type::STICK ? InputMap::BIND_STICK : 0;
        }
      }

      if(a.type == InputMap::Type::BUTTON)
      {
        // rows: face + shoulder buttons, D-pad, C-buttons, stick directions
        constexpr int ROWS[4][6] = {
          {0, 1, 2, 3, 4, 5}, {6, 7, 8, 9, -1, -1}, {10, 11, 12, 13, -1, -1}, {14, 15, 16, 17, -1, -1},
        };
        constexpr const char* ROW_LABELS[4] = {"Buttons", "D-Pad", "C-Buttons", "Stick"};
        for(int row = 0; row < 4; ++row) {
          ImTable::add(ROW_LABELS[row]);
          for(int col = 0; col < 6; ++col) {
            int i = ROWS[row][col];
            if(i < 0)break;
            if(col != 0)ImGui::SameLine();
            bool on = (a.binds & InputMap::bit(i)) != 0;
            if(ImGui::Checkbox(InputMap::BINDS[i].label, &on)) {
              a.binds = on ? (a.binds | InputMap::bit(i)) : (a.binds & ~InputMap::bit(i));
            }
          }
        }
        if(a.binds & InputMap::BIND_STICK) {
          ImTable::add("Stick Threshold");
          ImGui::SliderInt("##thr", &a.threshold, 1, 90);
          ImGui::TextDisabled("How far (raw units, edge is about 85) the stick must be pushed to count");
        }
      } else {
        ImTable::add("Sources");
        maskCheckBox("Analog Stick", a.binds, InputMap::BIND_STICK);
        ImGui::SameLine();
        maskCheckBox("D-Pad", a.binds, InputMap::BIND_DPAD);
        ImGui::SameLine();
        maskCheckBox("C-Buttons", a.binds, InputMap::BIND_CBTN);

        if(a.binds & InputMap::BIND_STICK) {
          ImTable::add("Deadzone");
          ImGui::SliderInt("##dz", &a.deadzone, 0, 40);
          ImTable::add("Full At");
          ImGui::SliderInt("##range", &a.range, std::max(a.deadzone + 1, 20), 127);
          ImTable::add("Curve");
          ImGui::SliderFloat("##curve", &a.curve, 0.5f, 3.0f, "%.2f");
          ImTable::add("");
          drawStickCurve(a);
        }
      }

      ImTable::add("");
      if(ImGui::Button(ICON_MDI_DELETE " Remove Action"))remove = true;

      ImTable::end();
      ImGui::TreePop();
    }
    ImGui::PopID();
  }

  void drawInput()
  {
    auto &conf = ctx.project->conf.input;
    ImGui::TextWrapped(ICON_MDI_INFORMATION_OUTLINE
      " Name your controls here, then read them in code with Input::held(Action::name),"
      " Input::pressed(...) and Input::stick(...), or with the Input nodes in node graphs."
      " Include \"p64/inputMap.h\" in scripts. Actions update on the next build.");

    auto err = InputMap::validate(conf);
    if(!err.empty()) {
      ImGui::TextColored({1.0f, 0.3f, 0.3f, 1.0f}, ICON_MDI_ALERT " %s", err.c_str());
    }

    int removeIdx = -1;
    for(size_t i = 0; i < conf.actions.size(); ++i) {
      bool remove = false;
      drawInputAction(conf.actions[i], (int)i, remove);
      if(remove)removeIdx = (int)i;
    }
    if(removeIdx >= 0)conf.actions.erase(conf.actions.begin() + removeIdx);

    bool full = conf.actions.size() >= InputMap::MAX_ACTIONS;
    if(full)ImGui::BeginDisabled();
    if(ImGui::Button(ICON_MDI_PLUS " Button Action")) {
      InputMap::Action a{};
      a.name = InputMap::makeName("action", conf);
      conf.actions.push_back(a);
    }
    ImGui::SameLine();
    if(ImGui::Button(ICON_MDI_PLUS " Stick Action")) {
      InputMap::Action a{};
      a.name = InputMap::makeName("stick", conf);
      a.type = InputMap::Type::STICK;
      a.binds = InputMap::BIND_STICK;
      conf.actions.push_back(a);
    }
    if(full)ImGui::EndDisabled();

    if(conf.actions.empty()) {
      ImGui::SameLine();
      if(ImGui::Button(ICON_MDI_GAMEPAD_VARIANT_OUTLINE " Add Common Actions")) {
        conf.actions = InputMap::defaultActions();
      }
    }
  }
}

bool Editor::ProjectSettings::draw()
{
  ImGui::BeginChild("TOP", ImVec2(0, -26_px));

  if (ImGui::CollapsingHeader("General", ImGuiTreeNodeFlags_DefaultOpen)) {
    ImTable::start("General");
    ImTable::add("Name", ctx.project->conf.name);
    ImTable::add("ROM-Name", ctx.project->conf.romName);
    ImTable::addCheckBox("Debug Menu (L + D-Up)", ctx.project->conf.debugMenu);
    ImTable::end();

    if(!ctx.project->conf.debugMenu) {
      ImGui::TextWrapped(ICON_MDI_INFORMATION_OUTLINE
        " The in-game debug menu hotkey (L + D-Up) is disabled.\n"
        "Call P64::Debug::Overlay::toggle() from your own code to open it.");
    }
  }

  if (ImGui::CollapsingHeader("Collision", ImGuiTreeNodeFlags_DefaultOpen))
  {
    ImTable::start("Collision");

    ImTable::add("Layer Names");
    for(int i=0; i<8; ++i) {
      ImTable::add("Layer " + std::to_string(i));
      ImGui::InputText(("##" + std::to_string(i)).c_str(), &ctx.project->conf.collLayerNames[i]);
    }
    ImTable::end();
  }

  if (ImGui::CollapsingHeader("Visibility", ImGuiTreeNodeFlags_DefaultOpen))
  {
    ImTable::start("Visibility");

    ImTable::add("Layer Names");
    for(int i=0; i<8; ++i) {
      ImTable::add("Layer " + std::to_string(i));
      ImGui::InputText(("##vis" + std::to_string(i)).c_str(), &ctx.project->conf.visLayerNames[i]);
    }
    ImTable::end();
  }

  if (ImGui::CollapsingHeader("Input")) {
    drawInput();
  }

  if (ImGui::CollapsingHeader("ROM Header")) {
    drawRomHeader();
  }

  if (ImGui::CollapsingHeader("Metadata")) {
    drawMetadata();
  }

  if (ImGui::CollapsingHeader("Assets", ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::TextWrapped(ICON_MDI_INFORMATION_OUTLINE
      " New and changed files are picked up automatically. A full rescan reloads every"
      " asset and script from disk, use it if something looks out of date.");

    bool assetsDirty = ctx.project->getAssets().isDirty();
    if(assetsDirty)ImGui::BeginDisabled();
    if(ImGui::Button(ICON_MDI_REFRESH " Rescan All Assets")) {
      Editor::Actions::call(Editor::Actions::Type::ASSETS_RELOAD);
      Utils::Logger::log("Full asset rescan done");
    }
    if(assetsDirty) {
      ImGui::EndDisabled();
      ImGui::SameLine();
      ImGui::TextColored({1.0f, 0.7f, 0.3f, 1.0f}, ICON_MDI_ALERT " Save first, a rescan discards unsaved asset changes");
    }
  }

  if (ImGui::CollapsingHeader("Environment", ImGuiTreeNodeFlags_DefaultOpen)) {
    ImTable::start("Environment");
    ImTable::addPath("Emulator", ctx.project->conf.pathEmu);
    ImTable::addPath("N64_INST", ctx.project->conf.pathN64Inst, true, "$N64_INST");
    ImTable::end();
  }

  ImGui::EndChild();

  bool hasError = metadataHasError();

  ImGui::BeginChild("BOTTOM", ImVec2(0, 24_px));
    if(hasError) {
      ImGui::TextColored({1.0f, 0.3f, 0.3f, 1.0f}, ICON_MDI_ALERT " Fix invalid metadata images before saving");
      ImGui::SameLine();
    }
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 64_px);

    if(hasError) ImGui::BeginDisabled();
    bool res = ImGui::Button("Save", ImVec2(60_px, 0));
    if(hasError) ImGui::EndDisabled();
  ImGui::EndChild();

  if (res) {
    ctx.project->save();
  }
  return res;
}
