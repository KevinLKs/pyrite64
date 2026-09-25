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
- ~~Builds still parse every model each build~~ fixed 2026-09-24, see below.
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

## 2026-09-24 (2): Texture picker and faster build start

### Texture picker
The Texture field in Material Instance and the Model Editor was a flat, alphabetized list of every image in the project. It is now a picker popup (`TextureEditor::drawTexturePicker`):
- Opens in the folder of the model being edited (the .glb's folder). The house button returns there, the up arrow and the breadcrumb segments navigate anywhere under `assets/`.
- "Include subfolders" (on by default) shows every image below the current folder, so a model with a `textures/` subfolder shows its textures right away. Subfolders are listed above the images.
- Thumbnail grid with names; hover shows the full path and size. Only visible rows are drawn, so a big folder stays fast.
- The search box filters by file name in the current folder and everything below it.
- Dragging an image from the asset browser onto the field still works. Changes go through undo/redo.

Files: `src/editor/pages/parts/assets/textureEditor.h/.cpp`, `matInstanceEditor.cpp`, `modelEditor.cpp`.

### Build start
Cause of the silent minute: a build creates its own copy of the project on a worker thread, and that copy parsed every model before the first log line.
- Models now load on demand in builds too. Only models whose `.t3dm` is out of date are parsed. The asset table gets each model's scale from `AssetManager::getModelScale()`, which reads only the glTF header data (same rule as a full load: manual override, else auto scale).
- The build copy no longer prepares image previews (nothing is drawn from it).
- Log feedback: "Build started, loading project..." right away, "Project loaded (N ms)", a line per converted model ("Converting model 12/151: charizard.glb"), and a summary ("3D models: 1 converted, 150 up to date").

Files: `src/project/assetManager.h/.cpp`, `src/build/projectBuilder.cpp`, `src/build/t3dmBuilder.cpp`.

### Test checklist
- [ ] Charizard's Material Instance: picker opens in Charizard's folder showing its textures; picking t05 updates the preview; Ctrl+Z undoes it.
- [ ] Up/breadcrumbs/search work; dragging a PNG from the asset browser onto the field works.
- [ ] Build with nothing changed: log starts immediately, "Project loaded" is a few seconds at most, "0 converted".
- [ ] Edit one .glb, build: only that model converts.
- [ ] ROM runs in ares with models at the correct scale (compare against a build from before this change).

## 2026-09-24 (3): Empty texture slots stop the build

Found while testing: Charizard (Battle scene) had Material Instance slot #1 set to "Texture" mode with no texture picked. Pyrite wrote that as an invalid asset index, and the ROM crashed on scene load (`NULL pointer dereference` in `Placeholder::update` / `rdpq_sprite_upload`).
- The build now stops with an error naming the object, slot and scene instead of producing a ROM that crashes. `src/project/component/shared/materialInstance.cpp`
- The Material Instance panel shows a red "No texture set" line under any such slot. `src/editor/pages/parts/assets/matInstanceEditor.cpp`
