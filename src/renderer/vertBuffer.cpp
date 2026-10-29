/**
* @copyright 2025 - Max Bebök
* @license MIT
*/
#include "vertBuffer.h"
#include <algorithm>

Renderer::VertBuffer::VertBuffer(SDL_GPUDevice* device)
  : gpuDevice{device}
{
}

Renderer::VertBuffer::~VertBuffer()
{
  SDL_ReleaseGPUBuffer(gpuDevice, buffer);
  SDL_ReleaseGPUBuffer(gpuDevice, bufferIdx);
  SDL_ReleaseGPUBuffer(gpuDevice, nextBuffer);
  SDL_ReleaseGPUBuffer(gpuDevice, nextBufferIdx);
  SDL_ReleaseGPUTransferBuffer(gpuDevice, bufferTrans);
  SDL_ReleaseGPUTransferBuffer(gpuDevice, bufferIdxTrans);
}

void Renderer::VertBuffer::grow(size_t sizeVert, size_t sizeIndex)
{
  assert(sizeVert != 0);
  assert(sizeIndex != 0);

  // reserve some room (lines change across frames)
  capacityVert = std::max(sizeVert, capacityVert * 2);
  capacityIdx = std::max(sizeIndex, capacityIdx * 2);

  SDL_ReleaseGPUTransferBuffer(gpuDevice, bufferTrans);
  SDL_ReleaseGPUTransferBuffer(gpuDevice, bufferIdxTrans);
  SDL_ReleaseGPUBuffer(gpuDevice, nextBuffer);
  SDL_ReleaseGPUBuffer(gpuDevice, nextBufferIdx);

  SDL_GPUBufferCreateInfo bufferInfo{
    .usage = SDL_GPU_BUFFERUSAGE_VERTEX,
    .size = (uint32_t)capacityVert
  };
  nextBuffer = SDL_CreateGPUBuffer(gpuDevice, &bufferInfo);
  assert(nextBuffer != nullptr);

  bufferInfo.usage = SDL_GPU_BUFFERUSAGE_INDEX;
  bufferInfo.size = (uint32_t)capacityIdx;
  nextBufferIdx = SDL_CreateGPUBuffer(gpuDevice, &bufferInfo);
  assert(nextBufferIdx != nullptr);

  SDL_GPUTransferBufferCreateInfo transferInfo{
    .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
    .size = (uint32_t)capacityVert
  };
  bufferTrans = SDL_CreateGPUTransferBuffer(gpuDevice, &transferInfo);
  assert(bufferTrans != nullptr);

  transferInfo.size = (uint32_t)capacityIdx;
  bufferIdxTrans = SDL_CreateGPUTransferBuffer(gpuDevice, &transferInfo);
  assert(bufferIdxTrans != nullptr);
}

void Renderer::VertBuffer::setData(char* verts, uint32_t vertsSize, const std::vector<uint16_t>&indices)
{
  currVertByteSize = vertsSize;
  currIdxByteSize = indices.size() * sizeof(uint16_t);

  if (currVertByteSize > capacityVert || currIdxByteSize > capacityIdx) {
    grow(currVertByteSize, currIdxByteSize);
  }

// @TODO: store vert/indices in one single buffer

  auto data = (Vertex*)SDL_MapGPUTransferBuffer(gpuDevice, bufferTrans, true);
  SDL_memcpy(data, verts, currVertByteSize);
  SDL_UnmapGPUTransferBuffer(gpuDevice, bufferTrans);

  auto idxData = (uint16_t*)SDL_MapGPUTransferBuffer(gpuDevice, bufferIdxTrans, true);
  SDL_memcpy(idxData, indices.data(), currIdxByteSize);
  SDL_UnmapGPUTransferBuffer(gpuDevice, bufferIdxTrans);

  needsUpload = true;
}

void Renderer::VertBuffer::upload(SDL_GPUCopyPass &pass) {
  if (!needsUpload || currVertByteSize == 0)return;

  if (nextBuffer) {
    SDL_ReleaseGPUBuffer(gpuDevice, buffer);
    SDL_ReleaseGPUBuffer(gpuDevice, bufferIdx);
    buffer = nextBuffer;
    bufferIdx = nextBufferIdx;
    nextBuffer = nullptr;
    nextBufferIdx = nullptr;
  }

  SDL_GPUTransferBufferLocation location{};
  location.transfer_buffer = bufferTrans;
  location.offset = 0;

  SDL_GPUBufferRegion region{};
  region.buffer = buffer;
  region.size = currVertByteSize;
  region.offset = 0;

  SDL_UploadToGPUBuffer(&pass, &location, &region, true);

  location = {};
  location.transfer_buffer = bufferIdxTrans;
  region = {};
  region.buffer = bufferIdx;
  region.size = currIdxByteSize;

  SDL_UploadToGPUBuffer(&pass, &location, &region, true);

  drawIdxCount = (uint32_t)(currIdxByteSize / sizeof(uint16_t));
  needsUpload = false;
}
