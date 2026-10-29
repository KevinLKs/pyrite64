"""
Pyrite64 MCP server.

Lets an MCP client (Claude Desktop, Claude Code, ...) inspect and edit the scene that is
open in a running Pyrite64 editor. It talks to the editor's built-in bridge
(src/editor/mcpBridge.cpp) on 127.0.0.1, which runs editor scripts (JavaScript) on the
editor's main thread.

Run:   python pyrite_mcp.py          (stdio transport, what Claude Desktop expects)
Env:   PYRITE_MCP_PORT  bridge port, default 6464 (must match the editor)
"""

from __future__ import annotations

import json
import math
import os
import socket
import threading
import time
from typing import Any

try:  # mcp >= 2
    from mcp.server.mcpserver import MCPServer as _Server
    from mcp.server.mcpserver.exceptions import ToolError
except ImportError:  # mcp 1.x
    from mcp.server.fastmcp import FastMCP as _Server
    from mcp.server.fastmcp.exceptions import ToolError

HOST = "127.0.0.1"
PORT = int(os.environ.get("PYRITE_MCP_PORT", "6464"))

JS_API = """
Editor script globals (JavaScript, QuickJS):
  scene.list() -> [{id,name}]            scene.current() -> {id,name,objectCount} | null
  scene.load(id)                          scene.objects(rootUUID?, maxDepth?) -> tree summary
  scene.find(text) -> [SceneObject]       scene.get(uuid) -> SceneObject | null
  scene.create(name, parentUUID?)         scene.addModel(assetUUID) / scene.addPrefab(assetUUID)
  scene.addJSON(objJson, parentUUID?)     scene.serialize(uuid) -> full object JSON incl. components
  scene.remove(uuid)                      scene.move(uuid, targetUUID (0 = root), asChild = true)
  scene.select([uuids])                   scene.setEnabled(uuid, bool)
  scene.addComponent(uuid, idOrName) -> componentUUID   scene.removeComponent(uuid, componentUUID)
  assets.list(type?) type: image|audio|font|model|code_object|code_global|prefab|node_graph|music|editor_script
  assets.get(assetUUID) -> {uuid,name,path,type,animations?}
  components.list() -> [{id,name}]
  editor.save() editor.build(run?) editor.isBuilding() editor.reloadAssets() editor.getSelObjects()
  history.markChanged(desc) history.undo() history.redo()
  console.log(text) console.read(maxChars?)
SceneObject: .uuid .name .pos {x,y,z} .rot quaternion {x,y,z,w} .scale {x,y,z} (assignable)
Asset UUIDs are strings (64-bit). Object UUIDs are numbers.
Each call runs in its own block scope; use globalThis.name = ... to keep helpers between calls.
After changing the scene call history.markChanged("what changed") so it can be undone.
The value of the last expression is returned (objects as JSON).
""".strip()

mcp = _Server(
    "pyrite64",
    instructions=(
        "Tools for the Pyrite64 N64 game editor. The editor must be running with a project open. "
        "Prefer the specific tools; use pyrite_run_js for anything they don't cover.\n\n" + JS_API
    ),
)


# ---------------------------------------------------------------- bridge connection

class BridgeError(ToolError):
    """Shown to the model as-is (script errors, editor not running)."""


class Bridge:
    def __init__(self, host: str, port: int):
        self.host, self.port = host, port
        self.sock: socket.socket | None = None
        self.buf = b""
        self.next_id = 1
        self.lock = threading.Lock()

    def _connect(self) -> None:
        try:
            self.sock = socket.create_connection((self.host, self.port), timeout=5)
        except OSError as e:
            self.sock = None
            raise BridgeError(
                f"Can't reach the Pyrite64 editor on {self.host}:{self.port}. "
                "Is the editor running (a build with the MCP bridge)?"
            ) from e
        self.sock.settimeout(60)
        self.buf = b""

    def _roundtrip(self, payload: dict) -> dict:
        if self.sock is None:
            self._connect()
        assert self.sock is not None
        self.sock.sendall((json.dumps(payload) + "\n").encode("utf-8"))
        while b"\n" not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("editor closed the connection")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return json.loads(line.decode("utf-8"))

    def request(self, payload: dict) -> dict:
        with self.lock:
            payload = dict(payload, id=self.next_id)
            self.next_id += 1
            try:
                return self._roundtrip(payload)
            except (OSError, ConnectionError):
                # editor restarted or connection dropped, retry once on a fresh socket
                self.close()
                try:
                    return self._roundtrip(payload)
                except (OSError, ConnectionError) as e:
                    self.close()
                    raise BridgeError(f"Lost connection to the editor: {e}") from e

    def close(self) -> None:
        if self.sock is not None:
            try:
                self.sock.close()
            except OSError:
                pass
        self.sock = None


bridge = Bridge(HOST, PORT)


def js(code: str) -> Any:
    """Runs JS in the editor. Returns parsed JSON when possible, raises on script errors.
    The code is wrapped in a block so const/let names don't leak into the editor's global
    scope between calls (the block still yields the last expression's value)."""
    res = bridge.request({"code": "{\n" + code + "\n}"})
    text = res.get("result", "")
    if not res.get("ok"):
        raise BridgeError(text or "script failed")
    try:
        return json.loads(text)
    except (TypeError, ValueError):
        return text


def q(value: Any) -> str:
    """Encodes a Python value as a JS literal."""
    return json.dumps(value)


def euler_to_quat(deg: list[float]) -> dict:
    """XYZ euler angles in degrees -> quaternion {x,y,z,w} (rotation applied X, then Y, then Z)."""
    rx, ry, rz = (math.radians(a) / 2 for a in deg)
    cx, sx = math.cos(rx), math.sin(rx)
    cy, sy = math.cos(ry), math.sin(ry)
    cz, sz = math.cos(rz), math.sin(rz)
    return {
        "x": sx * cy * cz - cx * sy * sz,
        "y": cx * sy * cz + sx * cy * sz,
        "z": cx * cy * sz - sx * sy * cz,
        "w": cx * cy * cz + sx * sy * sz,
    }


def vec(v: list[float] | None) -> dict | None:
    if v is None:
        return None
    if len(v) != 3:
        raise BridgeError("expected [x, y, z]")
    return {"x": v[0], "y": v[1], "z": v[2]}


def transform_js(var: str, pos, rot_euler_deg, rot_quat, scale) -> str:
    parts = []
    if pos is not None:
        parts.append(f"{var}.pos = {q(vec(pos))};")
    if rot_quat is not None:
        if len(rot_quat) != 4:
            raise BridgeError("rot_quat must be [x, y, z, w]")
        parts.append(f"{var}.rot = {q(dict(zip('xyzw', rot_quat)))};")
    elif rot_euler_deg is not None:
        if len(rot_euler_deg) != 3:
            raise BridgeError("rot_euler_deg must be [x, y, z]")
        parts.append(f"{var}.rot = {q(euler_to_quat(rot_euler_deg))};")
    if scale is not None:
        if isinstance(scale, (int, float)):
            scale = [scale, scale, scale]
        parts.append(f"{var}.scale = {q(vec(scale))};")
    return "\n".join(parts)


# JS helper: resolves an asset by uuid, exact name, file name, or project path
FIND_ASSET = """
function __findAsset(key, type) {
  const list = assets.list(type);
  const k = String(key).toLowerCase();
  const hit = list.find(a => a.uuid === String(key))
    || list.find(a => a.name.toLowerCase() === k || a.path.toLowerCase() === k)
    || list.find(a => a.name.toLowerCase().replace(/\\.[^.]+$/, '') === k);
  if (!hit) throw new Error(type + " asset not found: " + key);
  return hit;
}
"""


def obj_result(var: str = "o") -> str:
    return f"({{uuid: {var}.uuid, name: {var}.name, pos: {var}.pos, rot: {var}.rot, scale: {var}.scale}})"


# ---------------------------------------------------------------- tools

@mcp.tool()
def pyrite_status() -> dict:
    """Checks the editor connection and returns the open scene and basic project info."""
    return js("({scene: scene.current(), scenes: scene.list(), building: editor.isBuilding(),"
              " selected: editor.getSelObjects().map(o => o.uuid)})")


def pyrite_run_js(code: str) -> Any:
    return js(code)


pyrite_run_js.__doc__ = (
    "Runs arbitrary editor-script JavaScript in the open editor and returns the value of the "
    "last expression. Use for anything the other tools don't cover (batch edits, procedural "
    "layout, editing component settings). Call history.markChanged(\"...\") after scene changes "
    "so they can be undone.\n\n" + JS_API
)
pyrite_run_js = mcp.tool()(pyrite_run_js)


@mcp.tool()
def pyrite_load_scene(scene_id: int) -> Any:
    """Opens a scene by id (see pyrite_status for the list). Unsaved changes in the current scene
    may be lost, so call pyrite_save first if needed."""
    return js(f"scene.load({int(scene_id)}); scene.current()")


@mcp.tool()
def pyrite_scene_tree(root_uuid: int | None = None, max_depth: int = 3) -> Any:
    """Returns the object hierarchy of the open scene (uuid, name, transform, components, children).
    Use root_uuid to look at one branch; deeper levels are summarised as childCount."""
    return js(f"scene.objects({q(root_uuid)}, {int(max_depth)})")


@mcp.tool()
def pyrite_find_objects(name_contains: str) -> Any:
    """Finds objects whose name contains the text (case-insensitive)."""
    return js(f"scene.find({q(name_contains)}).map(o => ({{uuid: o.uuid, name: o.name, pos: o.pos}}))")


@mcp.tool()
def pyrite_get_object(uuid: int) -> Any:
    """Returns the full serialized JSON of one object, including all component settings.
    The same format can be passed to pyrite_run_js via scene.addJSON(json, parentUUID)."""
    return js(f"scene.serialize({int(uuid)})")


@mcp.tool()
def pyrite_list_assets(asset_type: str | None = None, name_contains: str | None = None) -> Any:
    """Lists project assets. asset_type: image, audio, font, model, code_object, code_global,
    prefab, node_graph, music, editor_script. Asset UUIDs are strings."""
    code = f"assets.list({q(asset_type)})"
    if name_contains:
        code += f".filter(a => (a.name + ' ' + a.path).toLowerCase().includes({q(name_contains.lower())}))"
    return js(code)


@mcp.tool()
def pyrite_list_components() -> Any:
    """Lists the component types that can be added to objects."""
    return js("components.list()")


@mcp.tool()
def pyrite_add_object(
    name: str,
    parent_uuid: int | None = None,
    pos: list[float] | None = None,
    rot_euler_deg: list[float] | None = None,
    scale: list[float] | float | None = None,
    components: list[str] | None = None,
) -> Any:
    """Creates an empty object (optionally under a parent, with components like "Light", "Camera",
    "Collider", "Code"). Good for grouping objects or as an empty transform."""
    code = f"const o = scene.create({q(name)}, {q(parent_uuid)});\n"
    code += transform_js("o", pos, rot_euler_deg, None, scale) + "\n"
    for comp in components or []:
        code += f"scene.addComponent(o.uuid, {q(comp)});\n"
    code += f"history.markChanged({q('Add ' + name)});\n" + obj_result()
    return js(code)


def _place(kind: str, asset: str, name, parent_uuid, pos, rot_euler_deg, scale) -> Any:
    asset_type = "model" if kind == "addModel" else "prefab"
    code = FIND_ASSET + f"const a = __findAsset({q(asset)}, {q(asset_type)});\n"
    code += f"const o = scene.{kind}(a.uuid);\n"
    if name:
        code += f"o.name = {q(name)};\n"
    if parent_uuid:
        code += f"scene.move(o.uuid, {int(parent_uuid)}, true);\n"
    code += transform_js("o", pos, rot_euler_deg, None, scale) + "\n"
    code += f"history.markChanged('Add ' + o.name);\n" + obj_result()
    return js(code)


@mcp.tool()
def pyrite_add_model(
    asset: str,
    name: str | None = None,
    parent_uuid: int | None = None,
    pos: list[float] | None = None,
    rot_euler_deg: list[float] | None = None,
    scale: list[float] | float | None = None,
) -> Any:
    """Places a 3D model asset in the scene (same as dragging it in). asset can be the asset UUID,
    file name (with or without extension) or project path. Animated models get the animated
    model component automatically."""
    return _place("addModel", asset, name, parent_uuid, pos, rot_euler_deg, scale)


@mcp.tool()
def pyrite_add_prefab(
    asset: str,
    name: str | None = None,
    parent_uuid: int | None = None,
    pos: list[float] | None = None,
    rot_euler_deg: list[float] | None = None,
    scale: list[float] | float | None = None,
) -> Any:
    """Places an instance of a prefab asset (by UUID, name or path)."""
    return _place("addPrefab", asset, name, parent_uuid, pos, rot_euler_deg, scale)


@mcp.tool()
def pyrite_set_transform(
    uuid: int,
    pos: list[float] | None = None,
    rot_euler_deg: list[float] | None = None,
    rot_quat: list[float] | None = None,
    scale: list[float] | float | None = None,
    name: str | None = None,
) -> Any:
    """Sets position, rotation (euler degrees XYZ, or quaternion [x,y,z,w]), scale and/or name
    of an object. Omitted values stay unchanged."""
    code = f"const o = scene.get({int(uuid)}); if (!o) throw new Error('object not found');\n"
    if name is not None:
        code += f"o.name = {q(name)};\n"
    code += transform_js("o", pos, rot_euler_deg, rot_quat, scale) + "\n"
    code += "history.markChanged('Transform ' + o.name);\n" + obj_result()
    return js(code)


@mcp.tool()
def pyrite_reparent(uuid: int, parent_uuid: int = 0) -> Any:
    """Moves an object under another object (parent_uuid 0 = scene root)."""
    return js(f"const ok = scene.move({int(uuid)}, {int(parent_uuid)}, true);"
              f" if (ok) history.markChanged('Move Object'); ok")


@mcp.tool()
def pyrite_delete_objects(uuids: list[int]) -> Any:
    """Deletes objects (and their children). Undoable with pyrite_undo."""
    code = f"const r = {q(uuids)}.map(u => scene.remove(u));\n"
    code += "history.markChanged('Delete Object');\nr"
    return js(code)


@mcp.tool()
def pyrite_add_component(uuid: int, component: str) -> Any:
    """Adds a component to an object by name ("Light", "Model (Static)", "Collider", ...) or id.
    Returns the new component's UUID. Configure it afterwards via pyrite_get_object and
    pyrite_run_js if needed."""
    return js(f"const c = scene.addComponent({int(uuid)}, {q(component)});"
              f" history.markChanged('Add Component'); ({{componentUUID: c}})")


@mcp.tool()
def pyrite_select(uuids: list[int]) -> Any:
    """Selects objects in the editor so the user can see what you're referring to."""
    return js(f"scene.select({q(uuids)})")


@mcp.tool()
def pyrite_undo(steps: int = 1) -> Any:
    """Undoes the last change(s) in the editor."""
    return js(f"let n = 0; for (let i = 0; i < {int(steps)}; ++i) if (history.undo()) ++n; n")


@mcp.tool()
def pyrite_save() -> Any:
    """Saves the project and the open scene to disk."""
    js("editor.save()")
    return "saved"


@mcp.tool()
def pyrite_get_log(max_chars: int = 4000) -> Any:
    """Returns the end of the editor log (build output, script errors, warnings)."""
    return js(f"console.read({int(max_chars)})")


BUILD_MARKER = "[mcp] build requested"


def _log_since_marker(max_chars: int = 6000) -> str:
    log = js("console.read(200000)")
    log = log if isinstance(log, str) else json.dumps(log)
    idx = log.rfind(BUILD_MARKER)
    if idx >= 0:
        log = log[idx + len(BUILD_MARKER):]
    return log[-max_chars:].strip()


@mcp.tool()
def pyrite_build(run: bool = False, timeout_s: int = 600) -> Any:
    """Saves, builds the ROM and waits for it to finish, then returns the build output.
    run=True also launches it in the configured emulator (returns once the log goes quiet,
    since the editor counts a running emulator as still busy)."""
    js("editor.save()")
    started = js(f"console.log({q(BUILD_MARKER)}); editor.build({q(run)})")
    if started is False:
        return {"started": False, "log": _log_since_marker()}

    deadline = time.time() + timeout_s
    last_len, quiet_since = -1, time.time()
    time.sleep(0.5)
    while time.time() < deadline:
        if not js("editor.isBuilding()"):
            return {"finished": True, "log": _log_since_marker()}
        if run:
            cur_len = len(js("console.read(200000)"))
            if cur_len != last_len:
                last_len, quiet_since = cur_len, time.time()
            elif time.time() - quiet_since > 8:
                return {"finished": True, "running": True, "log": _log_since_marker()}
        time.sleep(1.0)
    return {"finished": False, "log": _log_since_marker()}


def main() -> None:
    mcp.run()


if __name__ == "__main__":
    main()
