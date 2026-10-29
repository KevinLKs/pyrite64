/**
* @license MIT
*/
#pragma once

/**
 * Local bridge that lets external tools (e.g. an MCP server for AI assistants)
 * run editor scripts while the editor is open.
 *
 * Listens on 127.0.0.1 only. Protocol: newline-delimited JSON.
 *   request:  {"id": 1, "code": "scene.current()"}
 *             {"id": 2, "op": "ping"}
 *   response: {"id": 1, "ok": true, "result": "<text or JSON string>"}
 *
 * Network I/O runs on a background thread, but every script is evaluated on the
 * main thread inside poll(), so scripts can safely touch the scene and project.
 *
 * Port: 6464 by default, override with env PYRITE_MCP_PORT, set it to 0 to disable.
 */
namespace Editor::McpBridge
{
  void start();
  void stop();

  // Runs queued requests, call once per frame from the main thread.
  void poll();

  [[nodiscard]] bool isListening();
  [[nodiscard]] int getPort();
}
