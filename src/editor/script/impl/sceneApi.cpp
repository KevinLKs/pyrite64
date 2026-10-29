/**
* @license MIT
*
* Extra editor-script API used by the MCP bridge (and usable from any editor script):
*   scene.list() / scene.current() / scene.objects(rootUUID?, maxDepth?) / scene.find(text)
*   scene.get(uuid) / scene.create(name, parentUUID?) / scene.addModel(assetUUID)
*   scene.addPrefab(assetUUID) / scene.addJSON(json, parentUUID?) / scene.serialize(uuid)
*   scene.remove(uuid) / scene.move(uuid, targetUUID, asChild?) / scene.select(uuids)
*   scene.setEnabled(uuid, bool) / scene.addComponent(uuid, idOrName) / scene.removeComponent(uuid, compUUID)
*   assets.list(type?) / assets.get(uuid)
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

  //########// scene //########//

  JSValue sceneList(JSContext *c, JSValueConst, int, JSValueConst*)
  {
    if(!ctx.project)return JS_ThrowReferenceError(c, "no project loaded");
    Json arr = Json::array();
    for(auto &e : ctx.project->getScenes().getEntries()) {
      arr.push_back(Json{{"id", e.id}, {"name", e.name}});
    }
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
  };

  const JSCFunctionListEntry ASSET_FUNCS[] = {
    JS_CFUNC_DEF("list", 1, assetsList),
    JS_CFUNC_DEF("get", 1, assetsGet),
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
  extendGlobal(c, global, "components", COMPONENT_FUNCS, (int)std::size(COMPONENT_FUNCS));
  extendGlobal(c, global, "console", CONSOLE_FUNCS, (int)std::size(CONSOLE_FUNCS));
  extendGlobal(c, global, "editor", EDITOR_FUNCS, (int)std::size(EDITOR_FUNCS));
  JS_FreeValue(c, global);
}
