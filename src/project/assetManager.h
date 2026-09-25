/**
* @copyright 2025 - Max Bebök
* @license MIT
*/
#pragma once
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <chrono>
#include <memory>

#include "../renderer/n64Mesh.h"
#include "../renderer/object.h"
#include "../utils/codeParser.h"
#include "../renderer/texture.h"
#include "assets/model3d.h"
#include "scene/prefab.h"
#include "tiny3d/tools/gltf_importer/src/structs.h"

namespace Utils { class FileWatcher; }

namespace Project
{
  class Project;

  enum class ComprTypes : int
  {
    DEFAULT = 0,
    LEVEL_0,
    LEVEL_1,
    LEVEL_2,
    LEVEL_3,
  };

  enum class FileType : int
  {
    UNKNOWN = 0,
    IMAGE,
    AUDIO,
    FONT,
    MODEL_3D,
    CODE_OBJ,
    CODE_GLOBAL,
    PREFAB,
    NODE_GRAPH,
    MUSIC_XM,

    _SIZE
  };

  struct AssetConf
  {
    uint64_t uuid{0};
    int format{0};
    // legacy import scale for models (kept for scene migration), point size for fonts
    int baseScale{0};
    // optional manual override of the auto-computed model import scale (0 = auto)
    int baseScaleOverride{0};
    bool gltfBVH{0};

    ComprTypes compression{ComprTypes::DEFAULT};
    bool exclude{false};

    PROP_BOOL(wavForceMono);
    PROP_U32(wavResampleRate);
    PROP_S32(wavCompression);

    PROP_U32(fontId);
    PROP_STRING(fontCharset);

    // extra arbitrary data assets can store
    nlohmann::json data{};

    std::string serialize() const;
  };

  struct AssetManagerEntry
  {
    std::string name{};
    std::string path{};
    // path relative to the project root, unix separators (e.g. "assets/img/x.png")
    std::string projectPath{};
    std::string outPath{};
    std::string romPath{};
    FileType type{};
    std::shared_ptr<Renderer::Texture> texture{nullptr};
    Assets::Model3D model{};
    std::shared_ptr<Renderer::N64Mesh> mesh3D{};
    std::shared_ptr<Prefab> prefab{nullptr};
    AssetConf conf{};
    Utils::CPP::Struct params{};
    // Heavy data (currently: 3D models) is loaded on first access in the editor, see AssetManager::getEntryByUUID.
    bool loaded{false};
    // model.autoBaseScale holds the real value (set by a full load or getModelScale)
    bool scaleKnown{false};

    uint64_t getUUID() const { return conf.uuid; }

    // imgui selectbox:
    uint64_t getId() const { return conf.uuid; }
    const std::string &getName() const { return name; }
  };

  class AssetManager
  {
    private:
      Project *project;
      std::array<std::vector<AssetManagerEntry>, static_cast<size_t>(FileType::_SIZE)> entries{};

      // Every file known under assets/ and src/user/ (key: pathKey()) with its last-write time.
      // Only used to diff against the disk on a full sync (watcher overflow, polling fallback).
      std::unordered_map<std::string, uint64_t> watchFiles{};
      std::chrono::steady_clock::time_point watchLastCheck{};
      bool watchInitialized{false};
      std::unique_ptr<Utils::FileWatcher> watcher{};

      // Editor mode: the file watcher runs and image previews are prepared.
      // Off for builds, which must not spawn watcher threads and do not draw anything.
      // Models load on first use in both modes (see getEntryByUUID / getModelScale).
      bool editorMode{false};

      // pathKey() -> {type, index}, rebuilt together with entriesMap
      std::unordered_map<std::string, std::pair<int, int>> pathIndex{};

      std::unordered_set<uint64_t> dirtyPrefabs{};
      std::unordered_set<uint64_t> dirtyAssetMeta{};
      std::unordered_set<uint64_t> dirtyNodeGraphs{};
      std::unordered_map<uint64_t, std::string> savedPrefabState{};
      std::unordered_map<uint64_t, std::string> savedAssetMetaState{};
      std::unordered_map<uint64_t, std::string> savedNodeGraphState{};
      std::unordered_map<uint64_t, std::string> dirtyNodeGraphState{};

      std::string defaultObjScript{};
      std::string defaultGlobalScript{};
      std::shared_ptr<Renderer::Texture> fallbackTex{};

      void reloadEntry(AssetManagerEntry &entry, const std::string &path);
      void ensureLoaded(AssetManagerEntry &entry);
      void resetDirtyTracking();
      void clearDirtyTracking(uint64_t uuid);
      void rebuildIndex();
      void startWatcher();

      // Adds/refreshes (upserts) and removes the given files, touching nothing else.
      bool applyChanges(const std::vector<std::string> &upserts, const std::vector<std::string> &removes);
      // Walks all watched folders once and applies whatever differs from watchFiles.
      bool syncWithDisk();
    public:
      std::unordered_map<uint64_t, std::pair<int, int>> entriesMap{};
      //std::unordered_map<uint64_t, int> entriesMapScript{};

      explicit AssetManager(Project *pr);
      ~AssetManager();

      // Normalized lookup key for a file path (separators and "."/".." normalized).
      static std::string pathKey(const std::string &path);

      void setEditorMode(bool enabled) { editorMode = enabled; }

      // Import scale of a model. Cheap: reads only the glTF header data, the model is not loaded.
      float getModelScale(uint64_t uuid);
      void reload();
      void reloadAssetByUUID(uint64_t uuid);
      bool pollWatch();
      bool isDirty() const {
        return !dirtyPrefabs.empty() || !dirtyAssetMeta.empty() || !dirtyNodeGraphs.empty();
      }
      bool isNodeGraphDirty(uint64_t uuid) const {
        return dirtyNodeGraphs.contains(uuid);
      }

      [[nodiscard]] const auto& getEntries() const {
        return entries;
      }
      [[nodiscard]] const std::vector<AssetManagerEntry>& getTypeEntries(FileType type) const {
        return entries[static_cast<int>(type)];
      }

      AssetManagerEntry* getByName(const std::string &name) {
        for (auto &typed : entries) {
          for (auto &entry : typed) {
            if (entry.name == name) {
              return &entry;
            }
          }
        }
        return nullptr;
      }

      AssetManagerEntry* getByPath(const std::string &path);

      // Loads the entry's heavy data (models) on first access if it was deferred.
      AssetManagerEntry* getEntryByUUID(uint64_t uuid) {
        auto it = entriesMap.find(uuid);
        if (it == entriesMap.end()) {
          return nullptr;
        }
        auto &entry = entries[it->second.first][it->second.second];
        if (!entry.loaded && entry.type == FileType::MODEL_3D) {
          ensureLoaded(entry);
        }
        return &entry;
      }

      std::shared_ptr<Prefab> getPrefabByUUID(uint64_t uuid) {
        auto entry = getEntryByUUID(uuid);
        if (!entry || entry->type != FileType::PREFAB) {
          return nullptr;
        }
        return entry->prefab;
      }

      const std::shared_ptr<Renderer::Texture> &getFallbackTexture();

      void markPrefabDirty(uint64_t uuid);
      void markAssetMetaDirty(uint64_t uuid);
      void markNodeGraphDirty(uint64_t uuid, const std::string &currentState);
      void markNodeGraphSaved(uint64_t uuid, const std::string &savedState);
      void clearNodeGraphDirty(uint64_t uuid);

      void save();

      bool createScript(const std::string &name, bool isGlobal, const std::string &subDir = {});
      uint64_t createNodeGraph(const std::string &name);
  };
}
