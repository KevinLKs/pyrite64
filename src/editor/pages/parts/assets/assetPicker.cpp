/**
* @copyright 2026 - Max Bebök
* @license MIT
*/
#include "assetPicker.h"

#include "../../../../context.h"
#include "../../../imgui/helper.h"
#include "../../../undoRedo.h"
#include "../../../thumbnailCache.h"
#include "IconsMaterialDesignIcons.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace
{
  const std::string ROOT_DIR = "assets";

  // "assets/a/b/x.png" -> "assets/a/b"
  std::string parentDir(const std::string &projectPath)
  {
    auto pos = projectPath.find_last_of('/');
    return pos == std::string::npos ? std::string{} : projectPath.substr(0, pos);
  }

  bool isInside(const std::string &dir, const std::string &root)
  {
    return dir == root || (dir.size() > root.size() && dir.starts_with(root) && dir[root.size()] == '/');
  }

  bool containsNoCase(const std::string &text, const std::string &needle)
  {
    if(needle.empty())return true;
    auto it = std::search(text.begin(), text.end(), needle.begin(), needle.end(), [](char a, char b) {
      return std::tolower((unsigned char)a) == std::tolower((unsigned char)b);
    });
    return it != text.end();
  }

  // Shortens a label with "..." so it fits into maxWidth.
  std::string fitText(const std::string &text, float maxWidth)
  {
    if(ImGui::CalcTextSize(text.c_str()).x <= maxWidth)return text;
    std::string res = text;
    while(!res.empty() && ImGui::CalcTextSize((res + "...").c_str()).x > maxWidth)res.pop_back();
    return res + "...";
  }

  const char* typeNoun(Project::FileType type)
  {
    switch(type) {
      case Project::FileType::IMAGE:    return "textures";
      case Project::FileType::MODEL_3D: return "models";
      default:                          return "assets";
    }
  }

  struct PickerState
  {
    std::string dir{};
    std::string homeDir{};
    std::string lastDir{}; // folder the last asset of this type was picked from
    std::string search{};
    bool recursive{true};
  };

  // One state per asset type, so textures and models remember their own folders.
  PickerState& getState(Project::FileType type)
  {
    static std::map<Project::FileType, PickerState> states{};
    return states[type];
  }

  // Draws the preview of one asset into the square [p0, p0+tile].
  void drawThumb(Project::AssetManager &assetManager, const Project::AssetManagerEntry &asset,
    ImDrawList *dl, ImVec2 p0, float tile)
  {
    if(asset.type == Project::FileType::IMAGE)
    {
      // fork: textures load on first display
      if(!asset.loaded) {
        assetManager.ensureTexture(const_cast<Project::AssetManagerEntry&>(asset));
      }
      if(asset.texture && asset.texture->getWidth() > 0) {
        float w = (float)asset.texture->getWidth();
        float h = (float)asset.texture->getHeight();
        float scale = tile / std::max(w, h);
        ImVec2 size{w * scale, h * scale};
        ImVec2 imgP0{p0.x + (tile - size.x) * 0.5f, p0.y + (tile - size.y) * 0.5f};
        dl->AddImage(ImTextureRef(asset.texture->getGPUTex()), imgP0, ImVec2(imgP0.x + size.x, imgP0.y + size.y));
      }
      return;
    }

    if(asset.type == Project::FileType::MODEL_3D && ctx.thumbnails)
    {
      // rendered in the background, one angle per frame; hovering spins the turntable
      if(auto thumb = ctx.thumbnails->getModelTexture(asset.getUUID())) {
        ImVec2 p1{p0.x + tile, p0.y + tile};
        ImVec2 uv0{0,0}, uv1{1,1};
        ctx.thumbnails->getScrubUV(p0, p1, uv0, uv1);
        dl->AddImage(ImTextureRef(thumb), p0, p1, uv0, uv1);
        return;
      }
    }

    // fallback icon (also shown while a model thumbnail is still generating)
    const char *icon = asset.type == Project::FileType::MODEL_3D ? ICON_MDI_CUBE_OUTLINE : ICON_MDI_FILE_OUTLINE;
    float fontSize = tile * 0.5f;
    ImVec2 txtSize = ImGui::GetFont()->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, icon);
    dl->AddText(ImGui::GetFont(), fontSize,
      ImVec2(p0.x + (tile - txtSize.x) * 0.5f, p0.y + (tile - txtSize.y) * 0.5f),
      ImGui::GetColorU32(ImGuiCol_TextDisabled), icon);
  }
}

bool Editor::AssetPicker::draw(const char *label, uint64_t &uuid, Project::FileType type, const std::string &startDir)
{
  auto &state = getState(type);
  auto &assetManager = ctx.project->getAssets();
  const auto &entries = assetManager.getTypeEntries(type);
  const char *noun = typeNoun(type);

  const Project::AssetManagerEntry *current = nullptr;
  for(const auto &e : entries) {
    if(e.getUUID() == uuid) { current = &e; break; }
  }

  auto openPicker = [&](const char *popupId) {
    if(!startDir.empty())state.homeDir = startDir;
    else if(current)state.homeDir = parentDir(current->projectPath);
    else if(!state.lastDir.empty())state.homeDir = state.lastDir;
    else state.homeDir = ROOT_DIR;

    state.dir = state.homeDir;
    state.search.clear();
    ImGui::OpenPopup(popupId);
  };

  bool changed = false;
  bool locked = ImTable::isPrefabLocked();
  if(locked)ImGui::BeginDisabled();

  // Field: looks like a combo box, opens the picker
  std::string popupId = std::string{"##AssetPicker"} + label;
  std::string preview = current ? current->name : std::string{"<None>"};

  float arrowW = ImGui::GetFrameHeight();
  float fieldW = ImGui::CalcItemWidth() - arrowW;

  float widthBtn = arrowW + 1_px;
  if(uuid != 0)fieldW -= widthBtn;

  if(ImGui::Button((fitText(preview, fieldW - 12.0f) + "##" + label).c_str(), ImVec2(fieldW, 0))) {
    openPicker(popupId.c_str());
  }
  if(current && ImGui::IsItemHovered())ImGui::SetTooltip("%s", current->projectPath.c_str());

  // Assets dragged from the browser onto the field
  if(ImGui::HandleComboBoxDragDrop(uuid, [&entries](uint64_t dropUUID, const char *payloadType) {
    if(strcmp(payloadType, "ASSET") != 0)return false;
    return std::any_of(entries.begin(), entries.end(), [dropUUID](const auto &e) { return e.getUUID() == dropUUID; });
  })) {
    changed = true;
  }

  if(uuid != 0) {
    ImGui::SameLine(0, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {0, ImGui::GetStyle().FramePadding.y});
    if(ImGui::Button((std::string{ICON_MDI_WINDOW_CLOSE "##clear"} + label).c_str(), ImVec2(widthBtn, 0)))
    {
      uuid = 0;
      changed = true;
    }
    ImGui::PopStyleVar();
  }

  ImGui::SameLine(0, 0);
  if(ImGui::ArrowButton((std::string{"##arrow"} + label).c_str(), ImGuiDir_Down)) {
    openPicker(popupId.c_str());
  }
  if(locked)ImGui::EndDisabled();

  ImGui::SetNextWindowSize(ImVec2(460_px, 420_px), ImGuiCond_Appearing);
  if(ImGui::BeginPopup(popupId.c_str()))
  {
    // --- Navigation bar: up, home, breadcrumbs
    ImGui::BeginDisabled(state.dir == ROOT_DIR);
    if(ImGui::Button(ICON_MDI_ARROW_UP)) {
      state.dir = parentDir(state.dir);
      if(!isInside(state.dir, ROOT_DIR))state.dir = ROOT_DIR;
    }
    ImGui::EndDisabled();
    if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))ImGui::SetTooltip("Up one folder");

    ImGui::SameLine();
    ImGui::BeginDisabled(state.dir == state.homeDir);
    if(ImGui::Button(ICON_MDI_HOME)) state.dir = state.homeDir;
    ImGui::EndDisabled();
    if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      ImGui::SetTooltip("Back to %s", state.homeDir.c_str());
    }

    // breadcrumbs: every segment is a button that jumps there
    {
      std::string accum{};
      size_t start = 0;
      int seg = 0;
      while(start <= state.dir.size()) {
        auto end = state.dir.find('/', start);
        if(end == std::string::npos)end = state.dir.size();
        auto part = state.dir.substr(start, end - start);
        accum = accum.empty() ? part : accum + "/" + part;

        ImGui::SameLine(0, seg == 0 ? 8.0f : 2.0f);
        if(seg > 0) { ImGui::TextDisabled("/"); ImGui::SameLine(0, 2.0f); }
        ImGui::PushID(seg);
        if(ImGui::SmallButton(part.c_str()))state.dir = accum;
        ImGui::PopID();

        ++seg;
        if(end >= state.dir.size())break;
        start = end + 1;
      }
    }

    // --- Search + subfolder toggle
    if(ImGui::IsWindowAppearing())ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-160_px);
    std::string hint = std::string{ICON_MDI_MAGNIFY " Search "} + noun + "...";
    ImGui::InputTextWithHint("##search", hint.c_str(), &state.search);
    ImGui::SameLine();
    ImGui::Checkbox("Include subfolders", &state.recursive);
    ImGui::Separator();

    // --- Collect folders and assets for the current directory
    std::set<std::string> subDirs{};
    std::vector<const Project::AssetManagerEntry*> shown{};
    bool searching = !state.search.empty();
    for(const auto &e : entries)
    {
      auto dir = parentDir(e.projectPath);
      if(!isInside(dir, state.dir))continue;

      if(dir != state.dir) {
        // first path segment below the current folder
        auto rest = dir.substr(state.dir.size() + 1);
        subDirs.insert(rest.substr(0, rest.find('/')));
      }

      bool inScope = (dir == state.dir) || state.recursive || searching;
      if(!inScope)continue;
      if(searching && !containsNoCase(e.name, state.search))continue;
      shown.push_back(&e);
    }
    std::sort(shown.begin(), shown.end(), [](auto *a, auto *b) { return a->projectPath < b->projectPath; });

    ImGui::BeginChild("##content", ImVec2(0, 0));

    // folders
    if(!searching) {
      for(const auto &sub : subDirs) {
        if(ImGui::Selectable((std::string{ICON_MDI_FOLDER " "} + sub).c_str())) {
          state.dir += "/" + sub;
        }
      }
      if(!subDirs.empty() && !shown.empty())ImGui::Separator();
    }

    if(shown.empty()) {
      ImGui::TextDisabled(searching ? "No %s match." : "No %s here.", noun);
    }

    // thumbnail grid, only visible rows are drawn (so only those load / render thumbnails)
    const float tile = 72_px;
    const float labelH = ImGui::GetTextLineHeight();
    const float cellW = tile + 8_px;
    const float cellH = tile + labelH + 10_px;
    int cols = std::max(1, (int)(ImGui::GetContentRegionAvail().x / cellW));
    int rows = ((int)shown.size() + cols - 1) / cols;

    ImGuiListClipper clipper;
    clipper.Begin(rows, cellH);
    while(clipper.Step())
    {
      for(int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
      {
        ImVec2 rowPos = ImGui::GetCursorScreenPos();
        for(int col = 0; col < cols; ++col)
        {
          int idx = row * cols + col;
          if(idx >= (int)shown.size())break;
          const auto *asset = shown[idx];

          ImVec2 p0{rowPos.x + col * cellW, rowPos.y};
          ImGui::SetCursorScreenPos(p0);
          ImGui::PushID(idx);
          bool clicked = ImGui::InvisibleButton("##tile", ImVec2(cellW - 4_px, cellH - 4_px));
          bool hovered = ImGui::IsItemHovered();
          ImGui::PopID();

          auto *dl = ImGui::GetWindowDrawList();
          bool isSel = asset->getUUID() == uuid;
          if(isSel || hovered) {
            dl->AddRectFilled(p0, ImVec2(p0.x + cellW - 4_px, p0.y + cellH - 4_px),
              ImGui::GetColorU32(isSel ? ImGuiCol_HeaderActive : ImGuiCol_HeaderHovered), 4.0f);
          }

          drawThumb(assetManager, *asset, dl, ImVec2(p0.x + 2_px, p0.y + 2_px), tile);

          // name without extension, shortened to fit
          std::string name = asset->name;
          if(auto dot = name.find('.'); dot != std::string::npos)name = name.substr(0, dot);
          name = fitText(name, cellW - 8_px);
          dl->AddText(ImVec2(p0.x + 2_px, p0.y + tile + 4_px), ImGui::GetColorU32(ImGuiCol_Text), name.c_str());

          if(hovered) {
            if(asset->type == Project::FileType::IMAGE && asset->texture) {
              ImGui::SetTooltip("%s\n%dx%d", asset->projectPath.c_str(),
                asset->texture->getWidth(), asset->texture->getHeight());
            } else {
              ImGui::SetTooltip("%s", asset->projectPath.c_str());
            }
          }
          if(clicked) {
            if(uuid != asset->getUUID()) {
              uuid = asset->getUUID();
              changed = true;
            }
            state.lastDir = parentDir(asset->projectPath);
            ImGui::CloseCurrentPopup();
          }
        }
        // a real item for the row, so the child knows its size (no SetCursorPos-only extent)
        ImGui::SetCursorScreenPos(rowPos);
        ImGui::Dummy(ImVec2(cols * cellW, cellH));
      }
    }
    clipper.End();

    ImGui::EndChild();
    ImGui::EndPopup();
  }

  if(changed) {
    Editor::UndoRedo::getHistory().markChanged(std::string{"Edit "} + label);
  }
  return changed;
}
