'use strict';

/*
  The other end of the Emscripten WebSocket dial, shared by the Node suites.

  Emscripten's WebSocket library dials through the global WebSocket client that
  Node provides; this module accepts that dial. node:http performs the upgrade
  handshake and the frame codec below speaks just enough RFC 6455 for the
  suites: no extensions, no compression, one whole message per delivery. It is
  deliberately dependency-free, so a suite needs no npm install.

  Each suite brings its own conversation. A suite that reacts to messages sets
  `onMessage`; a suite that drives a request/response dialogue awaits
  `nextMessage()`. `startServer` hands the connection and the upgrade request to
  the suite, so a suite can assert what the dial carried.
*/

const http = require('node:http');
const crypto = require('node:crypto');

const WEBSOCKET_GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';
const OPCODE_CONTINUATION = 0x0;
const OPCODE_BINARY = 0x2;
const OPCODE_CLOSE = 0x8;
const OPCODE_PING = 0x9;
const OPCODE_PONG = 0xa;

const DEFAULT_TIMEOUT_MS = 10000;

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
  does. Messages from the client are small JSON lines, so a single-fragment
  reader is enough, with continuation frames assembled in case a client splits
  one.
*/
class Connection {
   constructor(socket) {
      this.onMessage = null;
      this.onClose = null;
      this.pending = Buffer.alloc(0);
      this.fragments = [];
      this.closed = false;
      this.inbox = [];
      this.waiters = [];
      this.socket = socket;

      socket.on('data', (chunk) => {
         this.pending = Buffer.concat([this.pending, chunk]);
         this.parse();
      });
      socket.on('error', () => this.socket.destroy());
      socket.on('close', () => this.finish());
   }

   sendBinary(payload) {
      this.send(OPCODE_BINARY, payload);
   }

   /* Sends a normal close and ends the socket; a no-op once it has closed. */
   close(code = 1000) {
      if (this.closed) {
         return;
      }
      this.send(OPCODE_CLOSE, closePayload(code));
      this.closed = true;
      this.socket.end();
   }

   /*
     The next whole message as bytes, for a suite that awaits one response at a
     time. A closed connection rejects instead of hanging, and so does a peer
     that never answers.
   */
   nextMessage(timeoutMs = DEFAULT_TIMEOUT_MS) {
      if (this.inbox.length > 0) {
         return Promise.resolve(this.inbox.shift());
      }
      if (this.closed) {
         return Promise.reject(new Error('the WebSocket closed before a message arrived'));
      }
      return new Promise((resolve, reject) => {
         const waiter = {resolve, reject, timer: null};
         waiter.timer = setTimeout(() => {
            this.waiters = this.waiters.filter((entry) => entry !== waiter);
            reject(new Error(`no WebSocket message within ${timeoutMs} ms`));
         }, timeoutMs);
         this.waiters.push(waiter);
      });
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
      const message = Buffer.concat(this.fragments);
      this.fragments = [];
      this.deliver(message);
   }

   deliver(message) {
      if (this.onMessage !== null) {
         this.onMessage(message);
         return;
      }
      const waiter = this.waiters.shift();
      if (waiter !== undefined) {
         clearTimeout(waiter.timer);
         waiter.resolve(message);
         return;
      }
      this.inbox.push(message);
   }

   finish() {
      this.closed = true;
      const waiters = this.waiters;
      this.waiters = [];
      for (const waiter of waiters) {
         clearTimeout(waiter.timer);
         waiter.reject(new Error('the WebSocket closed before a message arrived'));
      }
      if (this.onClose !== null) {
         this.onClose();
      }
   }
}

/*
  Listens on an ephemeral loopback port and calls `onConnection(connection,
  request)` for the one upgrade a suite expects. Resolves with the server and
  its port, which the suite passes to the child in SDL3MCP_TEST_WS_PORT.
*/
function startServer(onConnection) {
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
      onConnection(new Connection(socket), request);
   });

   return new Promise((resolve, reject) => {
      server.on('error', reject);
      server.listen(0, '127.0.0.1', () => {
         resolve({server, port: server.address().port});
      });
   });
}

module.exports = {startServer};
