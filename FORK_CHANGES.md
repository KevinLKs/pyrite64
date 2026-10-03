# Pyrite64 Fork: Change Notes

Last updated 2026-10-02.

## Overview

This fork makes the Pyrite64 editor stay responsive on projects with thousands of asset files, and it stops one class of crashing ROMs at build time. It changes 17 source files (about 1,200 lines added, 200 removed). Nothing in the N64 runtime (`n64/engine/`) changed, so ROM output and runtime behavior are the same as upstream.

If your project has a few dozen assets, you will mostly notice the texture picker and the build error. The speedups matter once a project holds hundreds of models and thousands of images.

| Item | Value |
| --- | --- |
| Upstream base | `ce75156` "preview transforms, honor depth setting in object (#327)", 2026-09-12 |
| Fork commits | `9602da5` "Performance Boost" and `dee4867` "Project Texture Assignment", both 2026-09-24 |
| Platforms | Windows and Linux get the new file watcher. macOS falls back to polling (still faster than upstream). |
| Testing | Built and used on Windows. The test checklist at the end of this file has not been run formally. |

The changes fall into six groups, covered in the sections below: asset watching, lazy loading, a rescan button, a texture picker, a faster build start, and a build check for empty texture slots.

## 1. Asset watching

The editor now learns about changed files from the operating system instead of re-checking every file every 2 seconds. This removes a periodic UI hitch that grows with project size.

**The problem upstream.** `AssetManager::pollWatch()` ran on the UI thread every 2 seconds. Each time, it walked all of `assets/` and `src/user/` and read the modification time of every file. With about 4,600 files that was a visible freeze every 2 seconds. Adding many files at once was also quadratic: each new file searched every existing entry twice (`removeEntryByPath()` and `getByPath()`).

| File | Change | Why |
| --- | --- | --- |
| `src/utils/fileWatcher.h`, `.cpp` (new) | `Utils::FileWatcher`, a recursive directory watcher. Windows: one thread per root using overlapped `ReadDirectoryChangesW`. Linux: one `inotify` thread, adding watches for new subfolders as they appear. The main thread drains results with `takeSettled()`. | The OS reports exactly which paths changed, so the editor does no work while nothing changes. |
| `src/utils/fileWatcher.cpp` | A path is handed out only after it has been quiet for 300 ms. | Large files being copied fire many events. Waiting avoids reading a half-written PNG or `.glb`. |
| `src/utils/fileWatcher.cpp` | If the OS drops events (`ERROR_NOTIFY_ENUM_DIR` on Windows, `IN_Q_OVERFLOW` on Linux), an overflow flag is set. | Some changes are then unknown, so the caller has to do one full rescan to stay correct. |
| `CMakeLists.txt` | Adds the two new files to the `pyrite64` target. | Needed to compile them. |
| `src/project/assetManager.h`, `.cpp` | `pollWatch()` now drains the watcher and passes only the reported paths to a new `applyChanges(upserts, removes)`. On overflow it runs `syncWithDisk()` once. | Work is now proportional to what changed, not to project size. |
| `src/project/assetManager.cpp` | Folder events are expanded: when a folder is moved in, moved out or deleted, its contents are listed and compared against the known files. | Windows and Linux report only the folder itself in those cases, not each file inside it. |
| `src/project/assetManager.cpp` | The watcher starts before the initial scan. | A file that changes during the scan is still caught. It may be refreshed twice, which is harmless. |
| `src/project/assetManager.h`, `.cpp` | New `pathIndex` hash map from a normalized path key to the entry. `getByPath()` is now a single lookup. `applyChanges()` removes all replaced entries in one pass per type, then re-sorts and rebuilds the index once. | Removes the quadratic cost when hundreds of files arrive at once. |
| `src/project/assetManager.cpp` | New `pathKey()` normalizes separators and `.`/`..`. | The OS, the directory scan and stored entries spell paths differently, which would break lookups. |
| `src/project/assetManager.cpp` | Fallback: if there is no backend (macOS) or the watcher fails to start, the old 2 s poll runs. It now uses the modification time from the directory listing instead of opening each file. | Keeps behavior correct everywhere. On Windows the listing already carries the time, so polling is also cheaper than before. |
| `src/project/assetManager.cpp` | Creating a script or node graph adds just that file through `applyChanges()` instead of calling `reload()`. | A full reload rescanned and reset the whole project for one new file. |
| `src/project/assetManager.cpp` | A changed model that was already loaded is reloaded right away, and its thumbnail is invalidated. Images and prefabs are reloaded as before. | Keeps the viewport and asset browser current. |

**What you will notice:** no periodic stutter while idle, and a file dropped into `assets/` appears in the browser within about half a second.

## 2. Lazy model loading and faster project open

Opening a project no longer parses every 3D model or decodes every PNG. Each model is parsed the first time something actually needs it.

**The problem upstream.** On open, the asset manager fully parsed every `.glb` (meshes, materials, animations). It also fully decoded every PNG just to learn its width and height. Open time grew with every model added, even ones no open scene used.

| File | Change | Why |
| --- | --- | --- |
| `src/project/assetManager.h` | `AssetManagerEntry` gains `loaded` and `scaleKnown` flags. | Tracks which models have been parsed and which have a known import scale. |
| `src/project/assetManager.cpp` | The loop that parsed every model at the end of `reload()` is removed. | This was the main open-time cost. |
| `src/project/assetManager.h` | `getEntryByUUID()` calls a new `ensureLoaded()` when it returns an unloaded model. | Every existing caller (scene view, inspector, thumbnails, build) gets a loaded model without code changes. |
| `src/project/assetManager.cpp` | `loaded` is set before parsing starts. | A model that fails to parse is not retried on every access. |
| `src/project/assetManager.h`, `.cpp` | New `getModelScale(uuid)`. It returns the manual scale override if set, otherwise runs the auto-scale calculation on the glTF without parsing meshes or animations. | Some callers need only the scale. Used by the build (section 5). |
| `src/project/project.h`, `.cpp` | `Project` takes a new `editorMode` flag (default `false`) and passes it to the asset manager. | Only the editor starts the file watcher and prepares image previews. Builds create their own `Project` and need neither. |
| `src/editor/globalActions.cpp` | The editor opens projects with `Project(path, true)`. | Turns on editor mode for the editor only. |
| `src/project/assetManager.cpp` | Image previews are prepared only when there is a window and editor mode is on. | The build's copy of the project never draws anything. |
| `src/renderer/texture.cpp` | New `readPngSize()` reads width and height from the PNG header (first 24 bytes: signature plus `IHDR`). It falls back to a full decode for non-PNG files or a malformed header. | A project open no longer decodes every image. Pixels are still read on first GPU use, as before. |

**Trade-off:** the first time you open a scene or select a model that has not been loaded, the editor pauses briefly while it parses. Saved thumbnails on disk do not trigger a load.

## 3. Rescan All Assets button

Project > Settings has a new **Assets** section with a **Rescan All Assets** button. It runs the same full reload as the existing reload hotkey (the `ASSETS_RELOAD` action).

| File | Change | Why |
| --- | --- | --- |
| `src/editor/pages/parts/projectSettings.cpp` | Adds the Assets section, a short note that changes are picked up automatically, and the button. It logs "Full asset rescan done". | With automatic watching, a manual escape hatch is useful when something looks out of date, for example after a network drive or sync tool missed an event. A visible button is easier to find than a hotkey. |
| `src/editor/pages/parts/projectSettings.cpp` | The button is disabled while assets have unsaved changes, with a warning: "Save first, a rescan discards unsaved asset changes". | A full reload rebuilds every entry from disk and would silently throw those edits away. |

## 4. Texture picker

The Texture field in Material Instance and the Model Editor now opens a folder browser with thumbnails, instead of one flat alphabetical list of every image in the project.

**The problem upstream.** With hundreds of images, the flat list was slow to scroll and gave no hint which textures belonged to the model being edited.

| File | Change | Why |
| --- | --- | --- |
| `src/editor/pages/parts/assets/textureEditor.h`, `.cpp` | New `drawTexturePicker(label, texUUID, startDir)`. The field looks like a combo box and opens a popup. | Reusable anywhere a texture is chosen. |
| `textureEditor.cpp` | The popup opens in the folder of the model being edited (the `.glb`'s folder). A home button returns there. An up arrow and clickable breadcrumb segments navigate anywhere under `assets/`. | Most models keep their textures next to them or one level down. |
| `textureEditor.cpp` | "Include subfolders" (on by default) lists every image below the current folder. Subfolders are listed above the images. | A model with a `textures/` subfolder shows its textures right away. |
| `textureEditor.cpp` | Thumbnail grid with names. Hover shows the full path and size. Only visible rows are drawn, using `ImGuiListClipper`, so only those textures get uploaded to the GPU. | Keeps a folder with thousands of images fast. |
| `textureEditor.cpp` | A search box filters by file name in the current folder and below. | Quick lookup by name. |
| `textureEditor.cpp` | Dragging an image from the asset browser onto the field still works. Every change goes through undo/redo. | Keeps upstream behavior. |
| `textureEditor.h`, `.cpp` | `TextureEditor::draw()` takes an optional `modelUUID` (default 0, which opens in `assets/`). | Tells the picker where to start. Existing callers compile unchanged. |
| `src/editor/pages/parts/assets/matInstanceEditor.cpp` | Passes the model's UUID to `TextureEditor::draw()`. | Material Instance slots open in the model's folder. |
| `src/editor/pages/parts/assets/modelEditor.cpp` | Passes the asset's UUID to `TextureEditor::draw()`. | Same for the Model Editor. |

## 5. Faster build start

A build with nothing changed now starts logging immediately and parses only models whose `.t3dm` output is out of date. Upstream spent up to a minute silently parsing every model before the first log line.

**The problem upstream.** A build creates its own `Project` on a worker thread. That copy parsed every model during load, and `buildT3DMAssets()` assumed every model was already loaded. The asset table also read each model's scale from the parsed model.

| File | Change | Why |
| --- | --- | --- |
| `src/build/t3dmBuilder.cpp` | `buildT3DMAssets()` checks `assetBuildNeeded()` first, then loads only that model with `getEntryByUUID()`. Up-to-date models are never parsed. | This is what removes the silent wait. |
| `src/build/t3dmBuilder.cpp` | If a model cannot be found when its turn comes, the build stops with "Model asset vanished during build: path". | Avoids a null dereference if a file is deleted mid-build. |
| `src/build/t3dmBuilder.cpp` | Logs "Converting model N/total: name" per conversion and a summary "3D models: X converted, Y up to date". | Upstream gave no feedback during this step. |
| `src/build/projectBuilder.cpp` | `SceneCtx::addAsset()` gets each model's vertex scale from `getModelScale()` instead of the parsed model. | The asset table needs the scale for every model, including ones that were not converted. This reads only glTF header data, using the same rule as a full load (manual override, else auto scale). |
| `src/build/projectBuilder.cpp` | Logs "Build started, loading project..." right away, then "Project loaded (N ms)". | Shows the build is alive and how long the load took. |
| `src/project/assetManager.cpp` | Builds open the project without editor mode (section 2): no file watcher threads, no image previews. | A build copy draws nothing and must not leave watcher threads running. |

The material writer, placeholder limit, compression, collision building and `.sdata` handling in `t3dmBuilder.cpp` are unchanged from upstream.

## 6. Empty texture slots stop the build

A Material Instance slot set to "Texture" with no texture picked now fails the build with a clear message. Upstream built a ROM that crashed on scene load.

**The problem upstream.** Such a slot was written to the scene file as an invalid asset index. At runtime this was a null sprite, and the ROM crashed with a `NULL pointer dereference` in `Placeholder::update` / `rdpq_sprite_upload` as soon as the scene loaded. Nothing in the editor pointed at the cause.

| File | Change | Why |
| --- | --- | --- |
| `src/project/component/shared/materialInstance.cpp` | Before writing texture slots, `MaterialInstance::build()` checks every set slot in Texture mode. If its texture is missing from the build's asset table, it throws with the slot number, the object name, and the scene name (or "(Prefab)"). It distinguishes "has no texture set" from "uses a texture that no longer exists". | Turns a runtime crash into a build error that names exactly what to fix. The second case also catches a texture deleted after it was assigned. |
| `src/editor/pages/parts/assets/matInstanceEditor.cpp` | Shows a red line under any Texture-mode slot with no valid texture: "No texture set, the build will stop here". | You see the problem in the inspector before you build. |

**Behavior change to be aware of:** a project that built without errors on upstream, but had an empty Texture slot somewhere, will now fail to build until you pick a texture or switch the slot's mode. That ROM would have crashed when it loaded the affected scene, so this only surfaces an existing bug.

## Known limits, compatibility and how to verify

Project files written by this fork are the same format as upstream's, so a project can move between the two editors. The fork does not change the scene or prefab format, file migration, or the N64 engine.

**Known limits**

- **First use of a model:** opening a scene or selecting a model that has not been loaded yet causes a short pause while it parses.
- **Bulk drops:** dropping thousands of files at once still costs one pause when they are processed. That pause scales with the new files only, not the whole project.
- **Linux watch limit:** large trees can hit `fs.inotify.max_user_watches`. The watcher logs this, and the editor falls back to full rescans. Raise the limit with `sysctl` if you see the message.
- **macOS:** there is no native backend yet, so macOS uses the improved 2 s poll.
- **Network drives and sync tools:** some do not deliver change notifications reliably. Use Rescan All Assets if the browser looks stale.
- **Line endings:** the fork commits were saved with Windows (CRLF) line endings, so a plain `git diff` against upstream shows whole files as changed. Use `git diff --ignore-cr-at-eol ce75156` to see the real changes.
- **Headers:** the two new `fileWatcher` files carry the upstream copyright header (MIT), matching the rest of the codebase.

**How to verify on your own project.** None of these have been run as a formal pass yet.

- [ ] Idle editor on a large project: no periodic hitch.
- [ ] Copy one PNG into `assets/`: it appears in the browser within about half a second.
- [ ] Copy a folder in, move a folder in (same drive), delete a folder: the browser updates each time.
- [ ] Edit a texture used by an open model: the viewport updates.
- [ ] Compare project open time with upstream on the same project.
- [ ] Rescan All Assets works, and is disabled while there are unsaved asset changes.
- [ ] Texture picker: opens in the model's folder, picking a texture updates the preview, Ctrl+Z undoes it.
- [ ] Picker up arrow, breadcrumbs, search and drag-and-drop from the asset browser all work.
- [ ] Build with nothing changed: logging starts immediately, "Project loaded" takes a few seconds at most, and the summary reads "0 converted".
- [ ] Edit one `.glb` and build: only that model converts.
- [ ] The ROM runs with models at the same scale as an upstream build.
- [ ] Set a Material Instance slot to Texture with nothing picked: the inspector shows the red warning and the build stops with the object, slot and scene named.
