/*
  The sdl_info wire shape is produced by a pure serializer, so every field can be
  checked without a window, a server, or a thread. These tests pin the awkward
  cases: null means "not bound" or "not set", never an empty value that a client
  would have to interpret differently.
*/

#include "info.hpp"

#include "test_util.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <string>

namespace
{

using sdl3mcp::InfoSnapshot;

InfoSnapshot unboundSnapshot()
{
    InfoSnapshot snapshot;
    snapshot.sdlVersion = "3.4.18";
    snapshot.frameTimeoutMs = 1500;
    snapshot.maxFrameBytes = 4096;
    snapshot.frame = 7;
    snapshot.appliedFrame = 3;
    return snapshot;
}

void testUnboundSnapshot()
{
    const nlohmann::json json = sdl3mcp::infoToJson(unboundSnapshot());

    CHECK(json["sdlVersion"] == "3.4.18");
    CHECK(json["videoDriver"].is_null());
    CHECK(json["window"].is_null());

    CHECK(json["input"]["heldKeys"].is_array());
    CHECK(json["input"]["heldKeys"].empty());
    CHECK(json["input"]["heldButtons"].is_array());
    CHECK(json["input"]["heldButtons"].empty());

    CHECK(json["capture"]["pending"] == false);
    CHECK(json["capture"]["count"] == 0);
    CHECK(json["capture"]["dropped"] == 0);
    CHECK(json["capture"]["lastWidth"] == 0);
    CHECK(json["capture"]["lastHeight"] == 0);
    CHECK(json["capture"]["lastBytes"] == 0);
    CHECK(json["capture"]["lastMs"] == 0.0);
    CHECK(json["capture"]["lastError"].is_null());

    CHECK(json["limits"]["frameTimeoutMs"] == 1500);
    CHECK(json["limits"]["maxFrameBytes"] == 4096);
    CHECK(json["frame"] == 7);
    CHECK(json["appliedFrame"] == 3);
}

void testBoundWindow()
{
    InfoSnapshot snapshot = unboundSnapshot();
    snapshot.videoDriver = "wayland";
    snapshot.window.bound = true;
    snapshot.window.title = "SDL3MCP demo";
    snapshot.window.width = 960;
    snapshot.window.height = 540;
    snapshot.window.pixelWidth = 1920;
    snapshot.window.pixelHeight = 1080;
    snapshot.window.pixelDensity = 2.0F;
    snapshot.window.relativeMouseMode = true;

    const nlohmann::json json = sdl3mcp::infoToJson(snapshot);

    CHECK(json["videoDriver"] == "wayland");
    CHECK(json["window"].is_object());
    CHECK(json["window"]["title"] == "SDL3MCP demo");
    CHECK(json["window"]["width"] == 960);
    CHECK(json["window"]["height"] == 540);
    CHECK(json["window"]["pixelWidth"] == 1920);
    CHECK(json["window"]["pixelHeight"] == 1080);
    CHECK(json["window"]["pixelDensity"] == 2.0);
    CHECK(json["window"]["relativeMouseMode"] == true);
}

void testCaptureError()
{
    InfoSnapshot snapshot = unboundSnapshot();

    CHECK(sdl3mcp::infoToJson(snapshot)["capture"]["lastError"].is_null());

    snapshot.capture.lastError = "frame timeout";
    snapshot.capture.count = 2;
    snapshot.capture.dropped = 1;

    const nlohmann::json capture = sdl3mcp::infoToJson(snapshot)["capture"];
    CHECK(capture["lastError"] == "frame timeout");
    CHECK(capture["count"] == 2);
    CHECK(capture["dropped"] == 1);
}

void testHeldInputOrder()
{
    InfoSnapshot snapshot = unboundSnapshot();
    snapshot.heldKeys = {"ctrl", "s"};
    snapshot.heldButtons = {"left", "right"};

    const nlohmann::json json = sdl3mcp::infoToJson(snapshot);
    CHECK(json["input"]["heldKeys"] ==
          nlohmann::json::array({"ctrl", "s"}));
    CHECK(json["input"]["heldButtons"] ==
          nlohmann::json::array({"left", "right"}));

    /* The arrays are serialized in insertion order, not sorted. */
    CHECK(json["input"]["heldKeys"][0] == "ctrl");
    CHECK(json["input"]["heldKeys"][1] == "s");
}

} // namespace

int main()
{
    testUnboundSnapshot();
    testBoundWindow();
    testCaptureError();
    testHeldInputOrder();

    std::fprintf(stderr, "info tests passed\n");
    return 0;
}
