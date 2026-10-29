/**
* @copyright 2025 - Max Bebök
* @license MIT
*/
#pragma once
#include <t3d/t3dmodel.h>
#include <t3d/t3danim.h>

#include "assets/assetManager.h"
#include "lib/matrixManager.h"
#include "renderer/drawLayer.h"
#include "renderer/material.h"
#include "scene/object.h"
#include "script/scriptTable.h"

namespace P64::Comp
{
  struct AnimModel
  {
    static constexpr uint32_t ID = 10;

    private:
      T3DModel *model{};

      T3DSkeleton skelMain{};
      T3DSkeleton *skelAnim{};
      T3DAnim *anims{};

      int16_t animIdxMain{-1};
      int16_t animIdxBlend{-1};

      RingMat4FP matFP{};
      float vertexScale{1.0f};
      uint8_t layerIdx{0};
      uint8_t flags{0};

    public:
      float blendFactor{0.5f};
      Renderer::MaterialInstance material{};

      void setMainAnim(int16_t idx);
      void setBlendAnim(int16_t idx);

      Renderer::MaterialInstance& getMatInstance() {
        return material;
      }

      T3DAnim* getMainAnim() {
        if (animIdxMain < 0) return nullptr;
        return &anims[animIdxMain];
      }

      T3DAnim* getBlendAnim() {
        if (animIdxBlend < 0) return nullptr;
        return &anims[animIdxBlend];
      }

      T3DAnim* getAnim(int16_t idx) const {
        return &anims[idx];
      }

      // Poke64 fork (2026-10-21): read-only access for effects that follow a joint
      // (Stadium creature flames, src/user/StadiumFlames.cpp).
      T3DSkeleton* getSkeleton() {
        return skelMain.bones ? &skelMain : nullptr;
      }

      float getVertexScale() const {
        return vertexScale;
      }

      // Poke64 fork (2026-10-26): clip count, so scripts can find clips by name
      // (player idle/walk/run in src/user/PlayerController.cpp).
      int16_t getAnimCount() const {
        return model ? (int16_t)t3d_model_get_animation_count(model) : 0;
      }

    static uint32_t getAllocSize([[maybe_unused]] uint16_t* initData);

    static void initDelete([[maybe_unused]] Object& obj, AnimModel* data, void* initData);

    static void update(Object& obj, AnimModel* data, [[maybe_unused]] float deltaTime);

    static void draw([[maybe_unused]] Object& obj, AnimModel* data, [[maybe_unused]] float deltaTime);
  };
}