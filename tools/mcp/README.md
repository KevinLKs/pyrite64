# Pyrite64 MCP server

Lets an MCP client (Claude Desktop, Claude Code and others) look at and edit the scene that is open in a running Pyrite64 editor: list objects and assets, place models and prefabs, move things around, add components, save and build.

## How it works

1. The editor starts a small bridge on `127.0.0.1:6464` (local machine only). It takes editor-script JavaScript, runs it on the editor's main thread and sends back the result.
2. `pyrite_mcp.py` is the MCP server. Each tool turns into a short editor script and sends it over the bridge.

Every change goes through the editor's undo history, so anything the assistant does can be undone with Ctrl+Z or the `pyrite_undo` tool.

## Setup

1. Build the editor (the bridge is part of it). The log shows `MCP bridge listening on 127.0.0.1:6464` once it is running.
2. Install the Python package (Python 3.10 or newer):

   ```
   pip install -r tools/mcp/requirements.txt
   ```

3. Add the server to your MCP client. For Claude Desktop, edit `claude_desktop_config.json`:

   ```json
   {
     "mcpServers": {
       "pyrite64": {
         "command": "python",
         "args": ["E:\\path\\to\\pyrite64\\tools\\mcp\\pyrite_mcp.py"]
       }
     }
   }
   ```

   Use the full path to the `python.exe` that has `mcp` installed if `python` on your PATH is a different one.

4. Restart the client, open a project in the editor, and ask it something like "what's in the open scene?".

## Settings

| Env var | Where | Meaning |
|---|---|---|
| `PYRITE_MCP_PORT` | editor and MCP server | Bridge port, default `6464`. Set to `0` in the editor's environment to turn the bridge off. Both sides must use the same port. |

Only one editor can own the port. A second editor instance logs a warning and runs without the bridge.

## Tools

| Tool | What it does |
|---|---|
| `pyrite_status` | Open scene, scene list, selection, whether a build is running |
| `pyrite_scene_tree` | Object hierarchy with transforms and components |
| `pyrite_find_objects` | Find objects by name |
| `pyrite_get_object` | Full object JSON including component settings |
| `pyrite_list_assets` / `pyrite_list_components` | What can be placed or added |
| `pyrite_add_object` / `pyrite_add_model` / `pyrite_add_prefab` | Create objects |
| `pyrite_set_transform` / `pyrite_reparent` / `pyrite_delete_objects` | Edit objects |
| `pyrite_add_component` / `pyrite_select` | Components, editor selection |
| `pyrite_load_scene` / `pyrite_save` / `pyrite_build` / `pyrite_get_log` / `pyrite_undo` | Project actions |
| `pyrite_run_js` | Run any editor script, for batch or procedural work the other tools don't cover |

The JavaScript API available to `pyrite_run_js` and to editor scripts is listed at the top of `pyrite_mcp.py` (`JS_API`). The scene, asset and component functions come from `src/editor/script/impl/sceneApi.cpp`.

## Limits

* Scripts run on the editor's main thread. A script with an endless loop freezes the editor.
* Requests time out after 30 seconds if the editor is busy (for example a modal dialog is open).
* Component settings can be read with `pyrite_get_object`, but there is no setter per component field yet. To change them, read the object JSON, edit it, and recreate the object with `scene.addJSON`.
