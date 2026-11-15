/*
  The sdl_info wire shape is produced by a pure serializer, so every field can
  be checked without a window or a session. These cases pin the 1.0 shape: the
  capture statistics are empty until the pipeline lands, and `limits` carries
  only maxFrameBytes because 1.0 has no library frame timeout.
*/

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <nlohmann/json.hpp>
#include <string>

#include "Info.h"

namespace {

using sdl3mcp::Info;

Info unboundInfo() {
   Info info;
   info.sdlVersion = "3.4.18";
   info.maxFrameBytes = 4096;
   info.frame = 7;
   info.appliedFrame = 3;
   return info;
}

TEST_CASE("unbound info") {
   const nlohmann::json json = sdl3mcp::infoToJson(unboundInfo());

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

   CHECK(json["limits"]["maxFrameBytes"] == 4096);
   CHECK(!json["limits"].contains("frameTimeoutMs"));

   CHECK(json["frame"] == 7);
   CHECK(json["appliedFrame"] == 3);
}

TEST_CASE("bound window") {
   Info info = unboundInfo();
   info.videoDriver = "wayland";
   info.window.bound = true;
   info.window.title = "SDL3MCP demo";
   info.window.width = 960;
   info.window.height = 540;
   info.window.pixelWidth = 1920;
   info.window.pixelHeight = 1080;
   info.window.pixelDensity = 2.0F;
   info.window.relativeMouseMode = true;
   info.heldKeys = {"left ctrl", "s"};
   info.heldButtons = {"left"};

   const nlohmann::json json = sdl3mcp::infoToJson(info);

   CHECK(json["videoDriver"] == "wayland");
   CHECK(json["window"].is_object());
   CHECK(json["window"]["title"] == "SDL3MCP demo");
   CHECK(json["window"]["width"] == 960);
   CHECK(json["window"]["height"] == 540);
   CHECK(json["window"]["pixelWidth"] == 1920);
   CHECK(json["window"]["pixelHeight"] == 1080);
   CHECK(json["window"]["pixelDensity"] == 2.0);
   CHECK(json["window"]["relativeMouseMode"] == true);

   CHECK(json["input"]["heldKeys"] == nlohmann::json({"left ctrl", "s"}));
   CHECK(json["input"]["heldButtons"] == nlohmann::json({"left"}));
}

TEST_CASE("capture statistics and errors") {
   Info info = unboundInfo();
   info.capture.pending = true;
   info.capture.count = 3;
   info.capture.dropped = 2;
   info.capture.lastWidth = 640;
   info.capture.lastHeight = 360;
   info.capture.lastBytes = 640u * 360u * 4u;
   info.capture.lastMs = 12.5;
   info.capture.lastError = "frame size does not match the dimensions";

   const nlohmann::json json = sdl3mcp::infoToJson(info);

   CHECK(json["capture"]["pending"] == true);
   CHECK(json["capture"]["count"] == 3);
   CHECK(json["capture"]["dropped"] == 2);
   CHECK(json["capture"]["lastWidth"] == 640);
   CHECK(json["capture"]["lastHeight"] == 360);
   CHECK(json["capture"]["lastBytes"] == 640u * 360u * 4u);
   CHECK(json["capture"]["lastMs"] == 12.5);
   CHECK(json["capture"]["lastError"] ==
         "frame size does not match the dimensions");
}

}  // namespace
