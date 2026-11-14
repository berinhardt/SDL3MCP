'use strict';

/*
  Runs the Emscripten transport suite against a local WebSocket server.

  The test executable is the wasm page, run by Node; Emscripten's WebSocket
  library dials through the global WebSocket client that Node provides. The
  server here is the other end of that dial; it is the shared, dependency-free
  server in node_websocket_server.js.

  This peer scripts the conversation rather than reacting to a protocol: it
  greets with a whole line, a line split across two messages and a CRLF line,
  answers "payload" with a frame header and exactly its byteLength in raw bytes,
  answers "ping" with a line, and closes on "close".

  The port is ephemeral and handed to the test in SDL3MCP_TEST_WS_PORT, so two
  runs can never collide. The child's exit code is the suite's verdict.
*/

const path = require('node:path');
const {spawn} = require('node:child_process');

const {startServer} = require('./node_websocket_server.js');

function scriptPeer(connection) {
   connection.onMessage = (message) => {
      handleMessage(connection, message.toString('utf8').replace(/\r?\n$/, ''));
   };

   /* The greeting exercises a whole line, a line split across two messages,
      and CRLF framing, all before the client asks for anything. */
   connection.sendBinary(Buffer.from('hello\n'));
   connection.sendBinary(Buffer.from('par'));
   connection.sendBinary(Buffer.from('tial\n'));
   connection.sendBinary(Buffer.from('crlf\r\n'));
}

function handleMessage(connection, message) {
   if (message === 'payload') {
      /* One result line and then exactly its byteLength in bytes, as the
         library's raw framing sends a frame. */
      connection.sendBinary(Buffer.from(
          '{"format":"rgba8888","width":1,"height":1,"byteLength":4,'
          + '"frame":7}\n'));
      connection.sendBinary(Buffer.from([1, 2, 3, 4]));
      return;
   }
   if (message === 'ping') {
      connection.sendBinary(Buffer.from('{"echo":"ping"}\n'));
      return;
   }
   if (message === 'close') {
      connection.close(1000);
   }
}

async function main() {
   const testExecutable = process.argv[2];
   if (!testExecutable) {
      console.error('usage: node run_node_websocket_test.js <test.js>');
      process.exit(2);
   }
   if (typeof WebSocket !== 'function') {
      console.error('this Node has no global WebSocket; the suite needs Node 22 or newer');
      process.exit(2);
   }

   const {server, port} = await startServer(scriptPeer);
   const child = spawn(process.execPath, [path.resolve(testExecutable)], {
      stdio: 'inherit',
      env: {...process.env, SDL3MCP_TEST_WS_PORT: String(port)},
   });

   /* A hung dial must fail the run, not leave CTest waiting forever. */
   const deadline = setTimeout(() => child.kill('SIGKILL'), 60000);

   const code = await new Promise((resolve) => {
      child.on('exit', (exitCode, signal) => {
         resolve(signal === null ? exitCode : 1);
      });
   });
   clearTimeout(deadline);

   server.close();
   /* The accepted sockets are not tracked by http.Server; the child is gone, so
      the process would exit anyway, but close() keeps the exit prompt. */
   process.exit(code === null ? 1 : code);
}

main().catch((error) => {
   console.error(error);
   process.exit(1);
});
