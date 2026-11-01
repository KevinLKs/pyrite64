/**
* @license MIT
*
* Extra editor-script API used by the MCP bridge (and usable from any editor script):
*   scene.list() / scene.current() / scene.objects(rootUUID?, maxDepth?) / scene.find(text)
*   scene.get(uuid) / scene.create(name, parentUUID?) / scene.addModel(assetUUID)
*   scene.addPrefab(assetUUID) / scene.addJSON(json, parentUUID?) / scene.serialize(uuid)
*   scene.remove(uuid) / scene.move(uuid, targetUUID, asChild?) / scene.select(uuids)
*   scene.setEnabled(uuid, bool) / scene.addComponent(uuid, idOrName) / scene.removeComponent(uuid, compUUID)
*   scene.newScene(name?, open = true) / scene.rename(name) or rename(id, name)
*   scene.duplicate(id, name?) / scene.deleteScene(id, force = false)
*   scene.config() / scene.setConfig(patch)
*   scene.setProps(uuid, {name, enabled, selectable, visMask, proportionalScale})
*   scene.getComponent(uuid, compUUID) / scene.setComponent(uuid, compUUID, {name?, enabled?, data?})
*   scene.createPrefab(uuid, subDir?) / scene.unpackPrefab(uuid)
*   project.config() / project.setConfig(patch)
*   assets.list(type?) / assets.get(uuid) / assets.config(uuid) / assets.setConfig(uuid, patch)
*   assets.createScript(name, isGlobal = false, subDir?) / assets.createNodeGraph(name)
*   components.list()
*   console.read(maxChars?)
*   editor.isBuilding()
*
* Asset UUIDs are 64-bit and returned as strings (JS numbers can't hold them),
* any function taking an asset UUID accepts a string or a number.
*/
#include "impl.h"
#include "../scriptUtils.h"
#include "../../../context.h"
#include "../../../project/scene/object.h"
#include "../../../project/scene/scene.h"
#include "../../../project/assetManager.h"
#include "../../../project/component/components.h"
#include "../../../utils/logger.h"
#include "../../../utils/fs.h"
#include "../../../project/project.h"
#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

using namespace Editor::ScriptHost;
using Json = nlohmann::json;

namespace
{
  Project::Scene* getScene(JSContext *c)
  {
    auto *scene = ctx.project ? ctx.project->getScenes().getLoadedScene() : nullptr;
    if(!scene)JS_ThrowReferenceError(c, "no scene loaded");
    return scene;
  }

  bool readU32(JSContext *c, JSValueConst v, uint32_t &out)
  {
    if(JS_IsString(v)) {
      out = (uint32_t)std::strtoul(toString(v, c).c_str(), nullptr, 10);
      return true;
    }
    return JS_ToUint32(c, &out, v) == 0;
  }

  bool readU64(JSContext *c, JSValueConst v, uint64_t &out)
  {
    if(JS_IsString(v)) {
      out = std::strtoull(toString(v, c).c_str(), nullptr, 10);
      return true;
    }
    if(JS_IsBigInt(v)) {
      int64_t tmp{};
      if(JS_ToBigInt64(c, &tmp, v) != 0)return false;
      out = (uint64_t)tmp;
      return true;
    }
    double d{};
    if(JS_ToFloat64(c, &d, v) != 0)return false;
    out = (uint64_t)d;
    return true;
  }

  std::string lower(std::string s)
  {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch){ return (char)std::tolower(ch); });
    return s;
  }

  JSValue fromJson(JSContext *c, const Json &j)
  {
    auto str = j.dump(-1, ' ', false, Json::error_handler_t::replace);
    return JS_ParseJSON(c, str.c_str(), str.size(), "<json>");
  }

  const char* fileTypeName(Project::FileType t)
  {
    using FT = Project::FileType;
    switch(t) {
      case FT::IMAGE: return "image";
      case FT::AUDIO: return "audio";
      case FT::FONT: return "font";
      case FT::MODEL_3D: return "model";
      case FT::CODE_OBJ: return "code_object";
      case FT::CODE_GLOBAL: return "code_global";
      case FT::PREFAB: return "prefab";
      case FT::NODE_GRAPH: return "node_graph";
      case FT::MUSIC_XM: return "music";
      case FT::EDITOR_SCRIPT: return "editor_script";
      default: return "unknown";
    }
  }

  const char* compName(int id)
  {
    for(auto &info : Project::Component::TABLE) {
      if(info.id == id)return info.name;
    }
    return "?";
  }

  // returns -1 if not found
  int findCompId(JSContext *c, JSValueConst v)
  {
    if(JS_IsNumber(v)) {
      int32_t id{};
      JS_ToInt32(c, &id, v);
      for(auto &info : Project::Component::TABLE) if(info.id == id)return id;
      return -1;
    }
    auto name = lower(toString(v, c));
    for(auto &info : Project::Component::TABLE) {
      if(lower(info.name) == name)return info.id;
    }
    // loose match, e.g. "animated" -> "Model (Animated)"
    for(auto &info : Project::Component::TABLE) {
      if(lower(info.name).find(name) != std::string::npos)return info.id;
    }
    return -1;
  }

  Json objSummary(Project::Object &obj, int depth, int maxDepth)
  {
    auto t = obj.getAuthoredTrans();
    Json j{
      {"uuid", obj.uuid},
      {"name", obj.name},
      {"enabled", obj.enabled},
      {"pos", {t.pos.x, t.pos.y, t.pos.z}},
      {"rot", {t.rot.x, t.rot.y, t.rot.z, t.rot.w}},
      {"scale", {t.scale.x, t.scale.y, t.scale.z}},
    };
    if(obj.isPrefabInstance())j["prefab"] = std::to_string(obj.uuidPrefab.value);

    Json comps = Json::array();
    for(auto &comp : obj.components) {
      comps.push_back(Json{{"uuid", std::to_string(comp.uuid)}, {"type", compName(comp.id)}, {"name", comp.name}});
    }
    j["components"] = comps;

    if(!obj.children.empty()) {
      if(maxDepth >= 0 && depth >= maxDepth) {
        j["childCount"] = obj.children.size();
      } else {
        Json kids = Json::array();
        for(auto &child : obj.children)kids.push_back(objSummary(*child, depth + 1, maxDepth));
        j["children"] = kids;
      }
    }
    return j;
  }

  Json assetSummary(const Project::AssetManagerEntry &e)
  {
    return Json{
      {"uuid", std::to_string(e.getUUID())},
      {"name", e.name},
      {"path", e.projectPath},
      {"type", fileTypeName(e.type)},
    };
  }

  // removes the object and all its children from the scene's lookup map
  void forgetRecursive(Project::Scene &scene, Project::Object &obj)
  {
    for(auto &child : obj.children)forgetRecursive(scene, *child);
    scene.objectsMap.erase(obj.uuid);
  }

  Json sceneEntryJson(int id, const std::string &fileName); // scene management helpers below

  //########// scene //########//

  JSValue sceneList(JSContext *c, JSValueConst, int, JSValueConst*)
  {
    if(!ctx.project)return JS_ThrowReferenceError(c, "no project loaded");
    Json arr = Json::array();
    for(auto &e : ctx.project->getScenes().getEntries())arr.push_back(sceneEntryJson(e.id, e.name));
    return fromJson(c, arr);
  }

  JSValue sceneCurrent(JSContext *c, JSValueConst, int, JSValueConst*)
  {
    auto *scene = ctx.project ? ctx.project->getScenes().getLoadedScene() : nullptr;
    if(!scene)return JS_NULL;
    return fromJson(c, Json{
      {"id", scene->getId()},
      {"name", scene->getName()},
      {"objectCount", scene->objectsMap.size()},
    });
  }

  JSValue sceneObjects(JSContext *c, JSValueConst, int argc, JSValueConst *argv)
  {
    auto *scene = getScene(c);
    if(!scene)return JS_EXCEPTION;

    uint32_t rootUUID = 0;
    int32_t maxDepth = -1;
    if(argc > 0 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0]) && !readU32(c, argv[0], rootUUID))return JS_EXCEPTION;
    if(argc > 1 && !JS_IsUndefined(argv[1]) && JS_ToInt32(c, &maxDepth, argv[1]) != 0)return JS_EXCEPTION;

    if(rootUUID) {
      auto obj = scene->getObjectByUUID(rootUUID);
      if(!obj)return JS_ThrowReferenceError(c, "object %u not found", rootUUID);
      return fromJson(c, objSummary(*obj, 0, maxDepth));
    }

    Json arr = Json::array();
    for(auto &child : scene->getRootObject().children) {
      arr.push_back(objSummary(*child, 0, maxDepth));
    }
    return fromJson(c, arr);
  }

  JSValue sceneFind(JSContext *c, JSValueConst, int argc, JSValueConst *argv)
  {
    auto *scene = getScene(c);
    if(!scene)return JS_EXCEPTION;
    auto needle = argc > 0 ? lower(toString(argv[0], c)) : std::string{};

    JSValue arr = JS_NewArray(c);
    uint32_t idx = 0;
    for(auto &[uuid, obj] : scene->objectsMap) {
      if(!obj)continue;
      if(needle.empty() || lower(obj->name).find(needle) != std::string::npos) {
        JS_SetPropertyUint32(c, arr, idx++, Impl::wrapObject(c, uuid));
      }
    }
    return arr;
  }

  JSValue sceneGet(JSContext *c, JSValueConst, int argc, JSValueConst *argv)
  {
    auto *scene = getScene(c);
    if(!scene)return JS_EXCEPTION;
    uint32_t uuid{};
    if(argc < 1 || !readU32(c, argv[0], uuid))return JS_ThrowTypeError(c, "get expects (uuid)");
    if(!scene->getObjectByUUID(uuid))return JS_NULL;
    return Impl::wrapObject(c, uuid);
  }

  JSValue sceneCreate(JSContext *c, JSValueConst, int argc, JSValueConst *argv)
  {
    auto *scene = getScene(c);
    if(!scene)return JS_EXCEPTION;

    Project::Object *parent = &scene->getRootObject();
    uint32_t parentUUID = 0;
    if(argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
      if(!readU32(c, argv[1], parentUUID))return JS_EXCEPTION;
      if(parentUUID) {
        auto p = scene->getObjectByUUID(parentUUID);
        if(!p)return JS_ThrowReferenceError(c, "parent %u not found", parentUUID);
        parent = p.get();
      }
    }

    auto obj = scene->addObject(*parent);
    if(argc > 0 && JS_IsString(argv[0]))obj->name = toString(argv[0], c);
    return Impl::wrapObject(c, obj->uuid);
  }

  JSValue sceneAddModel(JSContext *c, JSValueConst, int argc, JSValueConst *argv)
  {
    auto *scene = getScene(c);
    if(!scene)return JS_EXCEPTION;
    uint64_t assetUUID{};
    if(argc < 1 || !readU64(c, argv[0], assetUUID))return JS_ThrowTypeError(c, "addModel expects (assetUUID)");
    auto obj = scene->addModelObject(assetUUID);
    if(!obj)return JS_ThrowReferenceError(c, "asset is not a 3D model");
    return Impl::wrapObject(c, obj->uuid);
  }

  JSValue sceneAddPrefab(JSContext *c, JSValueConst, int argc, JSValueConst *argv)
  {
    auto *scene = getScene(c);
    if(!scene)return JS_EXCEPTION;
    uint64_t assetUUID{};
    if(argc < 1 || !readU64(c, argv[0], assetUUID))return JS_ThrowTypeError(c, "addPrefab expects (assetUUID)");
    auto obj = scene->addPrefabInstance(assetUUID);
    if(!obj)return JS_ThrowReferenceError(c, "asset is not a prefab");
    return Impl::wrapObject(c, obj->uuid);
  }

  JSValue sceneAddJSON(JSContext *c, JSValueConst, int argc, JSValueConst *argv)
  {
    auto *scene = getScene(c);
    if(!scene)return JS_EXCEPTION;
    if(argc < 1)return JS_ThrowTypeError(c, "addJSON expects (json, parentUUID?)");

    std::string text{};
    if(JS_IsString(argv[0])) {
      text = toString(argv[0], c);
    } else {
      JSValue str = JS_JSONStringify(c, argv[0], JS_UNDEFINED, JS_UNDEFINED);
      if(JS_IsException(str))return str;
      text = toString(str, c);
      JS_FreeValue(c, str);
    }
    auto parsed = Json::parse(text, nullptr, false);
    if(parsed.is_discarded() || !parsed.is_object())return JS_ThrowTypeError(c, "invalid object JSON");

    uint32_t parentUUID = 0;
    if(argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1]) && !readU32(c, argv[1], parentUUID))return JS_EXCEPTION;
    if(parentUUID && !scene->getObjectByUUID(parentUUID))return JS_ThrowReferenceError(c, "parent %u not found", parentUUID);

    auto obj = scene->addObject(text, parentUUID);
    return Impl::wrapObject(c, obj->uuid);
  }

  JSValue sceneSerialize(JSContext *c, JSValueConst, int argc, JSValueConst *argv)
  {
    auto *scene = getScene(c);
    if(!scene)return JS_EXCEPTION;
    uint32_t uuid{};
    if(argc < 1 || !readU32(c, argv[0], uuid))return JS_ThrowTypeError(c, "serialize expects (uuid)");
    auto obj = scene->getObjectByUUID(uuid);
    if(!obj)return JS_ThrowReferenceError(c, "object %u not found", uuid);
    return fromJson(c, obj->serialize());
  }

  JSValue sceneRemove(JSContext *c, JSValueConst, int argc, JSValueConst *argv)
  {
    auto *scene = getScene(c);
    if(!scene)return JS_EXCEPTION;
    uint32_t uuid{};
    if(argc < 1 || !readU32(c, argv[0], uuid))return JS_ThrowTypeError(c, "remove expects (uuid)");
    auto obj = scene->getObjectByUUID(uuid);
    if(!obj || !obj->parent)return JS_FALSE;

    for(auto &child : obj->children)forgetRecursive(*scene, *child);
    scene->removeObject(*obj);
    ctx.sanitizeObjectSelection(scene);
    return JS_TRUE;
  }

  JSValue sceneMove(JSContext *c, JSValueConst, int argc, JSValueConst *argv)
  {
    auto *scene = getScene(c);
    if(!scene)return JS_EXCEPTION;
    uint32_t uuid{}, target{};
    if(argc < 2 || !readU32(c, argv[0], uuid) || !readU32(c, argv[1], target)) {
      return JS_ThrowTypeError(c, "move expects (uuid, targetUUID, asChild = true)");
    }
    bool asChild = argc > 2 ? JS_ToBool(c, argv[2]) : true;
    // the scene root has uuid 0
    if(target == 0)target = scene->getRootObject().uuid;
    return JS_NewBool(c, scene->moveObject(uuid, target, asChild));
  }

  JSValue sceneSelect(JSContext *c, JSValueConst, int argc, JSValueConst *argv)
  {
    auto *scene = getScene(c);
    if(!scene)return JS_EXCEPTION;
    std::vector<uint32_t> uuids{};
    if(argc > 0 && JS_IsArray(argv[0])) {
      int64_t len{};
      JS_GetLength(c, argv[0], &len);
      for(int64_t i = 0; i < len; ++i) {
        JSValue v = JS_GetPropertyInt64(c, argv[0], i);
        uint32_t uuid{};
        if(readU32(c, v, uuid) && scene->getObjectByUUID(uuid))uuids.push_back(uuid);
        JS_FreeValue(c, v);
      }
    } else if(argc > 0) {
      uint32_t uuid{};
      if(readU32(c, argv[0], uuid) && scene->getObjectByUUID(uuid))uuids.push_back(uuid);
    }
    ctx.setObjectSelectionList(uuids, uuids.empty() ? 0 : uuids.back());
    return JS_NewUint32(c, (uint32_t)uuids.size());
  }

  JSValue sceneSetEnabled(JSContext *c, JSValueConst, int argc, JSValueConst *argv)
  {
    auto *scene = getScene(c);
    if(!scene)return JS_EXCEPTION;
    uint32_t uuid{};
    if(argc < 2 || !readU32(c, argv[0], uuid))return JS_ThrowTypeError(c, "setEnabled expects (uuid, bool)");
    auto obj = scene->getObjectByUUID(uuid);
    if(!obj)return JS_ThrowReferenceError(c, "object %u not found", uuid);
    obj->enabled = JS_ToBool(c, argv[1]);
    return JS_UNDEFINED;
  }

  JSValue sceneAddComponent(JSContext *c, JSValueConst, int argc, JSValueConst *argv)
  {
    auto *scene = getScene(c);
    if(!scene)return JS_EXCEPTION;
    uint32_t uuid{};
    if(argc < 2 || !readU32(c, argv[0], uuid))return JS_ThrowTypeError(c, "addComponent expects (uuid, idOrName)");
    auto obj = scene->getObjectByUUID(uuid);
    if(!obj)return JS_ThrowReferenceError(c, "object %u not found", uuid);
    int compId = findCompId(c, argv[1]);
    if(compId < 0)return JS_ThrowTypeError(c, "unknown component, see components.list()");
    obj->addComponent(compId);
    return JS_NewString(c, std::to_string(obj->components.back().uuid).c_str());
  }

  JSValue sceneRemoveComponent(JSContext *c, JSValueConst, int argc, JSValueConst *argv)
  {
    auto *scene = getScene(c);
    if(!scene)return JS_EXCEPTION;
    uint32_t uuid{};
    uint64_t compUUID{};
    if(argc < 2 || !readU32(c, argv[0], uuid) || !readU64(c, argv[1], compUUID)) {
      return JS_ThrowTypeError(c, "removeComponent expects (uuid, componentUUID)");
    }
    auto obj = scene->getObjectByUUID(uuid);
    if(!obj)return JS_ThrowReferenceError(c, "object %u not found", uuid);
    obj->removeComponent(compUUID);
    return JS_UNDEFINED;
  }

  //########// assets //########//

  JSValue assetsList(JSContext *c, JSValueConst, int argc, JSValueConst *argv)
  {
    if(!ctx.project)return JS_ThrowReferenceError(c, "no project loaded");
    auto filter = argc > 0 && JS_IsString(argv[0]) ? lower(toString(argv[0], c)) : std::string{};

    Json arr = Json::array();
    for(auto &typeEntries : ctx.project->getAssets().getEntries()) {
      for(auto &e : typeEntries) {
        if(!filter.empty() && filter != fileTypeName(e.type))continue;
        arr.push_back(assetSummary(e));
      }
    }
    return fromJson(c, arr);
  }

  JSValue assetsGet(JSContext *c, JSValueConst, int argc, JSValueConst *argv)
  {
    if(!ctx.project)return JS_ThrowReferenceError(c, "no project loaded");
    uint64_t uuid{};
    if(argc < 1 || !readU64(c, argv[0], uuid))return JS_ThrowTypeError(c, "get expects (assetUUID)");
    auto *e = ctx.project->getAssets().getEntryByUUID(uuid);
    if(!e)return JS_NULL;
    auto j = assetSummary(*e);
    if(e->type == Project::FileType::MODEL_3D) {
      Json anims = Json::array();
      for(auto &anim : e->model.t3dm.animations)anims.push_back(anim.name);
      j["animations"] = anims;
    }
    return fromJson(c, j);
  }

  //########// misc //########//

  JSValue componentsList(JSContext *c, JSValueConst, int, JSValueConst*)
  {
    Json arr = Json::array();
    for(auto &info : Project::Component::TABLE) {
      arr.push_back(Json{{"id", info.id}, {"name", info.name}});
    }
    return fromJson(c, arr);
  }

  JSValue consoleRead(JSContext *c, JSValueConst, int argc, JSValueConst *argv)
  {
    int32_t maxChars = 4000;
    if(argc > 0)JS_ToInt32(c, &maxChars, argv[0]);
    const auto &log = Utils::Logger::getLogStripped();
    if(maxChars > 0 && log.size() > (size_t)maxChars) {
      auto tail = log.substr(log.size() - (size_t)maxChars);
      return JS_NewStringLen(c, tail.data(), tail.size());
    }
    return JS_NewStringLen(c, log.data(), log.size());
  }

  JSValue editorIsBuilding(JSContext *c, JSValueConst, int, JSValueConst*)
  {
    return JS_NewBool(c, ctx.isBuildOrRunning());
  }


  //########// scene management helpers //########//

  // Name as the editor sees it: the open scene's in-memory name wins over the file
  Json sceneEntryJson(int id, const std::string &fileName)
  {
    auto *loaded = ctx.project->getScenes().getLoadedScene();
    bool isLoaded = loaded && loaded->getId() == id;
    return Json{
      {"id", id},
      {"name", isLoaded ? loaded->getName() : fileName},
      {"loaded", isLoaded},
      {"onBoot", (int)ctx.project->conf.sceneIdOnBoot == id},
      {"onReset", (int)ctx.project->conf.sceneIdOnReset == id},
    };
  }

  const Project::SceneEntry* findSceneEntry(int id)
  {
    for(auto &e : ctx.project->getScenes().getEntries()) {
      if(e.id == id)return &e;
    }
    return nullptr;
  }

  int maxSceneId()
  {
    int id = 0;
    for(auto &e : ctx.project->getScenes().getEntries())id = std::max(id, e.id);
    return id;
  }

  std::string sceneFilePath(int id)
  {
    return ctx.project->getPath() + "/data/scenes/" + std::to_string(id) + "/scene.json";
  }

  // Renames a scene that is not open by editing its file
  bool renameSceneFile(int id, const std::string &name)
  {
    auto path = sceneFilePath(id);
    auto text = Utils::FS::loadTextFile(path);
    if(text.empty())return false;
    auto doc = Json::parse(text, nullptr, false);
    if(doc.is_discarded() || !doc.is_object())return false;
    doc["conf"]["name"] = name;
    Utils::FS::saveTextFile(path, doc.dump(2));
    return true;
  }

  bool readSceneId(JSContext *c, JSValueConst v, int &out)
  {
    int32_t id{};
    if(JS_ToInt32(c, &id, v) != 0)return false;
    out = id;
    return true;
  }

  bool readJsonArg(JSContext *c, JSValueConst v, Json &out)
  {
    std::string text{};
    if(JS_IsString(v)) {
      text = toString(v, c);
    } else {
      JSValue str = JS_JSONStringify(c, v, JS_UNDEFINED, JS_UNDEFINED);
      if(JS_IsException(str))return false;
      text = toString(str, c);
      JS_FreeValue(c, str);
    }
    out = Json::parse(text, nullptr, false);
    if(out.is_discarded()) {
      JS_ThrowTypeError(c, "invalid JSON");
      return false;
    }
    return true;
  }

  void afterSceneSwitch()
  {
    if(auto *scene = ctx.project->getScenes().getLoadedScene())ctx.sanitizeObjectSelection(scene);
  }

  //########// scene management //########//

  // scene.newScene(name?, open = true) -> {id, name, ...}
  JSValue sceneNew(JSContext *c, JSValueConst, int argc, JSValueConst *argv)
  {
    if(!ctx.project)return JS_ThrowReferenceError(c, "no project loaded");
    auto &scenes = ctx.project->getScenes();
    bool open = (argc > 1 && !JS_IsUndefined(argv[1])) ? JS_ToBool(c, argv[1]) : true;
    auto *prev = scenes.getLoadedScene();
    int prevId = prev ? prev->getId() : 0;

    scenes.add();
    int newId = maxSceneId();
    if(newId <= 0)return JS_ThrowInternalError(c, "failed to create scene folder");

    // loadScene saves the open scene first, then opens the new (empty) one
    scenes.loadScene(newId);
    auto *scene = scenes.getLoadedScene();
    if(!scene)return JS_ThrowInternalError(c, "failed to open new scene");
    if(argc > 0 && JS_IsString(argv[0]))scene->conf.name.value = toString(argv[0], c);
    scene->save();
    scenes.reload();

    if(!open && prevId > 0)scenes.loadScene(prevId);
    afterSceneSwitch();
    auto *e = findSceneEntry(newId);
    return fromJson(c, sceneEntryJson(newId, e ? e->name : std::string{}));
  }

  // scene.rename(name) renames the open scene, scene.rename(id, name) any scene
  JSValue sceneRename(JSContext *c, JSValueConst, int argc, JSValueConst *argv)
  {
    if(!ctx.project)return JS_ThrowReferenceError(c, "no project loaded");
    auto &scenes = ctx.project->getScenes();
    auto *loaded = scenes.getLoadedScene();

    int id = 0;
    std::string name{};
    bool hasId = argc >= 2 && !JS_IsUndefined(argv[1]);
    if(argc >= 1 && !hasId) {
      if(!loaded)return JS_ThrowReferenceError(c, "no scene loaded");
      id = loaded->getId();
      name = toString(argv[0], c);
    } else if(hasId) {
      if(!readSceneId(c, argv[0], id))return JS_EXCEPTION;
      name = toString(argv[1], c);
    } else {
      return JS_ThrowTypeError(c, "rename expects (name) or (id, name)");
    }
    if(name.empty())return JS_ThrowTypeError(c, "name must not be empty");
    if(!findSceneEntry(id))return JS_ThrowReferenceError(c, "scene %d not found", id);

    if(loaded && loaded->getId() == id) {
      loaded->conf.name.value = name;
      loaded->save();
    } else if(!renameSceneFile(id, name)) {
      return JS_ThrowInternalError(c, "could not update scene %d's file", id);
    }
    scenes.reload();
    return fromJson(c, sceneEntryJson(id, name));
  }

  // scene.duplicate(id, name?) -> new scene entry; default name is "<name> Copy"
  JSValue sceneDuplicate(JSContext *c, JSValueConst, int argc, JSValueConst *argv)
  {
    if(!ctx.project)return JS_ThrowReferenceError(c, "no project loaded");
    auto &scenes = ctx.project->getScenes();
    int id = 0;
    if(argc < 1 || !readSceneId(c, argv[0], id))return JS_ThrowTypeError(c, "duplicate expects (id, name?)");
    auto *src = findSceneEntry(id);
    if(!src)return JS_ThrowReferenceError(c, "scene %d not found", id);

    auto *loaded = scenes.getLoadedScene();
    std::string srcName = (loaded && loaded->getId() == id) ? loaded->getName() : src->name;
    // copy what's on screen, not the last save
    if(loaded && loaded->getId() == id)loaded->save();

    scenes.duplicate(id);
    int newId = maxSceneId();
    std::string name = (argc > 1 && JS_IsString(argv[1])) ? toString(argv[1], c) : srcName + " Copy";
    renameSceneFile(newId, name);
    scenes.reload();
    return fromJson(c, sceneEntryJson(newId, name));
  }

  // scene.deleteScene(id, force = false). Refuses the last scene, and the boot/reset scene unless forced.
  JSValue sceneDelete(JSContext *c, JSValueConst, int argc, JSValueConst *argv)
  {
    if(!ctx.project)return JS_ThrowReferenceError(c, "no project loaded");
    auto &scenes = ctx.project->getScenes();
    int id = 0;
    if(argc < 1 || !readSceneId(c, argv[0], id))return JS_ThrowTypeError(c, "deleteScene expects (id, force?)");
    bool force = argc > 1 ? JS_ToBool(c, argv[1]) : false;
    if(!findSceneEntry(id))return JS_ThrowReferenceError(c, "scene %d not found", id);
    if(scenes.getEntries().size() <= 1)return JS_ThrowRangeError(c, "can't delete the only scene");
    bool isBoot = (int)ctx.project->conf.sceneIdOnBoot == id;
    bool isReset = (int)ctx.project->conf.sceneIdOnReset == id;
    if((isBoot || isReset) && !force) {
      return JS_ThrowRangeError(c, "scene %d is the %s scene, pass force = true or change it first",
        id, isBoot ? "boot" : "reset");
    }
    scenes.remove(id);
    afterSceneSwitch();
    return JS_TRUE;
  }

  JSValue sceneConfig(JSContext *c, JSValueConst, int, JSValueConst*)
  {
    auto *scene = getScene(c);
    if(!scene)return JS_EXCEPTION;
    return fromJson(c, scene->conf.serialize());
  }

  // Merge-patches the open scene's settings (RFC 7386: arrays such as layers3D are replaced whole)
  JSValue sceneSetConfig(JSContext *c, JSValueConst, int argc, JSValueConst *argv)
  {
    auto *scene = getScene(c);
    if(!scene)return JS_EXCEPTION;
    Json patch{};
    if(argc < 1 || !readJsonArg(c, argv[0], patch) || !patch.is_object()) {
      return JS_ThrowTypeError(c, "setConfig expects ({...})");
    }
    try {
      Json conf = scene->conf.serialize();
      conf.merge_patch(patch);
      scene->deserializeConf(conf);
    } catch(const std::exception &ex) {
      return JS_ThrowTypeError(c, "bad value: %s", ex.what());
    }
    return fromJson(c, scene->conf.serialize());
  }

  // scene.setProps(uuid, {name, enabled, selectable, visMask, proportionalScale})
  JSValue sceneSetProps(JSContext *c, JSValueConst, int argc, JSValueConst *argv)
  {
    auto *scene = getScene(c);
    if(!scene)return JS_EXCEPTION;
    uint32_t uuid{};
    Json p{};
    if(argc < 2 || !readU32(c, argv[0], uuid) || !readJsonArg(c, argv[1], p) || !p.is_object()) {
      return JS_ThrowTypeError(c, "setProps expects (uuid, {...})");
    }
    auto obj = scene->getObjectByUUID(uuid);
    if(!obj)return JS_ThrowReferenceError(c, "object %u not found", uuid);
    try {
      if(p.contains("name"))obj->name = p["name"].get<std::string>();
      if(p.contains("enabled"))obj->enabled = p["enabled"].get<bool>();
      if(p.contains("selectable"))obj->selectable = p["selectable"].get<bool>();
      if(p.contains("proportionalScale"))obj->proportionalScale = p["proportionalScale"].get<bool>();
      if(p.contains("visMask"))obj->visMask.value = p["visMask"].get<uint32_t>();
    } catch(const std::exception &ex) {
      return JS_ThrowTypeError(c, "bad value: %s", ex.what());
    }
    return fromJson(c, objSummary(*obj, 0, 0));
  }

  Project::Component::Entry* findComponent(JSContext *c, Project::Scene *scene, JSValueConst vObj, JSValueConst vComp, std::shared_ptr<Project::Object> &objOut)
  {
    uint32_t uuid{};
    uint64_t compUUID{};
    if(!readU32(c, vObj, uuid) || !readU64(c, vComp, compUUID)) {
      JS_ThrowTypeError(c, "expects (uuid, componentUUID)");
      return nullptr;
    }
    objOut = scene->getObjectByUUID(uuid);
    if(!objOut) {
      JS_ThrowReferenceError(c, "object %u not found", uuid);
      return nullptr;
    }
    for(auto &comp : objOut->components) {
      if(comp.uuid == compUUID)return &comp;
    }
    if(objOut->isPrefabInstance()) {
      JS_ThrowReferenceError(c, "component not on object %u (prefab instance: edit the prefab or unpackPrefab first)", uuid);
    } else {
      JS_ThrowReferenceError(c, "component not found on object %u", uuid);
    }
    return nullptr;
  }

  Json componentJson(const Project::Component::Entry &comp)
  {
    auto &info = Project::Component::TABLE[comp.id];
    return Json{
      {"uuid", std::to_string(comp.uuid)},
      {"id", comp.id},
      {"type", info.name},
      {"name", comp.name},
      {"enabled", comp.enabled.value},
      {"data", info.funcSerialize ? info.funcSerialize(comp) : Json::object()},
    };
  }

  JSValue sceneGetComponent(JSContext *c, JSValueConst, int argc, JSValueConst *argv)
  {
    auto *scene = getScene(c);
    if(!scene)return JS_EXCEPTION;
    if(argc < 2)return JS_ThrowTypeError(c, "getComponent expects (uuid, componentUUID)");
    std::shared_ptr<Project::Object> obj{};
    auto *comp = findComponent(c, scene, argv[0], argv[1], obj);
    if(!comp)return JS_EXCEPTION;
    return fromJson(c, componentJson(*comp));
  }

  // scene.setComponent(uuid, compUUID, {name?, enabled?, data?}); data is merge-patched onto the current settings
  JSValue sceneSetComponent(JSContext *c, JSValueConst, int argc, JSValueConst *argv)
  {
    auto *scene = getScene(c);
    if(!scene)return JS_EXCEPTION;
    if(argc < 3)return JS_ThrowTypeError(c, "setComponent expects (uuid, componentUUID, {name?, enabled?, data?})");
    std::shared_ptr<Project::Object> obj{};
    auto *comp = findComponent(c, scene, argv[0], argv[1], obj);
    if(!comp)return JS_EXCEPTION;
    Json p{};
    if(!readJsonArg(c, argv[2], p) || !p.is_object())return JS_ThrowTypeError(c, "third argument must be an object");

    auto &info = Project::Component::TABLE[comp->id];
    try {
      // build the new data first so a bad patch leaves the component untouched
      std::shared_ptr<void> newData{};
      if(p.contains("data")) {
        if(!info.funcSerialize || !info.funcDeserialize)return JS_ThrowTypeError(c, "component has no editable data");
        Json data = info.funcSerialize(*comp);
        data.merge_patch(p["data"]);
        newData = info.funcDeserialize(data);
        if(!newData)return JS_ThrowTypeError(c, "component rejected the data");
      }
      if(p.contains("name"))comp->name = p["name"].get<std::string>();
      if(p.contains("enabled"))comp->enabled.value = p["enabled"].get<bool>();
      if(newData)comp->data = newData;
    } catch(const std::exception &ex) {
      return JS_ThrowTypeError(c, "bad value: %s", ex.what());
    }
    return fromJson(c, componentJson(*comp));
  }

  JSValue sceneCreatePrefab(JSContext *c, JSValueConst, int argc, JSValueConst *argv)
  {
    auto *scene = getScene(c);
    if(!scene)return JS_EXCEPTION;
    uint32_t uuid{};
    if(argc < 1 || !readU32(c, argv[0], uuid))return JS_ThrowTypeError(c, "createPrefab expects (uuid, subDir?)");
    if(!scene->getObjectByUUID(uuid))return JS_ThrowReferenceError(c, "object %u not found", uuid);
    std::string subDir = (argc > 1 && JS_IsString(argv[1])) ? toString(argv[1], c) : std::string{};
    uint64_t prefabUUID = scene->createPrefabFromObject(uuid, subDir);
    if(!prefabUUID)return JS_ThrowInternalError(c, "prefab creation failed");
    return JS_NewString(c, std::to_string(prefabUUID).c_str());
  }

  JSValue sceneUnpackPrefab(JSContext *c, JSValueConst, int argc, JSValueConst *argv)
  {
    auto *scene = getScene(c);
    if(!scene)return JS_EXCEPTION;
    uint32_t uuid{};
    if(argc < 1 || !readU32(c, argv[0], uuid))return JS_ThrowTypeError(c, "unpackPrefab expects (uuid)");
    auto obj = scene->getObjectByUUID(uuid);
    if(!obj)return JS_ThrowReferenceError(c, "object %u not found", uuid);
    if(!obj->isPrefabInstance())return JS_ThrowTypeError(c, "object %u is not a prefab instance", uuid);
    scene->unpackPrefabInstance(uuid);
    return JS_TRUE;
  }

  //########// project //########//

  JSValue projectConfig(JSContext *c, JSValueConst, int, JSValueConst*)
  {
    if(!ctx.project)return JS_ThrowReferenceError(c, "no project loaded");
    return fromJson(c, Json::parse(ctx.project->conf.serialize(), nullptr, false));
  }

  // Merge-patches project settings: name, romName, sceneIdOnBoot, sceneIdOnReset, debugMenu,
  // collLayer0..7, visLayer0..7, romHeader, metadata, pathEmu, pathN64Inst
  JSValue projectSetConfig(JSContext *c, JSValueConst, int argc, JSValueConst *argv)
  {
    if(!ctx.project)return JS_ThrowReferenceError(c, "no project loaded");
    Json patch{};
    if(argc < 1 || !readJsonArg(c, argv[0], patch) || !patch.is_object()) {
      return JS_ThrowTypeError(c, "setConfig expects ({...})");
    }
    try {
      for(auto key : {"sceneIdOnBoot", "sceneIdOnReset"}) {
        if(patch.contains(key) && !findSceneEntry(patch[key].get<int>())) {
          return JS_ThrowReferenceError(c, "%s: scene %d not found", key, patch[key].get<int>());
        }
      }
      Json conf = Json::parse(ctx.project->conf.serialize(), nullptr, false);
      if(conf.is_discarded())return JS_ThrowInternalError(c, "could not read project config");
      conf.merge_patch(patch);
      ctx.project->deserialize(conf);
    } catch(const std::exception &ex) {
      return JS_ThrowTypeError(c, "bad value: %s", ex.what());
    }
    ctx.project->markDirty();
    return fromJson(c, Json::parse(ctx.project->conf.serialize(), nullptr, false));
  }

  //########// asset settings //########//

  JSValue assetsConfig(JSContext *c, JSValueConst, int argc, JSValueConst *argv)
  {
    if(!ctx.project)return JS_ThrowReferenceError(c, "no project loaded");
    uint64_t uuid{};
    if(argc < 1 || !readU64(c, argv[0], uuid))return JS_ThrowTypeError(c, "config expects (assetUUID)");
    auto *e = ctx.project->getAssets().getEntryByUUID(uuid);
    if(!e)return JS_ThrowReferenceError(c, "asset not found");
    auto j = assetSummary(*e);
    j["conf"] = Json::parse(e->conf.serialize(), nullptr, false);
    return fromJson(c, j);
  }

  // assets.setConfig(uuid, {format, baseScale, baseScaleOverride, gltfBVH, compression, exclude, autoRun,
  //   wavForceMono, wavResampleRate, wavCompression, fontId, fontCharset, data})
  JSValue assetsSetConfig(JSContext *c, JSValueConst, int argc, JSValueConst *argv)
  {
    if(!ctx.project)return JS_ThrowReferenceError(c, "no project loaded");
    uint64_t uuid{};
    Json p{};
    if(argc < 2 || !readU64(c, argv[0], uuid) || !readJsonArg(c, argv[1], p) || !p.is_object()) {
      return JS_ThrowTypeError(c, "setConfig expects (assetUUID, {...})");
    }
    auto &assets = ctx.project->getAssets();
    auto *e = assets.getEntryByUUID(uuid);
    if(!e)return JS_ThrowReferenceError(c, "asset not found");
    auto &cf = e->conf;
    try {
      if(p.contains("format"))cf.format = p["format"].get<int>();
      if(p.contains("baseScale"))cf.baseScale = p["baseScale"].get<int>();
      if(p.contains("baseScaleOverride"))cf.baseScaleOverride = p["baseScaleOverride"].get<int>();
      if(p.contains("gltfBVH"))cf.gltfBVH = p["gltfBVH"].get<bool>();
      if(p.contains("compression"))cf.compression = (Project::ComprTypes)p["compression"].get<int>();
      if(p.contains("exclude"))cf.exclude = p["exclude"].get<bool>();
      if(p.contains("autoRun"))cf.autoRun = p["autoRun"].get<bool>();
      if(p.contains("wavForceMono"))cf.wavForceMono.value = p["wavForceMono"].get<bool>();
      if(p.contains("wavResampleRate"))cf.wavResampleRate.value = p["wavResampleRate"].get<uint32_t>();
      if(p.contains("wavCompression"))cf.wavCompression.value = p["wavCompression"].get<int32_t>();
      if(p.contains("fontId"))cf.fontId.value = p["fontId"].get<uint32_t>();
      if(p.contains("fontCharset"))cf.fontCharset.value = p["fontCharset"].get<std::string>();
      if(p.contains("data")) {
        if(!cf.data.is_object())cf.data = Json::object();
        cf.data.merge_patch(p["data"]);
      }
    } catch(const std::exception &ex) {
      return JS_ThrowTypeError(c, "bad value: %s", ex.what());
    }
    assets.markAssetMetaDirty(uuid);
    auto j = assetSummary(*e);
    j["conf"] = Json::parse(cf.serialize(), nullptr, false);
    return fromJson(c, j);
  }

  JSValue assetsCreateScript(JSContext *c, JSValueConst, int argc, JSValueConst *argv)
  {
    if(!ctx.project)return JS_ThrowReferenceError(c, "no project loaded");
    if(argc < 1 || !JS_IsString(argv[0]))return JS_ThrowTypeError(c, "createScript expects (name, isGlobal?, subDir?)");
    auto name = toString(argv[0], c);
    bool isGlobal = argc > 1 ? JS_ToBool(c, argv[1]) : false;
    std::string subDir = (argc > 2 && JS_IsString(argv[2])) ? toString(argv[2], c) : std::string{};
    return JS_NewBool(c, ctx.project->getAssets().createScript(name, isGlobal, subDir));
  }

  JSValue assetsCreateNodeGraph(JSContext *c, JSValueConst, int argc, JSValueConst *argv)
  {
    if(!ctx.project)return JS_ThrowReferenceError(c, "no project loaded");
    if(argc < 1 || !JS_IsString(argv[0]))return JS_ThrowTypeError(c, "createNodeGraph expects (name)");
    uint64_t uuid = ctx.project->getAssets().createNodeGraph(toString(argv[0], c));
    if(!uuid)return JS_ThrowInternalError(c, "node graph creation failed");
    return JS_NewString(c, std::to_string(uuid).c_str());
  }

  const JSCFunctionListEntry SCENE_FUNCS[] = {
    JS_CFUNC_DEF("list", 0, sceneList),
    JS_CFUNC_DEF("current", 0, sceneCurrent),
    JS_CFUNC_DEF("objects", 2, sceneObjects),
    JS_CFUNC_DEF("find", 1, sceneFind),
    JS_CFUNC_DEF("get", 1, sceneGet),
    JS_CFUNC_DEF("create", 2, sceneCreate),
    JS_CFUNC_DEF("addModel", 1, sceneAddModel),
    JS_CFUNC_DEF("addPrefab", 1, sceneAddPrefab),
    JS_CFUNC_DEF("addJSON", 2, sceneAddJSON),
    JS_CFUNC_DEF("serialize", 1, sceneSerialize),
    JS_CFUNC_DEF("remove", 1, sceneRemove),
    JS_CFUNC_DEF("move", 3, sceneMove),
    JS_CFUNC_DEF("select", 1, sceneSelect),
    JS_CFUNC_DEF("setEnabled", 2, sceneSetEnabled),
    JS_CFUNC_DEF("addComponent", 2, sceneAddComponent),
    JS_CFUNC_DEF("removeComponent", 2, sceneRemoveComponent),
    JS_CFUNC_DEF("newScene", 2, sceneNew),
    JS_CFUNC_DEF("rename", 2, sceneRename),
    JS_CFUNC_DEF("duplicate", 2, sceneDuplicate),
    JS_CFUNC_DEF("deleteScene", 2, sceneDelete),
    JS_CFUNC_DEF("config", 0, sceneConfig),
    JS_CFUNC_DEF("setConfig", 1, sceneSetConfig),
    JS_CFUNC_DEF("setProps", 2, sceneSetProps),
    JS_CFUNC_DEF("getComponent", 2, sceneGetComponent),
    JS_CFUNC_DEF("setComponent", 3, sceneSetComponent),
    JS_CFUNC_DEF("createPrefab", 2, sceneCreatePrefab),
    JS_CFUNC_DEF("unpackPrefab", 1, sceneUnpackPrefab),
  };

  const JSCFunctionListEntry PROJECT_FUNCS[] = {
    JS_CFUNC_DEF("config", 0, projectConfig),
    JS_CFUNC_DEF("setConfig", 1, projectSetConfig),
  };

  const JSCFunctionListEntry ASSET_FUNCS[] = {
    JS_CFUNC_DEF("list", 1, assetsList),
    JS_CFUNC_DEF("get", 1, assetsGet),
    JS_CFUNC_DEF("config", 1, assetsConfig),
    JS_CFUNC_DEF("setConfig", 2, assetsSetConfig),
    JS_CFUNC_DEF("createScript", 3, assetsCreateScript),
    JS_CFUNC_DEF("createNodeGraph", 1, assetsCreateNodeGraph),
  };

  const JSCFunctionListEntry COMPONENT_FUNCS[] = {
    JS_CFUNC_DEF("list", 0, componentsList),
  };

  const JSCFunctionListEntry CONSOLE_FUNCS[] = {
    JS_CFUNC_DEF("read", 1, consoleRead),
  };

  const JSCFunctionListEntry EDITOR_FUNCS[] = {
    JS_CFUNC_DEF("isBuilding", 0, editorIsBuilding),
  };

  // adds functions to an existing global object, or creates it
  void extendGlobal(JSContext *c, JSValue global, const char *name, const JSCFunctionListEntry *funcs, int count)
  {
    JSValue obj = JS_GetPropertyStr(c, global, name);
    if(!JS_IsObject(obj)) {
      JS_FreeValue(c, obj);
      obj = JS_NewObject(c);
      JS_SetPropertyFunctionList(c, obj, funcs, count);
      JS_SetPropertyStr(c, global, name, obj); // takes ownership
      return;
    }
    JS_SetPropertyFunctionList(c, obj, funcs, count);
    JS_FreeValue(c, obj);
  }
}

void Editor::ScriptHost::Impl::initSceneApi(JSContext *c)
{
  JSValue global = JS_GetGlobalObject(c);
  extendGlobal(c, global, "scene", SCENE_FUNCS, (int)std::size(SCENE_FUNCS));
  extendGlobal(c, global, "assets", ASSET_FUNCS, (int)std::size(ASSET_FUNCS));
  extendGlobal(c, global, "project", PROJECT_FUNCS, (int)std::size(PROJECT_FUNCS));
  extendGlobal(c, global, "components", COMPONENT_FUNCS, (int)std::size(COMPONENT_FUNCS));
  extendGlobal(c, global, "console", CONSOLE_FUNCS, (int)std::size(CONSOLE_FUNCS));
  extendGlobal(c, global, "editor", EDITOR_FUNCS, (int)std::size(EDITOR_FUNCS));
  JS_FreeValue(c, global);
}
