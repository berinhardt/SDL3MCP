'use strict';

/*
  Runs the Emscripten WebSocket suite against a local WebSocket server.

  The test executable is the wasm page, run by Node; Emscripten's WebSocket
  library dials through the global WebSocket client that Node provides. The
  server here is the other end of that dial. It is deliberately dependency-free
  (node:http plus a small RFC 6455 frame reader/writer) so the suite needs no
  npm install.

  The port is ephemeral and handed to the test in SDL3MCP_TEST_WS_PORT, so two
  runs can never collide. The child's exit code is the suite's verdict.
*/

const http = require('node:http');
const crypto = require('node:crypto');
const path = require('node:path');
const { spawn } = require('node:child_process');

const WEBSOCKET_GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';
const OPCODE_CONTINUATION = 0x0;
const OPCODE_TEXT = 0x1;
const OPCODE_BINARY = 0x2;
const OPCODE_CLOSE = 0x8;
const OPCODE_PING = 0x9;
const OPCODE_PONG = 0xa;

function encodeFrame(opcode, payload) {
   const length = payload.length;
   let header;
   if (length < 126) {
      header = Buffer.from([0x80 | opcode, length]);
   } else if (length < 65536) {
      header = Buffer.alloc(4);
      header[0] = 0x80 | opcode;
      header[1] = 126;
      header.writeUInt16BE(length, 2);
   } else {
      header = Buffer.alloc(10);
      header[0] = 0x80 | opcode;
      header[1] = 127;
      header.writeUInt32BE(0, 2);
      header.writeUInt32BE(length, 6);
   }
   return Buffer.concat([header, payload]);
}

function closePayload(code) {
   const payload = Buffer.alloc(2);
   payload.writeUInt16BE(code, 0);
   return payload;
}

/*
  One accepted connection. The client always masks its frames; the server never
  does. Messages from the client are small JSON lines or the words the test
  sends, so a single-fragment reader is enough, with continuation frames
  assembled in case a client splits one.
*/
class Connection {
   constructor(socket) {
      this.socket = socket;
      this.pending = Buffer.alloc(0);
      this.fragments = [];
      this.closed = false;

      socket.on('data', (chunk) => {
         this.pending = Buffer.concat([this.pending, chunk]);
         this.parse();
      });
      socket.on('error', () => this.socket.destroy());

      /* The greeting exercises a whole line, a line split across two messages,
         and CRLF framing, all before the client asks for anything. */
      this.send(OPCODE_BINARY, Buffer.from('hello\n'));
      this.send(OPCODE_BINARY, Buffer.from('par'));
      this.send(OPCODE_BINARY, Buffer.from('tial\n'));
      this.send(OPCODE_BINARY, Buffer.from('crlf\r\n'));
   }

   send(opcode, payload) {
      if (this.closed) {
         return;
      }
      this.socket.write(encodeFrame(opcode, payload));
   }

   parse() {
      for (;;) {
         if (this.pending.length < 2) {
            return;
         }
         const first = this.pending[0];
         const second = this.pending[1];
         const finished = (first & 0x80) !== 0;
         const opcode = first & 0x0f;
         const masked = (second & 0x80) !== 0;
         let length = second & 0x7f;
         let offset = 2;

         if (length === 126) {
            if (this.pending.length < offset + 2) {
               return;
            }
            length = this.pending.readUInt16BE(offset);
            offset += 2;
         } else if (length === 127) {
            if (this.pending.length < offset + 8) {
               return;
            }
            const high = this.pending.readUInt32BE(offset);
            const low = this.pending.readUInt32BE(offset + 4);
            length = high * 0x100000000 + low;
            offset += 8;
         }

         const maskLength = masked ? 4 : 0;
         if (this.pending.length < offset + maskLength + length) {
            return;
         }

         let payload = this.pending.subarray(offset + maskLength,
                                            offset + maskLength + length);
         if (masked) {
            const mask = this.pending.subarray(offset, offset + 4);
            payload = Buffer.from(payload);
            for (let index = 0; index < payload.length; ++index) {
               payload[index] ^= mask[index % 4];
            }
         } else {
            payload = Buffer.from(payload);
         }
         this.pending = this.pending.subarray(offset + maskLength + length);

         this.handleFrame(finished, opcode, payload);
         if (this.closed) {
            return;
         }
      }
   }

   handleFrame(finished, opcode, payload) {
      if (opcode === OPCODE_CLOSE) {
         this.send(OPCODE_CLOSE, payload.subarray(0, 2));
         this.closed = true;
         this.socket.end();
         return;
      }
      if (opcode === OPCODE_PING) {
         this.send(OPCODE_PONG, payload);
         return;
      }
      if (opcode === OPCODE_PONG) {
         return;
      }

      this.fragments.push(payload);
      if (!finished) {
         return;
      }
      const message = Buffer.concat(this.fragments).toString('utf8');
      this.fragments = [];
      this.handleMessage(message.replace(/\r?\n$/, ''));
   }

   handleMessage(message) {
      if (message === 'payload') {
         /* One result line and then exactly its byteLength in bytes, as the
            library's raw framing sends a frame. */
         this.send(OPCODE_BINARY, Buffer.from(
             '{"format":"rgba8888","width":1,"height":1,"byteLength":4,'
             + '"frame":7}\n'));
         this.send(OPCODE_BINARY, Buffer.from([1, 2, 3, 4]));
         return;
      }
      if (message === 'ping') {
         this.send(OPCODE_BINARY, Buffer.from('{"echo":"ping"}\n'));
         return;
      }
      if (message === 'close') {
         this.send(OPCODE_CLOSE, closePayload(1000));
         this.closed = true;
         this.socket.end();
      }
   }
}

function startServer() {
   const server = http.createServer();
   server.on('upgrade', (request, socket) => {
      const key = request.headers['sec-websocket-key'];
      if (typeof key !== 'string') {
         socket.destroy();
         return;
      }
      const accept = crypto
          .createHash('sha1')
          .update(key + WEBSOCKET_GUID)
          .digest('base64');
      socket.write(
          'HTTP/1.1 101 Switching Protocols\r\n'
          + 'Upgrade: websocket\r\n'
          + 'Connection: Upgrade\r\n'
          + `Sec-WebSocket-Accept: ${accept}\r\n`
          + '\r\n');
      new Connection(socket);
   });

   return new Promise((resolve, reject) => {
      server.on('error', reject);
      server.listen(0, '127.0.0.1', () => {
         resolve({server, port: server.address().port});
      });
   });
}

async function main() {
   const testExecutable = process.argv[2];
   if (!testExecutable) {
      console.error('usage: node run_node_websocket_test.js <test.js>');
      process.exit(2);
   }

   const {server, port} = await startServer();
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
