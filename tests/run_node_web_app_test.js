'use strict';

/*
  Drives the Emscripten app executable over the real WebSocket path under Node.

  The executable is the page half: it selects SDL's dummy video driver in C++,
  creates a real SDL_Window, runs the app loop and writes a synthetic frame
  whenever one is requested. This file is the page's client: it starts a
  WebSocket server on an ephemeral loopback port, hands the port to the child,
  and speaks JSON-RPC to the real archive once the page query dials in. It also
  reads the raw payload that follows a frame header, so the framing is asserted
  byte for byte rather than through a base64 envelope.

  The conversation is the whole app half of the browser path: the page query
  dial, initialize, tools/list, sdl_info with the dummy driver and the window
  fields, two captures whose frame stamps advance, sdl_input whose actions reach
  the app as SDL events with held state, and sdl_terminate with its one quit
  event. The app's own stdout supplies the event and quit evidence.

  What this cannot cover is the browser itself: the Emscripten video driver
  (canvas, WebGL, DOM input), the browser's own WebSocket implementation, a real
  page's query string and window.close() semantics, and the Handler's web stack.
  Those stay with the manual `--web` acceptance run; README §Tests and
  docs/architecture.md §The browser arm say so.

  Node 22 or newer supplies the global WebSocket the page's -lwebsocket.js dial
  uses. The server side is the shared, dependency-free node_websocket_server.js.
*/

const path = require('node:path');
const assert = require('node:assert/strict');
const {spawn} = require('node:child_process');

const {startServer} = require('./node_websocket_server.js');

/* These mirror tests/web_app_node_tests.cpp; a change there changes both. */
const WINDOW_TITLE = 'SDL3MCP node path';
const WINDOW_WIDTH = 64;
const WINDOW_HEIGHT = 48;
const FRAME_WIDTH = 4;
const FRAME_HEIGHT = 3;
const FRAME_BYTES = FRAME_WIDTH * FRAME_HEIGHT * 4;
const SCANCODE_A = 4;
const KEYCODE_A = 97;
const SDL_BUTTON_LEFT = 1;

/* The pattern the app writes: pixel (x, y) is (x, y, 3, 255). */
function framePattern() {
   const pixels = Buffer.alloc(FRAME_BYTES);
   for (let y = 0; y < FRAME_HEIGHT; ++y) {
      for (let x = 0; x < FRAME_WIDTH; ++x) {
         const index = (y * FRAME_WIDTH + x) * 4;
         pixels[index + 0] = x * 40 + 1;
         pixels[index + 1] = y * 40 + 2;
         pixels[index + 2] = 3;
         pixels[index + 3] = 255;
      }
   }
   return pixels;
}

function delay(milliseconds) {
   return new Promise((resolve) => setTimeout(resolve, milliseconds));
}

/*
  The client half of the MCP session. `request` waits for the answer with the
  same id, and when the answer is a raw frame header it reads exactly the
  announced byteLength next, which is the framing a client has to implement.
*/
class McpClient {
   constructor(connection) {
      this.connection = connection;
      this.nextId = 0;
   }

   notify(method) {
      this.connection.sendBinary(
          Buffer.from(JSON.stringify({jsonrpc: '2.0', method}) + '\n'));
   }

   async request(method, params) {
      const id = ++this.nextId;
      const message = {jsonrpc: '2.0', id, method};
      if (params !== undefined) {
         message.params = params;
      }
      this.connection.sendBinary(Buffer.from(JSON.stringify(message) + '\n'));

      const answer = await this.readMessage();
      assert.equal(answer.id, id,
                   `expected the answer to request ${id}, got ${JSON.stringify(answer)}`);
      assert.equal(answer.jsonrpc, '2.0');

      let payload = null;
      if (answer.result && typeof answer.result.byteLength === 'number') {
         payload = await this.connection.nextMessage();
         assert.equal(payload.length, answer.result.byteLength,
                      'the raw payload is not the announced byteLength');
      }
      return {answer, payload};
   }

   callTool(name, args = {}) {
      return this.request('tools/call', {name, arguments: args});
   }

   async readMessage() {
      const bytes = await this.connection.nextMessage();
      const text = bytes.toString('utf8').replace(/\r?\n$/, '');
      try {
         return JSON.parse(text);
      } catch (error) {
         throw new Error(`the app sent a line that is not JSON: ${text}`);
      }
   }
}

function toolDocument({answer}) {
   assert.equal(answer.result.isError, false,
                `the tool call failed: ${JSON.stringify(answer.result)}`);
   assert.equal(answer.result.content[0].type, 'text');
   return JSON.parse(answer.result.content[0].text);
}

/* Checks a raw frame answer and returns its stamp, which must exceed `above`. */
function checkFrame({answer, payload}, above) {
   const header = answer.result;
   assert.equal(header.format, 'rgba8888');
   assert.equal(header.width, FRAME_WIDTH);
   assert.equal(header.height, FRAME_HEIGHT);
   assert.equal(header.byteLength, FRAME_BYTES);
   assert.ok(Number.isInteger(header.frame) && header.frame >= 1,
             `the frame stamp is not a positive integer: ${header.frame}`);
   if (above !== null) {
      assert.ok(header.frame > above,
                `the frame stamp did not advance past ${above}: ${header.frame}`);
   }
   assert.deepEqual(payload, framePattern(),
                    'the raw RGBA payload is not the frame the app wrote');
   return header.frame;
}

async function waitForLine(output, pattern, timeoutMs = 10000) {
   const deadline = Date.now() + timeoutMs;
   for (;;) {
      const line = output().split('\n').find((entry) => pattern.test(entry));
      if (line !== undefined) {
         return line;
      }
      if (Date.now() > deadline) {
         throw new Error(`the app never printed ${pattern}`);
      }
      await delay(20);
   }
}

/* The app's event evidence reaches stdout; output() returns everything so far. */
async function converse(client, output) {
   const initialized = await client.request(
       'initialize', {protocolVersion: '2025-11-25', capabilities: {}});
   assert.equal(initialized.answer.result.protocolVersion, '2025-11-25');
   assert.deepEqual(initialized.answer.result.capabilities.tools, {});
   assert.equal(initialized.answer.result.serverInfo.name, 'sdl3mcp');
   assert.ok(initialized.answer.result.serverInfo.version.length > 0);
   client.notify('notifications/initialized');

   const listed = await client.request('tools/list');
   const names = listed.answer.result.tools.map((tool) => tool.name).sort();
   assert.deepEqual(names,
                    ['sdl_capture', 'sdl_info', 'sdl_input', 'sdl_terminate']);

   /*
     The page dialed the endpoint the shim put in window.location.search, and
     the app bound a real window from the dummy video driver.
   */
   const info = toolDocument(await client.callTool('sdl_info'));
   assert.equal(info.videoDriver, 'dummy');
   assert.equal(info.window.title, WINDOW_TITLE);
   assert.equal(info.window.width, WINDOW_WIDTH);
   assert.equal(info.window.height, WINDOW_HEIGHT);
   assert.ok(info.window.pixelWidth > 0);
   assert.ok(info.window.pixelHeight > 0);
   assert.equal(info.window.pixelDensity, 1);
   assert.equal(info.window.relativeMouseMode, false);
   assert.deepEqual(info.input.heldKeys, []);
   assert.deepEqual(info.input.heldButtons, []);
   assert.equal(info.capture.pending, false);
   assert.equal(info.capture.count, 0);
   assert.equal(info.limits.maxFrameBytes, 32 * 1024 * 1024);
   assert.ok(info.frame >= 1);
   assert.equal(info.appliedFrame, 0);

   /* Two captures: the payload is exact and the stamp advances. */
   const firstFrame = checkFrame(await client.callTool('sdl_capture'), info.frame);
   const afterCapture = toolDocument(await client.callTool('sdl_info'));
   assert.equal(afterCapture.capture.count, 1);
   assert.equal(afterCapture.capture.pending, false);
   assert.equal(afterCapture.capture.dropped, 0);
   assert.equal(afterCapture.capture.lastWidth, FRAME_WIDTH);
   assert.equal(afterCapture.capture.lastHeight, FRAME_HEIGHT);
   assert.equal(afterCapture.capture.lastBytes, FRAME_BYTES);
   assert.equal(afterCapture.capture.lastError, null);

   const secondFrame = checkFrame(await client.callTool('sdl_capture'), firstFrame);
   assert.ok(secondFrame > firstFrame);

   /* A press is held: sdl_info reports both keys, and the app saw SDL events. */
   const press = await client.callTool('sdl_input', {
      actions: [
         {type: 'key', key: 'a', action: 'down'},
         {type: 'mouse_button', button: 'left', action: 'down', x: 10, y: 10},
      ],
   });
   checkFrame(press, secondFrame);
   assert.match(press.answer.result.text,
                /^2 actions applied at frame \d+; frame \d+ attached/);

   const held = toolDocument(await client.callTool('sdl_info'));
   assert.deepEqual(held.input.heldKeys, ['a']);
   assert.deepEqual(held.input.heldButtons, ['left']);

   await waitForLine(output,
                     new RegExp(`^app: key down scancode=${SCANCODE_A} key=${KEYCODE_A} `));
   await waitForLine(output, /^app: mouse motion x=10 y=10$/);
   await waitForLine(output,
                     new RegExp(`^app: mouse button down button=${SDL_BUTTON_LEFT} `));

   /* A release is not held: the same state comes back empty and SDL saw the up. */
   const release = await client.callTool('sdl_input', {
      actions: [
         {type: 'key', key: 'a', action: 'up'},
         {type: 'mouse_button', button: 'left', action: 'up'},
      ],
   });
   checkFrame(release, secondFrame);
   assert.match(release.answer.result.text,
                /^2 actions applied at frame \d+; frame \d+ attached/);

   const released = toolDocument(await client.callTool('sdl_info'));
   assert.deepEqual(released.input.heldKeys, []);
   assert.deepEqual(released.input.heldButtons, []);

   await waitForLine(output,
                     new RegExp(`^app: key up scancode=${SCANCODE_A} key=${KEYCODE_A} `));
   await waitForLine(output,
                     new RegExp(`^app: mouse button up button=${SDL_BUTTON_LEFT} `));

   const terminated = await client.callTool('sdl_terminate');
   assert.equal(terminated.answer.result.content[0].text, 'termination requested');
   assert.equal(terminated.answer.result.isError, false);
   assert.equal(terminated.payload, null);
}

async function main() {
   const executable = process.argv[2];
   if (!executable) {
      console.error('usage: node run_node_web_app_test.js <test.js>');
      process.exit(2);
   }
   if (typeof WebSocket !== 'function') {
      console.error('this Node has no global WebSocket; the suite needs Node 22 or newer');
      process.exit(2);
   }

   let accept;
   const dialed = new Promise((resolve) => {
      accept = resolve;
   });
   const {server, port} = await startServer((connection, request) => {
      accept({connection, request});
   });

   const stdout = [];
   const stderr = [];
   const child = spawn(process.execPath, [path.resolve(executable)], {
      stdio: ['ignore', 'pipe', 'pipe'],
      env: {...process.env, SDL3MCP_TEST_WS_PORT: String(port)},
   });
   child.stdout.on('data', (chunk) => stdout.push(chunk));
   child.stderr.on('data', (chunk) => stderr.push(chunk));
   const exited = new Promise((resolve) => {
      child.on('exit', (exitCode, signal) => {
         resolve(signal === null ? exitCode : 1);
      });
   });

   /* A hung dial must fail the run, not leave CTest waiting forever. */
   const deadline = setTimeout(() => child.kill('SIGKILL'), 120000);
   const output = () => Buffer.concat(stdout).toString('utf8');
   const diagnostics = () => `--- app stdout ---\n${output()}\n--- app stderr ---\n`
       + Buffer.concat(stderr).toString('utf8');

   try {
      const {connection, request} = await Promise.race([
         dialed,
         delay(30000).then(() => {
            throw new Error('the page query did not dial the test server');
         }),
      ]);

      /* The shim's window.location.search names this port, so the dial proves
         the query reached Configuration and became the endpoint. */
      assert.equal(request.headers.host, `127.0.0.1:${port}`,
                   'the dial did not carry the endpoint the page query named');
      assert.equal(request.headers.upgrade, 'websocket');

      await converse(new McpClient(connection), output);
      connection.close(1000);

      const code = await exited;
      assert.equal(code, 0, `the app exited with ${code}\n${diagnostics()}`);

      /* sdl_terminate pushes exactly one quit event, and shutdown closes the page. */
      await waitForLine(output, /^app: quit event$/);
      const quitLines = output().split('\n')
          .filter((line) => line === 'app: quit event');
      assert.equal(quitLines.length, 1,
                   `expected one quit event, got ${quitLines.length}`);

      await waitForLine(output, /^app: page close calls 1$/);
      assert.match(output(), /^app: video driver dummy$/m);
      assert.match(output(),
                   new RegExp(`^app: window bound ${WINDOW_WIDTH}x${WINDOW_HEIGHT}$`, 'm'));
   } catch (error) {
      console.error(error instanceof Error ? error.message : error);
      console.error(diagnostics());
      child.kill('SIGKILL');
      clearTimeout(deadline);
      server.close();
      process.exit(1);
   }

   clearTimeout(deadline);
   server.close();
   process.exit(0);
}

main().catch((error) => {
   console.error(error);
   process.exit(1);
});
