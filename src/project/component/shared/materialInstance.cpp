/**
* @copyright 2026 - Max Bebök
* @license MIT
*/
#include "materialInstance.h"

#include "../../assets/model3d.h"
#include "../../scene/object.h"
#include "../../scene/scene.h"
#include "../../../build/sceneContext.h"
#include <array>
#include <stdexcept>
#include <string>

nlohmann::json Project::Component::Shared::MaterialInstance::serialize() const
{
  Utils::JSON::Builder j{};
  j.set(setDepth).set(depth)
    .set(setPrim).set(prim)
    .set(setEnv).set(env)
    .set(setLighting).set(lighting)
    .set(setFresnel).set(fresnel)
    .set(fresnelColor);

  for(uint32_t i=0; i<texSlots.size(); ++i) {
    if(texSlots[i].set.value) {
      j.set("tex" + std::to_string(i), texSlots[i].serialize());
    }
  }
  return j.doc;
}

void Project::Component::Shared::MaterialInstance::deserialize(const nlohmann::json &doc)
{
  Utils::JSON::readProp(doc, setDepth, false);
  Utils::JSON::readProp(doc, depth);
  Utils::JSON::readProp(doc, setPrim, false);
  Utils::JSON::readProp(doc, prim, {1, 1, 1, 1});
  Utils::JSON::readProp(doc, setEnv, false);
  Utils::JSON::readProp(doc, env, {1, 1, 1, 1});
  Utils::JSON::readProp(doc, setLighting, false);
  Utils::JSON::readProp(doc, lighting, true);
  Utils::JSON::readProp(doc, setFresnel, false);
  Utils::JSON::readProp(doc, fresnel, 0);
  Utils::JSON::readProp(doc, fresnelColor, {1,1,1,1});

  for(int i=0; i<8; ++i) {
    if(doc.contains("tex" + std::to_string(i))) {
      texSlots[i].deserialize(doc["tex" + std::to_string(i)]);
    }
  }
}

void Project::Component::Shared::MaterialInstance::build(Utils::BinaryFile &file, Build::SceneCtx &ctx, Object &obj)
{
  uint16_t setMask = 0;
  if(setDepth.resolve(obj)) setMask |= 1 << 0;
  if(setPrim.resolve(obj))  setMask |= 1 << 1;
  if(setEnv.resolve(obj))   setMask |= 1 << 2;
  if(setLighting.resolve(obj)) setMask |= 1 << 3;
  if(setFresnel.resolve(obj)) setMask |= 1 << 4;

  for(size_t i=0; i<texSlots.size(); ++i) {
    if(texSlots[i].set.value) {
      setMask |= 1 << (texSlots.size() + i);
    }
  }

  uint8_t valFlags = depth.resolve(obj); // 2bits
  if(lighting.value) valFlags |= 1 << 2;

  auto posStart = file.getPos();
  file.write<uint32_t>(0); // size
  file.write<uint16_t>(setMask);
  file.write<uint8_t>(fresnel.resolve(obj));
  file.write<uint8_t>(valFlags);
  file.writeRGBA(prim.resolve(obj));
  file.writeRGBA(env.resolve(obj));
  file.writeRGBA(fresnelColor.resolve(obj));

  // A "Texture" placeholder without a texture used to be written as an invalid asset index,
  // which crashes the ROM on scene load (null sprite in Placeholder::update). Fail the build instead.
  for(size_t i=0; i<texSlots.size(); ++i)
  {
    const auto &slot = texSlots[i];
    if(!slot.set.value || slot.dynType.value != Assets::MaterialTex::DYN_TYPE_FULL)continue;
    if(ctx.assetUUIDToIdx.contains(slot.texUUID.value))continue;

    std::string error = "Material Instance slot #" + std::to_string(i) + " on object '" + obj.name + "' "
      + (slot.texUUID.value == 0 ? "has no texture set." : "uses a texture that no longer exists.");
    error += ctx.scene ? "\nScene: '" + ctx.scene->getName() + "'" : std::string{" (Prefab)"};
    error += "\nPick a texture for that slot in the object's Material Instance section.";
    throw std::runtime_error(error);
  }

  for(size_t i=0; i<texSlots.size(); ++i)
  {
    if(texSlots[i].set.value) {
      file.write<uint32_t>(0); // runtime pointer
      file.write<uint32_t>(0); // runtime pointer
      file.write<uint32_t>(0); // runtime pointer
      texSlots[i].build(file, ctx);
    }
  }

  auto size = file.getPos() - posStart;
  file.atPos(posStart, [&]{
    file.write<uint32_t>(size);
  });
}

void Project::Component::Shared::MaterialInstance::validateWithModel(const Assets::Model3D &model)
{
  // Fork change (2026-10-18): the runtime material reads the instance slot named by its own
  // dynPlaceholder index (tile.phIndex), so each instance slot is placed at that index instead of
  // being counted in unordered_map iteration order (which could swap e.g. eye and mouth slots).
  // An out-of-range or already used index falls back to the next free slot.
  std::array<bool, 8> used{};
  auto handleTex = [&](const Assets::MaterialTex &t, int tileIdx)
  {
    if(!t.dynType.value)return;
    int idx = t.dynPlaceholder.value;
    if(idx < 0 || idx >= (int)texSlots.size() || used[idx]) {
      idx = -1;
      for(int i=0; i<(int)texSlots.size(); ++i) {
        if(!used[i]) { idx = i; break; }
      }
      if(idx < 0)return;
    }
    used[idx] = true;
    texSlots[idx].set.value = true;
    texSlots[idx].dynPlaceholder.value = tileIdx;
    texSlots[idx].dynType = t.dynType;

    if(t.dynType.value == Assets::MaterialTex::DYN_TYPE_TILE) {
      // store texture size in repeat
      texSlots[idx].repeat.value = t.texSize.value;
    }
  };
  for(auto &[slot, tex] : model.materials)
  {
    handleTex(tex.tex0, 0);
    handleTex(tex.tex1, 1);
  }

  // disable the unused slots
  for(size_t i=0; i<texSlots.size(); ++i) {
    if(!used[i])texSlots[i].set.value = false;
  }
}
