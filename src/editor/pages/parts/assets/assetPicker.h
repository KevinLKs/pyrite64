/**
* @copyright 2026 - Max Bebök
* @license MIT
*/
#pragma once
#include <cstdint>
#include <string>
#include "../../../../project/assetManager.h"

namespace Editor::AssetPicker
{
  /**
   * Asset field with a folder-based picker popup (thumbnails, breadcrumbs, search).
   * Supports images (texture preview) and 3D models (turntable thumbnails, hover to spin).
   *
   * The popup opens in startDir (project-relative, e.g. "assets/characters/hero") if given,
   * else in the folder of the current selection, else the last folder picked from for this
   * asset type, else "assets".
   * Accepts matching assets dragged in from the asset browser and records an undo step.
   * @return true if the selection changed
   */
  bool draw(const char *label, uint64_t &uuid, Project::FileType type, const std::string &startDir = "");
}
