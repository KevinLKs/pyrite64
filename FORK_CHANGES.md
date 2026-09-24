# Pyrite64 fork changes (Kevin)

Changes made on top of upstream Pyrite64. Keep this list current so an upstream merge does not silently drop them.

## 2026-09-24: Asset watching and project open speed

### Problem
- `AssetManager::pollWatch()` walked all of `assets/` and `src/user/` every 2 seconds on the UI thread and stat'ed every file. With ~4,600 files that was a visible hitch every 2 seconds.
- Adding many files at once was quadratic: each new file searched every existing entry twice (`removeEntryByPath`, `getByPath`).
- Opening a project fully decoded every PNG just to read its size, and fully parsed every 3D model, every time.

### What changed
| Area | Change | Files |
| --- | --- | --- |
| File watching | New `Utils::FileWatcher`: OS change notifications (ReadDirectoryChangesW on Windows, inotify on Linux) on background threads. Only changed paths are processed. Paths must be quiet for 300 ms before they are read, so half-copied files are skipped until done. | `src/utils/fileWatcher.h/.cpp`, `CMakeLists.txt` |
| Incremental updates | `pollWatch()` now drains the watcher and calls `applyChanges()`, which touches only the reported files. Folder moves/deletes (which only report the folder) are expanded by listing that folder. If the OS drops events (buffer overflow) one full `syncWithDisk()` runs. | `src/project/assetManager.cpp/.h` |
| Lookup speed | Path lookups use a `pathIndex` hash map instead of a linear search. Batch adds are no longer quadratic. `getByPath()` is O(1). | `src/project/assetManager.cpp/.h` |
| Fallback | macOS (no backend yet) or a watcher that fails to start falls back to the old 2 s poll, now using cached directory timestamps. | `src/project/assetManager.cpp` |
| Lazy models | In the editor, models are not parsed at project open. `getEntryByUUID()` loads a model on first access (scene, inspector, thumbnail). Builds (`Project` created without editor mode) still load every model. | `src/project/assetManager.h/.cpp`, `src/project/project.h/.cpp`, `src/editor/globalActions.cpp` |
| PNG size | `Renderer::Texture` reads width/height from the PNG header (24 bytes) instead of decoding the whole image. Non-PNG and SVG still decode. | `src/renderer/texture.cpp` |
| Rescan button | Project > Settings > Assets > "Rescan All Assets" runs the existing full reload (same as the reload hotkey). Disabled while there are unsaved asset changes, since a reload discards them. | `src/editor/pages/parts/projectSettings.cpp` |
| Small | Creating a script or node graph adds just that file instead of reloading the whole project. Changed models get their thumbnail invalidated. | `src/project/assetManager.cpp` |

### Known limits
- First access to a model not yet loaded (opening a scene, selecting it) pauses briefly while it parses. Saved thumbnails on disk do not need the model loaded.
- Builds still parse every model each build (unchanged from upstream). Possible follow-up: only parse models whose output is out of date.
- Dropping thousands of files still costs one pause when they are processed, proportional to the new files only.
- Linux: large trees can hit `fs.inotify.max_user_watches`. The watcher logs it and forces a full rescan.

### Test checklist
- [ ] Idle editor with all 151 Pokemon in `assets/`: no periodic hitch.
- [ ] Copy one PNG into `assets/`: appears in the browser within about half a second.
- [ ] Copy a whole folder in, move a folder in (same drive), delete a folder: browser updates each time.
- [ ] Edit a texture used by an open model: viewport updates.
- [ ] Project open time with all 151 models, compared to before.
- [ ] Project > Settings > Rescan All Assets works, and is disabled with unsaved asset changes.
- [ ] Build and run in ares still works (models load eagerly in the build).
