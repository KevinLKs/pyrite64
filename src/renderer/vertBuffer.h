/**
* @copyright 2025 - Max Bebök
* @license MIT
*/
#pragma once
#include <string>
#include <vector>

#include "vertex.h"
#include "SDL3/SDL_gpu.h"

namespace Renderer
{
  class VertBuffer
  {
    private:
      SDL_GPUDevice* gpuDevice{nullptr};

      SDL_GPUBuffer* buffer{nullptr};
      SDL_GPUBuffer* bufferIdx{nullptr};
      SDL_GPUBuffer* nextBuffer{nullptr};
      SDL_GPUBuffer* nextBufferIdx{nullptr};

      SDL_GPUTransferBuffer* bufferTrans{nullptr};
      SDL_GPUTransferBuffer* bufferIdxTrans{nullptr};

      size_t capacityVert{0};
      size_t capacityIdx{0};
      size_t currVertByteSize{0};
      size_t currIdxByteSize{0};
      uint32_t drawIdxCount{0}; // index count for the GPU
      bool needsUpload{false};

      void grow(size_t sizeVert, size_t sizeIndex);

      void setData(char* verts, uint32_t vertsSize,
        const std::vector<uint16_t> &indices
      );

    public:
      VertBuffer(SDL_GPUDevice* device);
      ~VertBuffer();

      void setData(
        const std::vector<Vertex> &vertices,
        const std::vector<uint16_t> &indices
      ) {
        setData(
          (char*)vertices.data(),
          static_cast<uint32_t>(vertices.size() * sizeof(Vertex)),
          indices
        );
      }

      void setData(
        const std::vector<LineVertex> &vertices,
        const std::vector<uint16_t> &indices
      ) {
        setData(
          (char*)vertices.data(),
          static_cast<uint32_t>(vertices.size() * sizeof(LineVertex)),
          indices
        );
      }

      void upload(SDL_GPUCopyPass& pass);

      void addBindings(SDL_GPUBufferBinding binding[]) const {
        binding[0].buffer = buffer;
        binding[0].offset = 0;

        binding[1].buffer = bufferIdx;
        binding[1].offset = 0;
      }

      uint32_t getIndexCount() const {
        return drawIdxCount;
      }
  };
}