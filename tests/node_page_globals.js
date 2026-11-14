/*
  The two page globals the library reads, supplied to the Node suite only.

  A real page provides window.location.search (the ?mcp=HOST:PORT dial) and
  window.close() (the tab closing on shutdown), and src/Api.cpp reads exactly
  those two browser names. Under Node nothing provides them.

  This file is passed to the test executable as --post-js and must stay post-js:
  a window that exists at module load flips Emscripten's ENVIRONMENT_IS_WEB and
  trips the -sENVIRONMENT=node assert. It deliberately defines nothing else -
  SDL's dummy video driver supplies the window, SDL supplies the event queue,
  and the real archive supplies the protocol.
*/

globalThis.window = {
   location: {
      search: '?mcp=127.0.0.1:' + (process.env.SDL3MCP_TEST_WS_PORT || '0'),
   },
   close() {
      globalThis.__sdl3mcpPageCloseCalls =
          (globalThis.__sdl3mcpPageCloseCalls || 0) + 1;
   },
};
