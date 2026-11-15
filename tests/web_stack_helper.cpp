/*
  A controllable stand-in for the two tools a web run starts.

    --listen PORT --pid-file PATH
        Bind 127.0.0.1:PORT and accept connections forever, discarding them.
        This is the page server: the launcher only probes that the port accepts.

    --bridge PORT HOST:PORT --pid-file PATH
        Bind 127.0.0.1:PORT and, for each connection that sends bytes, dial
        HOST:PORT and splice the two streams. A connection that closes without
        sending anything — the launcher's readiness probe — never reaches the
        Handler, which is how websocat's WebSocket listener behaves.

  Each connection is handled in its own child, so one closed connection cannot
  end the listener. The whole process tree stays in the bridge's process group,
  which is what the Handler's SIGTERM to -pid reaches.
*/

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace {

constexpr int kPollMs = 50;
constexpr int kDialTimeoutMs = 2000;

void writePidFile(const std::string& path) {
   if (path.empty()) {
      return;
   }
   std::FILE* file = std::fopen(path.c_str(), "w");
   if (file == nullptr) {
      return;
   }
   std::fprintf(file, "%d\n", static_cast<int>(::getpid()));
   std::fclose(file);
}

bool parsePort(const char* text, std::uint16_t& port) {
   char* end = nullptr;
   const long value = std::strtol(text, &end, 10);
   if (end == text || *end != '\0' || value <= 0 || value > 65535) {
      return false;
   }
   port = static_cast<std::uint16_t>(value);
   return true;
}

int listenLoopback(std::uint16_t port) {
   const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
   if (fd < 0) {
      return -1;
   }
   int reuse = 1;
   (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);

   sockaddr_in address{};
   address.sin_family = AF_INET;
   address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
   address.sin_port = htons(port);
   if (::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof address) !=
           0 ||
       ::listen(fd, 16) != 0) {
      ::close(fd);
      return -1;
   }
   return fd;
}

int dialLoopback(std::uint16_t port) {
   const auto deadline =
       std::chrono::steady_clock::now() + std::chrono::milliseconds(kDialTimeoutMs);
   for (;;) {
      const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
      if (fd >= 0) {
         sockaddr_in address{};
         address.sin_family = AF_INET;
         address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
         address.sin_port = htons(port);
         if (::connect(fd, reinterpret_cast<const sockaddr*>(&address),
                       sizeof address) == 0) {
            return fd;
         }
         ::close(fd);
      }
      if (std::chrono::steady_clock::now() >= deadline) {
         return -1;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
   }
}

/*
  Classifies a new connection without speaking a protocol. The launcher's
  readiness probe connects and closes at once, so it is an EOF; the application
  connects and waits for the Handler's initialize, so it sends nothing and stays
  open. Bytes are the WebSocket-handshake case and are forwarded as they are.
*/
bool readFirstChunk(int fd, std::string& chunk, bool& closed) {
   chunk.clear();
   closed = false;
   for (;;) {
      pollfd descriptor{fd, POLLIN, 0};
      const int ready = ::poll(&descriptor, 1, 100);
      if (ready < 0) {
         if (errno == EINTR) {
            continue;
         }
         closed = true;
         return false;
      }
      if (ready == 0) {
         /* An open, silent connection: the application waiting to be greeted. */
         return true;
      }
      char buffer[4096];
      const ssize_t count = ::read(fd, buffer, sizeof buffer);
      if (count <= 0) {
         closed = true;
         return false;
      }
      chunk.assign(buffer, static_cast<std::size_t>(count));
      return true;
   }
}

/* Copies both directions until both sides have closed their write half. */
void splice(int left, int right) {
   bool leftOpen = true;
   bool rightOpen = true;
   while (leftOpen || rightOpen) {
      pollfd descriptors[2] = {{left, POLLIN, 0}, {right, POLLIN, 0}};
      const int ready = ::poll(descriptors, 2, -1);
      if (ready < 0) {
         if (errno == EINTR) {
            continue;
         }
         break;
      }

      char buffer[8192];
      if (leftOpen && (descriptors[0].revents & (POLLIN | POLLHUP)) != 0) {
         const ssize_t count = ::read(left, buffer, sizeof buffer);
         if (count <= 0) {
            leftOpen = false;
            ::shutdown(right, SHUT_WR);
         } else {
            (void)::write(right, buffer, static_cast<std::size_t>(count));
         }
      }
      if (rightOpen && (descriptors[1].revents & (POLLIN | POLLHUP)) != 0) {
         const ssize_t count = ::read(right, buffer, sizeof buffer);
         if (count <= 0) {
            rightOpen = false;
            ::shutdown(left, SHUT_WR);
         } else {
            (void)::write(left, buffer, static_cast<std::size_t>(count));
         }
      }
   }
}

void serveConnection(int client, std::uint16_t upstreamPort) {
   std::string chunk;
   bool closed = false;
   if (!readFirstChunk(client, chunk, closed) || closed) {
      ::close(client);
      return;
   }
   const int upstream = dialLoopback(upstreamPort);
   if (upstream < 0) {
      ::close(client);
      return;
   }
   if (!chunk.empty()) {
      (void)::write(upstream, chunk.data(), chunk.size());
   }
   splice(client, upstream);
   ::close(client);
   ::close(upstream);
}

int runListen(std::uint16_t port) {
   const int listener = listenLoopback(port);
   if (listener < 0) {
      std::fprintf(stderr, "web-stack: cannot listen on %u\n",
                   static_cast<unsigned>(port));
      return 1;
   }
   for (;;) {
      const int client = ::accept(listener, nullptr, nullptr);
      if (client < 0) {
         if (errno == EINTR) {
            continue;
         }
         return 1;
      }
      ::close(client);
   }
}

int runBridge(std::uint16_t port, std::uint16_t upstreamPort) {
   const int listener = listenLoopback(port);
   if (listener < 0) {
      std::fprintf(stderr, "web-stack: cannot listen on %u\n",
                   static_cast<unsigned>(port));
      return 1;
   }
   for (;;) {
      const int client = ::accept(listener, nullptr, nullptr);
      if (client < 0) {
         if (errno == EINTR) {
            continue;
         }
         return 1;
      }
      const pid_t child = ::fork();
      if (child == 0) {
         ::close(listener);
         serveConnection(client, upstreamPort);
         ::_exit(0);
      }
      ::close(client);
   }
}

}  // namespace

int main(int argc, char** argv) {
   ::signal(SIGPIPE, SIG_IGN);

   bool bridge = false;
   bool sleepMode = false;
   std::uint16_t port = 0;
   std::uint16_t upstreamPort = 0;
   std::string pidFile;

   for (int index = 1; index < argc; ++index) {
      const std::string argument = argv[index];
      if (argument == "--sleep") {
         sleepMode = true;
      } else if (argument == "--listen") {
         bridge = false;
         if (index + 1 >= argc || !parsePort(argv[++index], port)) {
            return 2;
         }
      } else if (argument == "--bridge") {
         bridge = true;
         if (index + 2 >= argc || !parsePort(argv[++index], port)) {
            return 2;
         }
         const std::string target = argv[++index];
         const std::size_t separator = target.rfind(':');
         if (separator == std::string::npos ||
             !parsePort(target.c_str() + separator + 1, upstreamPort)) {
            return 2;
         }
      } else if (argument == "--pid-file" && index + 1 < argc) {
         pidFile = argv[++index];
      } else {
         return 2;
      }
   }

   if (sleepMode) {
      writePidFile(pidFile);
      for (;;) {
         ::pause();
      }
   }

   if (port == 0) {
      return 2;
   }

   writePidFile(pidFile);

   /* The bridge reaps its per-connection children without tracking them. */
   struct sigaction reap{};
   reap.sa_handler = SIG_IGN;
   sigemptyset(&reap.sa_mask);
   reap.sa_flags = SA_NOCLDWAIT;
   (void)::sigaction(SIGCHLD, &reap, nullptr);

   return bridge ? runBridge(port, upstreamPort) : runListen(port);
}
