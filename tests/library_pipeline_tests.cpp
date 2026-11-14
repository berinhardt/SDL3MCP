/*
  The 1.0 library end to end over its native socket, with a test MCP server on a
  loopback port and the C API on the main thread. The app uses SDL's dummy video
  driver, so a real window exists with no display; it writes one synthetic frame
  whenever SDL3MCP_FrameRequested() is true.

  The conversation pins the input/capture pipeline: a capture is a no-op input,
  several requests registered in one pump share the next frame, a region crops
  that frame per request, a rejected WriteFrame leaves the request pending, and
  sdl_info reads the live stats. One lifecycle per process, so this is the only
  AppInit in this executable.
*/

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <SDL3/SDL.h>
#include <SDL3MCP/SDL3MCP.h>
#include <arpa/inet.h>
#include <doctest/doctest.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

constexpr int kFrameWidth = 4;
constexpr int kFrameHeight = 3;
constexpr int kWindowWidth = 16;
constexpr int kWindowHeight = 12;
constexpr std::uint32_t kDefaultMaxFrameBytes = 32u * 1024u * 1024u;

/* A frame whose pixels encode their coordinates, so a crop is checkable. */
std::vector<std::uint8_t> framePattern() {
   std::vector<std::uint8_t> pixels(
       static_cast<std::size_t>(kFrameWidth) * kFrameHeight * 4);
   for (int y = 0; y < kFrameHeight; ++y) {
      for (int x = 0; x < kFrameWidth; ++x) {
         const std::size_t index =
             (static_cast<std::size_t>(y) * kFrameWidth + x) * 4;
         pixels[index + 0] = static_cast<std::uint8_t>(x * 40 + 1);
         pixels[index + 1] = static_cast<std::uint8_t>(y * 40 + 2);
         pixels[index + 2] = 3;
         pixels[index + 3] = 255;
      }
   }
   return pixels;
}

std::vector<std::uint8_t> croppedPattern(int x, int y, int width, int height) {
   const std::vector<std::uint8_t> frame = framePattern();
   std::vector<std::uint8_t> cropped(
       static_cast<std::size_t>(width) * height * 4);
   for (int row = 0; row < height; ++row) {
      for (int column = 0; column < width; ++column) {
         const std::size_t source =
             (static_cast<std::size_t>(y + row) * kFrameWidth + (x + column)) * 4;
         const std::size_t target =
             (static_cast<std::size_t>(row) * width + column) * 4;
         std::copy_n(frame.begin() + static_cast<std::ptrdiff_t>(source), 4,
                     cropped.begin() + static_cast<std::ptrdiff_t>(target));
      }
   }
   return cropped;
}

void writeAll(int fd, std::string_view data) {
   std::size_t written = 0;
   while (written < data.size()) {
      const ssize_t count =
          ::write(fd, data.data() + written, data.size() - written);
      if (count < 0 && errno == EINTR) {
         continue;
      }
      CHECK(count > 0);
      if (count <= 0) {
         return;
      }
      written += static_cast<std::size_t>(count);
   }
}

void writeLine(int fd, const nlohmann::json& message) {
   writeAll(fd, message.dump() + "\n");
}

/* Reads newline-delimited messages and raw payload bytes, buffering both. */
class LineReader {
  public:
   explicit LineReader(int fd) : fd_(fd) {
   }

   std::string readLine(std::chrono::milliseconds timeout = 10000ms) {
      const auto deadline = std::chrono::steady_clock::now() + timeout;
      for (;;) {
         const std::size_t newline = pending_.find('\n');
         if (newline != std::string::npos) {
            std::string line = pending_.substr(0, newline);
            pending_.erase(0, newline + 1);
            return line;
         }
         if (!fill(deadline)) {
            return {};
         }
      }
   }

   std::vector<std::uint8_t> readBytes(std::size_t count) {
      const auto deadline = std::chrono::steady_clock::now() + 10000ms;
      std::vector<std::uint8_t> bytes;
      bytes.reserve(count);
      while (bytes.size() < count) {
         if (!pending_.empty()) {
            const std::size_t take =
                std::min(pending_.size(), count - bytes.size());
            bytes.insert(bytes.end(), pending_.begin(),
                         pending_.begin() + static_cast<std::ptrdiff_t>(take));
            pending_.erase(0, take);
            continue;
         }
         if (!fill(deadline)) {
            break;
         }
      }
      CHECK(bytes.size() == count);
      return bytes;
   }

  private:
   bool fill(std::chrono::steady_clock::time_point deadline) {
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now());
      CHECK(remaining > 0ms);
      if (remaining <= 0ms) {
         return false;
      }

      pollfd descriptor{};
      descriptor.fd = fd_;
      descriptor.events = POLLIN;
      const int ready = ::poll(&descriptor, 1, static_cast<int>(remaining.count()));
      if (ready < 0 && errno == EINTR) {
         return true;
      }
      CHECK(ready > 0);
      if (ready <= 0) {
         return false;
      }

      char chunk[1024];
      const ssize_t count = ::read(fd_, chunk, sizeof chunk);
      CHECK(count > 0);
      if (count <= 0) {
         return false;
      }
      pending_.append(chunk, static_cast<std::size_t>(count));
      return true;
   }

   int fd_ = -1;
   std::string pending_;
};

/* What the app loop shares with the test server thread. */
struct AppControl {
   std::atomic<bool> finished{false};
   std::atomic<int> dropFrames{0};
   std::atomic<bool> holdAfterDrop{false};
   std::atomic<bool> droppedOne{false};
};

struct RawResponse {
   nlohmann::json header;
   std::vector<std::uint8_t> payload;
};

const RawResponse* findByHeaderId(const std::vector<RawResponse>& responses,
                                  const nlohmann::json& id) {
   for (const RawResponse& response : responses) {
      if (response.header["id"] == id) {
         return &response;
      }
   }
   return nullptr;
}

nlohmann::json callLine(const nlohmann::json& id,
                        std::string_view name,
                        nlohmann::json arguments = nlohmann::json::object()) {
   return nlohmann::json{
       {"jsonrpc", "2.0"},
       {"id", id},
       {"method", "tools/call"},
       {"params",
        nlohmann::json{{"name", name}, {"arguments", std::move(arguments)}}},
   };
}

nlohmann::json region(int x, int y, int width, int height) {
   return nlohmann::json{
       {"x", x}, {"y", y}, {"width", width}, {"height", height}};
}

/*
   The MCP client half: it listens on loopback, accepts the app's one dial, and
   drives the conversation. `finished` is the app loop's stop condition.
*/
class TestServer {
  public:
   TestServer() = default;

   ~TestServer() {
      if (connection_ >= 0) {
         ::close(connection_);
      }
      if (listener_ >= 0) {
         ::close(listener_);
      }
   }

   bool listenOnLoopback() {
      listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
      CHECK(listener_ >= 0);
      if (listener_ < 0) {
         return false;
      }

      int reuse = 1;
      (void)::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &reuse,
                         sizeof reuse);

      sockaddr_in address{};
      address.sin_family = AF_INET;
      address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      address.sin_port = 0;
      CHECK(::bind(listener_, reinterpret_cast<const sockaddr*>(&address),
                   sizeof address) == 0);
      CHECK(::listen(listener_, 1) == 0);

      socklen_t length = sizeof address;
      CHECK(::getsockname(listener_, reinterpret_cast<sockaddr*>(&address),
                          &length) == 0);
      port = ntohs(address.sin_port);
      CHECK(port != 0);
      return listener_ >= 0 && port != 0;
   }

   void run(AppControl& control) {
      try {
         converse(control);
      } catch (const std::exception& exception) {
         failure = exception.what();
      } catch (...) {
         failure = "unknown exception";
      }
      finished.store(true, std::memory_order_release);
   }

   std::uint16_t port = 0;
   std::atomic<bool> finished{false};
   std::string failure;

  private:
   void converse(AppControl& control) {
      pollfd descriptor{};
      descriptor.fd = listener_;
      descriptor.events = POLLIN;
      const int ready = ::poll(&descriptor, 1, 10000);
      CHECK(ready > 0);
      if (ready <= 0) {
         return;
      }

      const int connection = ::accept(listener_, nullptr, nullptr);
      CHECK(connection >= 0);
      if (connection < 0) {
         return;
      }
      connection_ = connection;
      LineReader reader(connection);

      const nlohmann::json initialized = request(
          reader, {{"jsonrpc", "2.0"},
                   {"id", 1},
                   {"method", "initialize"},
                   {"params", {{"protocolVersion", "2025-11-25"}}}});
      CHECK(initialized["result"]["serverInfo"]["name"] == "sdl3mcp");
      writeLine(connection,
                {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});

      const nlohmann::json listed =
          request(reader, {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}});
      std::vector<std::string> names;
      for (const nlohmann::json& tool : listed["result"]["tools"]) {
         names.push_back(tool["name"].get<std::string>());
      }
      CHECK(names.size() == 4);
      CHECK(std::find(names.begin(), names.end(), "sdl_capture") != names.end());
      CHECK(std::find(names.begin(), names.end(), "sdl_info") != names.end());
      CHECK(std::find(names.begin(), names.end(), "sdl_input") != names.end());
      CHECK(std::find(names.begin(), names.end(), "sdl_terminate") != names.end());

      const nlohmann::json info = request(
          reader, {{"jsonrpc", "2.0"},
                   {"id", 3},
                   {"method", "tools/call"},
                   {"params", {{"name", "sdl_info"}, {"arguments", nlohmann::json::object()}}}});
      const nlohmann::json before = infoDocument(info);
      CHECK(before["window"]["title"] == "SDL3MCP pipeline test");
      CHECK(before["window"]["width"] == kWindowWidth);
      CHECK(before["window"]["height"] == kWindowHeight);
      CHECK(before["window"]["pixelWidth"] > 0);
      CHECK(before["window"]["pixelHeight"] > 0);
      CHECK(before["input"]["heldKeys"].is_array());
      CHECK(before["input"]["heldKeys"].empty());
      CHECK(before["input"]["heldButtons"].is_array());
      CHECK(before["input"]["heldButtons"].empty());
      CHECK(before["capture"]["count"] == 0);
      CHECK(before["capture"]["dropped"] == 0);
      CHECK(before["capture"]["pending"] == false);
      CHECK(before["limits"]["maxFrameBytes"] == kDefaultMaxFrameBytes);
      CHECK(!before["limits"].contains("frameTimeoutMs"));
      const std::uint64_t firstFrame = before["frame"].get<std::uint64_t>();

      /* One capture: the frame rendered after the request is attached. */
      writeLine(connection, callLine(4, "sdl_capture"));
      const RawResponse capture = readFrameResponse(reader);
      CHECK(capture.header["id"] == 4);
      checkFrame(capture, kFrameWidth, kFrameHeight, false);
      CHECK(capture.payload == framePattern());
      CHECK(capture.header["result"]["frame"].get<std::uint64_t>() > firstFrame);
      const std::uint64_t captureFrame =
          capture.header["result"]["frame"].get<std::uint64_t>();

      /* A region crops that same frame, never resizes it. */
      writeLine(connection, callLine(5, "sdl_capture", {{"region", region(1, 1, 2, 2)}}));
      const RawResponse cropped = readFrameResponse(reader);
      CHECK(cropped.header["id"] == 5);
      checkFrame(cropped, 2, 2, false);
      CHECK(cropped.payload == croppedPattern(1, 1, 2, 2));
      CHECK(cropped.header["result"]["frame"].get<std::uint64_t>() > captureFrame);

      /*
        Three requests written together reach one pump: a capture, an input and
        a cropped capture. They share the one frame the app writes next.
      */
      const std::string batch =
          callLine(6, "sdl_capture").dump() + "\n" +
          callLine(7, "sdl_input",
                   {{"actions",
                     nlohmann::json::array(
                         {{{"type", "key"}, {"key", "a"}, {"action", "press"}}})}})
              .dump() +
          "\n" +
          callLine(8, "sdl_capture", {{"region", region(0, 0, 1, 1)}}).dump() +
          "\n";
      writeAll(connection, batch);

      const RawResponse firstShared = readFrameResponse(reader);
      const RawResponse secondShared = readFrameResponse(reader);
      const RawResponse thirdShared = readFrameResponse(reader);
      const std::vector<RawResponse> shared{firstShared, secondShared,
                                            thirdShared};

      const RawResponse* sharedCapture = findByHeaderId(shared, 6);
      const RawResponse* sharedInput = findByHeaderId(shared, 7);
      const RawResponse* sharedCorner = findByHeaderId(shared, 8);
      REQUIRE(sharedCapture != nullptr);
      REQUIRE(sharedInput != nullptr);
      REQUIRE(sharedCorner != nullptr);

      const std::uint64_t sharedFrame =
          (*sharedCapture).header["result"]["frame"].get<std::uint64_t>();
      CHECK((*sharedInput).header["result"]["frame"].get<std::uint64_t>() ==
            sharedFrame);
      CHECK((*sharedCorner).header["result"]["frame"].get<std::uint64_t>() ==
            sharedFrame);
      CHECK((*sharedCorner).payload == croppedPattern(0, 0, 1, 1));
      checkFrame(*sharedInput, kFrameWidth, kFrameHeight, true);

      /* The text names the applied frame and the attached frame. */
      const std::string& text =
          (*sharedInput).header["result"]["text"].get<std::string>();
      CHECK(text.find("1 actions applied at frame ") == 0);
      CHECK(text.find("; frame " + std::to_string(sharedFrame) + " attached") !=
            std::string::npos);

      const nlohmann::json afterShared = infoDocument(request(
          reader, {{"jsonrpc", "2.0"},
                   {"id", 9},
                   {"method", "tools/call"},
                   {"params", {{"name", "sdl_info"}, {"arguments", nlohmann::json::object()}}}}));
      const std::uint64_t appliedFrame =
          afterShared["appliedFrame"].get<std::uint64_t>();
      CHECK(sharedFrame == appliedFrame + 1);
      CHECK(text == "1 actions applied at frame " + std::to_string(appliedFrame) +
                        "; frame " + std::to_string(sharedFrame) +
                        " attached, rendered after the actions were delivered");
      /* Two earlier captures, two shared captures and one input observation. */
      CHECK(afterShared["capture"]["count"] == 5);
      CHECK(afterShared["capture"]["pending"] == false);
      CHECK(afterShared["capture"]["lastWidth"] == kFrameWidth);
      CHECK(afterShared["capture"]["lastHeight"] == kFrameHeight);
      CHECK(afterShared["capture"]["lastBytes"] ==
            static_cast<std::uint64_t>(kFrameWidth * kFrameHeight * 4));
      CHECK(afterShared["capture"]["lastError"].is_null());

      /*
        A rejected WriteFrame leaves the request pending; the app holds the good
        frame so sdl_info can observe the drop before the retry completes it.
      */
      control.droppedOne.store(false, std::memory_order_release);
      control.dropFrames.store(1, std::memory_order_release);
      control.holdAfterDrop.store(true, std::memory_order_release);
      writeLine(connection, callLine(10, "sdl_capture"));

      const auto dropDeadline = std::chrono::steady_clock::now() + 10s;
      while (!control.droppedOne.load(std::memory_order_acquire) &&
             std::chrono::steady_clock::now() < dropDeadline) {
         std::this_thread::yield();
      }
      CHECK(control.droppedOne.load(std::memory_order_acquire));

      const nlohmann::json dropped = infoDocument(request(
          reader, {{"jsonrpc", "2.0"},
                   {"id", 11},
                   {"method", "tools/call"},
                   {"params", {{"name", "sdl_info"}, {"arguments", nlohmann::json::object()}}}}));
      CHECK(dropped["capture"]["pending"] == true);
      CHECK(dropped["capture"]["dropped"] == 1);
      CHECK(dropped["capture"]["lastError"].is_string());

      control.holdAfterDrop.store(false, std::memory_order_release);
      const RawResponse retried = readFrameResponse(reader);
      CHECK(retried.header["id"] == 10);
      checkFrame(retried, kFrameWidth, kFrameHeight, false);
      CHECK(retried.header["result"]["frame"].get<std::uint64_t>() > sharedFrame);

      const nlohmann::json recovered = infoDocument(request(
          reader, {{"jsonrpc", "2.0"},
                   {"id", 12},
                   {"method", "tools/call"},
                   {"params", {{"name", "sdl_info"}, {"arguments", nlohmann::json::object()}}}}));
      CHECK(recovered["capture"]["pending"] == false);
      CHECK(recovered["capture"]["dropped"] == 1);
      CHECK(recovered["capture"]["lastError"].is_null());
      CHECK(recovered["capture"]["count"] == 6);

      const nlohmann::json terminated = request(
          reader, {{"jsonrpc", "2.0"},
                   {"id", 13},
                   {"method", "tools/call"},
                   {"params", {{"name", "sdl_terminate"}, {"arguments", nlohmann::json::object()}}}});
      CHECK(terminated["result"]["content"][0]["text"] == "termination requested");

      control.finished.store(true, std::memory_order_release);
      ::close(connection_);
      connection_ = -1;
   }

   nlohmann::json request(LineReader& reader, const nlohmann::json& message) {
      writeLine(connection_, message);
      return nlohmann::json::parse(reader.readLine());
   }

   RawResponse readFrameResponse(LineReader& reader) {
      RawResponse response;
      response.header = nlohmann::json::parse(reader.readLine());
      const auto length = response.header["result"]["byteLength"].get<std::size_t>();
      response.payload = reader.readBytes(length);
      return response;
   }

   static nlohmann::json infoDocument(const nlohmann::json& response) {
      return nlohmann::json::parse(
          response["result"]["content"][0]["text"].get<std::string>());
   }

   static void checkFrame(const RawResponse& response,
                          int width,
                          int height,
                          bool hasText) {
      CHECK(response.header["result"]["format"] == "rgba8888");
      CHECK(response.header["result"]["width"] == width);
      CHECK(response.header["result"]["height"] == height);
      CHECK(response.header["result"]["byteLength"] ==
            static_cast<std::uint64_t>(width) * height * 4);
      CHECK(response.header["result"].contains("frame"));
      CHECK(response.header["result"].contains("text") == hasText);
      CHECK(response.payload.size() ==
            static_cast<std::size_t>(width) * height * 4);
   }

   int listener_ = -1;
   int connection_ = -1;
};

int quitEvents() {
   int count = 0;
   SDL_Event event;
   while (SDL_PollEvent(&event)) {
      if (event.type == SDL_EVENT_QUIT) {
         ++count;
      }
   }
   return count;
}

}  // namespace

TEST_CASE("the input and capture pipeline serves one frame per request batch") {
   (void)SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
   CHECK(SDL_Init(SDL_INIT_VIDEO));
   CHECK(std::string_view(SDL_GetCurrentVideoDriver()) == "dummy");

   SDL_Window* window =
       SDL_CreateWindow("SDL3MCP pipeline test", kWindowWidth, kWindowHeight, 0);
   CHECK(window != nullptr);

   AppControl control;
   TestServer server;
   CHECK(server.listenOnLoopback());
   std::thread serverThread([&server, &control] { server.run(control); });

   const std::string endpoint = "--mcp=127.0.0.1:" + std::to_string(server.port);
   char program[] = "library_pipeline_tests";
   char* argv[] = {program, const_cast<char*>(endpoint.c_str()), nullptr};
   CHECK(SDL3MCP_AppInit(nullptr, 2, argv) == SDL_APP_CONTINUE);
   CHECK(SDL3MCP_SetWindow(window) == SDL_APP_CONTINUE);

   const std::vector<std::uint8_t> pixels = framePattern();
   bool wroteBad = false;
   bool appFailed = false;
   const auto deadline = std::chrono::steady_clock::now() + 10s;
   while (!control.finished.load(std::memory_order_acquire) &&
          std::chrono::steady_clock::now() < deadline) {
      if (SDL3MCP_FrameRequested()) {
         if (control.dropFrames.load(std::memory_order_acquire) > 0) {
            control.dropFrames.fetch_sub(1, std::memory_order_acq_rel);
            SDL3MCP_WriteFrame(pixels.data(), pixels.size() - 1, kFrameWidth,
                               kFrameHeight);
            wroteBad = true;
            control.droppedOne.store(true, std::memory_order_release);
         } else if (!(control.holdAfterDrop.load(std::memory_order_acquire) &&
                      wroteBad)) {
            SDL3MCP_WriteFrame(pixels.data(), pixels.size(), kFrameWidth,
                               kFrameHeight);
            wroteBad = false;
         }
      }
      if (SDL3MCP_AppIterate() != SDL_APP_CONTINUE) {
         appFailed = true;
         break;
      }
      std::this_thread::yield();
   }
   CHECK(!appFailed);
   CHECK(control.finished.load(std::memory_order_acquire));

   /* The terminate request is answered just before the quit push. */
   CHECK(SDL3MCP_AppIterate() == SDL_APP_CONTINUE);
   CHECK(quitEvents() == 1);

   SDL3MCP_AppShutdown();
   SDL3MCP_AppShutdown();

   serverThread.join();
   CHECK(server.failure.empty());

   SDL_DestroyWindow(window);
   SDL_Quit();
}
