/*
  The 1.0 argument parsers: the same wire documents sdl_capture and sdl_input
  accept, with no library timeout in either. Both are pure, so every schema rule,
  default, and action index is pinned without a window, a thread, or a queue.
*/

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include "Capture.h"
#include "Input.h"
#include "InputJson.h"

namespace {

using sdl3mcp::CaptureArguments;
using sdl3mcp::InputRequest;
using sdl3mcp::KeyAction;
using sdl3mcp::KeyInput;
using sdl3mcp::Modifier;
using sdl3mcp::MouseButton;
using sdl3mcp::MouseButtonAction;
using sdl3mcp::MouseButtonInput;
using sdl3mcp::MouseMoveInput;
using sdl3mcp::ReleaseAllInput;
using sdl3mcp::ScrollInput;
using sdl3mcp::TextInput;

nlohmann::json single(const nlohmann::json& action) {
   return nlohmann::json{{"actions", nlohmann::json::array({action})}};
}

nlohmann::json argumentsWith(const nlohmann::json& actions) {
   return nlohmann::json{{"actions", actions}};
}

nlohmann::json repeatedKeyActions(std::size_t count) {
   nlohmann::json actions = nlohmann::json::array();
   for (std::size_t index = 0; index < count; ++index) {
      actions.push_back(nlohmann::json{{"type", "key"}, {"key", "a"}});
   }
   return actions;
}

void checkErrorText(const std::string& error,
                    std::string_view fragment,
                    const char* file,
                    int line) {
   const bool hasFragment = error.find(fragment) != std::string::npos;
   const bool isSingleLine = error.find('\n') == std::string::npos;
   if (error.empty() || !hasFragment || !isSingleLine) {
      std::fprintf(stderr,
                   "%s:%d: error \"%s\" does not match \"%.*s\"\n",
                   file,
                   line,
                   error.c_str(),
                   static_cast<int>(fragment.size()),
                   fragment.data());
      std::exit(1);
   }
}

std::optional<InputRequest> parseOk(const nlohmann::json& arguments,
                                    const char* file,
                                    int line) {
   std::string error;
   std::optional<InputRequest> request =
       sdl3mcp::parseInputArguments(arguments, error);
   if (!request.has_value() || !error.empty()) {
      std::fprintf(stderr, "%s:%d: unexpected error \"%s\"\n", file, line,
                   error.c_str());
      std::exit(1);
   }
   return request;
}

void expectError(const nlohmann::json& arguments,
                 std::string_view fragment,
                 const char* file,
                 int line) {
   std::string error;
   const std::optional<InputRequest> request =
       sdl3mcp::parseInputArguments(arguments, error);
   if (request.has_value()) {
      std::fprintf(stderr, "%s:%d: expected an error matching \"%.*s\"\n", file,
                   line, static_cast<int>(fragment.size()), fragment.data());
      std::exit(1);
   }
   checkErrorText(error, fragment, file, line);
}

void expectIndexedError(const nlohmann::json& arguments,
                        std::size_t index,
                        std::string_view fragment,
                        const char* file,
                        int line) {
   const std::string prefix = "actions[" + std::to_string(index) + "]: ";
   std::string error;
   const std::optional<InputRequest> request =
       sdl3mcp::parseInputArguments(arguments, error);
   if (request.has_value() || error.compare(0, prefix.size(), prefix) != 0) {
      std::fprintf(stderr, "%s:%d: expected an error prefixed \"%s\", got \"%s\"\n",
                   file, line, prefix.c_str(), error.c_str());
      std::exit(1);
   }
   checkErrorText(error, fragment, file, line);
}

#define PARSE_OK(arguments) parseOk((arguments), __FILE__, __LINE__)
#define EXPECT_ERROR(arguments, fragment) \
   expectError((arguments), (fragment), __FILE__, __LINE__)
#define EXPECT_INDEXED_ERROR(arguments, index, fragment) \
   expectIndexedError((arguments), (index), (fragment), __FILE__, __LINE__)

std::optional<CaptureArguments> parseCaptureOk(const nlohmann::json& arguments,
                                               const char* file,
                                               int line) {
   std::string error;
   std::optional<CaptureArguments> parsed =
       sdl3mcp::parseCaptureArguments(arguments, error);
   if (!parsed.has_value() || !error.empty()) {
      std::fprintf(stderr, "%s:%d: unexpected capture error \"%s\"\n", file, line,
                   error.c_str());
      std::exit(1);
   }
   return parsed;
}

void expectCaptureError(const nlohmann::json& arguments,
                        std::string_view fragment,
                        const char* file,
                        int line) {
   std::string error;
   const std::optional<CaptureArguments> parsed =
       sdl3mcp::parseCaptureArguments(arguments, error);
   if (parsed.has_value()) {
      std::fprintf(stderr, "%s:%d: expected a capture error matching \"%.*s\"\n",
                   file, line, static_cast<int>(fragment.size()), fragment.data());
      std::exit(1);
   }
   checkErrorText(error, fragment, file, line);
}

#define CAPTURE_OK(arguments) parseCaptureOk((arguments), __FILE__, __LINE__)
#define CAPTURE_ERROR(arguments, fragment) \
   expectCaptureError((arguments), (fragment), __FILE__, __LINE__)

nlohmann::json regionJson(int x, int y, int width, int height) {
   return nlohmann::json{{"x", x}, {"y", y}, {"width", width}, {"height", height}};
}

}  // namespace

TEST_CASE("input key defaults") {
   const std::optional<InputRequest> request =
       PARSE_OK(single({{"type", "key"}, {"key", "a"}}));
   CHECK(request->batch.actions.size() == 1);
   const auto& key = std::get<KeyInput>(request->batch.actions.at(0));
   CHECK(key.key == "a");
   CHECK(key.action == KeyAction::Press);
   CHECK(key.modifiers.empty());
}

TEST_CASE("input key actions and modifiers") {
   const auto down =
       PARSE_OK(single({{"type", "key"}, {"key", "space"}, {"action", "down"}}));
   CHECK(std::get<KeyInput>(down->batch.actions.at(0)).action == KeyAction::Down);

   const auto up =
       PARSE_OK(single({{"type", "key"}, {"key", "space"}, {"action", "up"}}));
   CHECK(std::get<KeyInput>(up->batch.actions.at(0)).action == KeyAction::Up);

   const auto request = PARSE_OK(single({{"type", "key"},
                                         {"key", "s"},
                                         {"action", "press"},
                                         {"modifiers", {"shift", "ctrl", "alt", "gui"}}}));
   const auto& key = std::get<KeyInput>(request->batch.actions.at(0));
   REQUIRE(key.modifiers.size() == 4);
   CHECK(key.modifiers.at(0) == Modifier::Shift);
   CHECK(key.modifiers.at(1) == Modifier::Ctrl);
   CHECK(key.modifiers.at(2) == Modifier::Alt);
   CHECK(key.modifiers.at(3) == Modifier::Gui);
}

TEST_CASE("input key errors") {
   EXPECT_INDEXED_ERROR(single({{"type", "key"}, {"key", "a"}, {"modifiers", {"hyper"}}}),
                        0, "unknown modifier \"hyper\"");
   EXPECT_INDEXED_ERROR(single({{"type", "key"},
                                {"key", "a"},
                                {"modifiers", {"ctrl", "shift", "alt", "gui", "ctrl"}}}),
                        0, "modifiers has 5 entries; the maximum is 4");
   EXPECT_INDEXED_ERROR(single({{"type", "key"}, {"key", "a"}, {"modifiers", "ctrl"}}),
                        0, "modifiers must be an array");
   EXPECT_INDEXED_ERROR(single({{"type", "key"}, {"key", "a"}, {"modifiers", {1}}}),
                        0, "modifiers must be strings");
   EXPECT_INDEXED_ERROR(
       single({{"type", "key"}, {"key", "a"}, {"action", "down"}, {"modifiers", {"ctrl"}}}),
       0, "modifiers are only valid for press");
   EXPECT_INDEXED_ERROR(single({{"type", "key"}, {"key", "a"}, {"repeat", 3}}),
                        0, "unknown property \"repeat\"");
   EXPECT_INDEXED_ERROR(single({{"type", "key"}}), 0, "key is required");
   EXPECT_INDEXED_ERROR(single({{"type", "key"}, {"key", 5}}), 0, "key must be a string");
   EXPECT_INDEXED_ERROR(single({{"type", "key"}, {"key", "a"}, {"action", "hold"}}),
                        0, "unknown key action \"hold\"");
   EXPECT_INDEXED_ERROR(single({{"type", "key"}, {"key", "a"}, {"action", 1}}),
                        0, "action must be a string");
   EXPECT_INDEXED_ERROR(single({{"type", "key"}, {"key", ""}}), 0,
                        "key must not be empty");
   EXPECT_INDEXED_ERROR(single({{"type", "key"}, {"key", std::string(65, 'k')}}), 0,
                        "key is 65 bytes; the maximum is 64");
}

TEST_CASE("input text") {
   const auto one = PARSE_OK(single({{"type", "text"}, {"text", "a"}}));
   CHECK(std::get<TextInput>(one->batch.actions.at(0)).text == "a");

   EXPECT_INDEXED_ERROR(single({{"type", "text"}, {"text", ""}}), 0,
                        "text must not be empty");
   EXPECT_INDEXED_ERROR(single({{"type", "text"}, {"text", std::string(1025, 'x')}}),
                        0, "text is 1025 bytes; the maximum is 1024");
   EXPECT_INDEXED_ERROR(single({{"type", "text"}, {"text", 7}}), 0,
                        "text must be a string");
}

TEST_CASE("input mouse move") {
   const auto absolute =
       PARSE_OK(single({{"type", "mouse_move"}, {"x", 10}, {"y", 20}}));
   const auto& move = std::get<MouseMoveInput>(absolute->batch.actions.at(0));
   CHECK(move.mode == MouseMoveInput::Mode::Absolute);
   CHECK(move.x == 10.0F);
   CHECK(move.y == 20.0F);

   const auto relative =
       PARSE_OK(single({{"type", "mouse_move"}, {"dx", -3}, {"dy", 4}}));
   const auto& delta = std::get<MouseMoveInput>(relative->batch.actions.at(0));
   CHECK(delta.mode == MouseMoveInput::Mode::Relative);

   EXPECT_INDEXED_ERROR(
       single({{"type", "mouse_move"}, {"x", 1}, {"y", 2}, {"dx", 3}}), 0,
       "provide either x and y or dx and dy, not both");
   EXPECT_INDEXED_ERROR(single({{"type", "mouse_move"}, {"x", 1}}), 0,
                        "x and y must be provided together");
   EXPECT_INDEXED_ERROR(single({{"type", "mouse_move"}}), 0,
                        "mouse_move requires x and y or dx and dy");
}

TEST_CASE("input mouse button") {
   const auto positioned = PARSE_OK(single({{"type", "mouse_button"},
                                            {"button", "right"},
                                            {"action", "down"},
                                            {"x", 12},
                                            {"y", 34},
                                            {"clicks", 2}}));
   const auto& input = std::get<MouseButtonInput>(positioned->batch.actions.at(0));
   CHECK(input.button == MouseButton::Right);
   CHECK(input.action == MouseButtonAction::Down);
   CHECK(input.clicks == 2);

   EXPECT_INDEXED_ERROR(single({{"type", "mouse_button"}, {"button", "left"}, {"clicks", 0}}),
                        0, "clicks must be between 1 and 3");
   EXPECT_INDEXED_ERROR(single({{"type", "mouse_button"}, {"button", "left"}, {"clicks", 4}}),
                        0, "clicks must be between 1 and 3");
   EXPECT_INDEXED_ERROR(single({{"type", "mouse_button"}, {"button", "back"}}), 0,
                        "unknown mouse button \"back\"");
}

TEST_CASE("input scroll and release all") {
   const auto both = PARSE_OK(single({{"type", "scroll"}, {"x", 2}, {"y", -3}}));
   const auto& full = std::get<ScrollInput>(both->batch.actions.at(0));
   CHECK(full.x == 2.0F);
   CHECK(full.y == -3.0F);

   const auto release = PARSE_OK(single({{"type", "release_all"}}));
   CHECK(std::holds_alternative<ReleaseAllInput>(release->batch.actions.at(0)));

   EXPECT_INDEXED_ERROR(single({{"type", "scroll"}, {"x", 1}}), 0, "y is required");
}

TEST_CASE("input batch rules") {
   EXPECT_ERROR(argumentsWith(nlohmann::json::array()), "at least one action");
   EXPECT_ERROR(argumentsWith(repeatedKeyActions(257)), "the maximum is 256");

   const auto full = PARSE_OK(argumentsWith(repeatedKeyActions(256)));
   CHECK(full->batch.actions.size() == 256);

   EXPECT_INDEXED_ERROR(single({{"type", "tap"}}), 0, "unknown action type \"tap\"");
   EXPECT_ERROR(nlohmann::json::object(), "actions is required");
   EXPECT_ERROR(nlohmann::json::array(), "arguments must be a JSON object");
   EXPECT_ERROR(argumentsWith("key"), "actions must be an array");
   const nlohmann::json withExtra = {{"actions", nlohmann::json::array()}, {"batch", 1}};
   EXPECT_ERROR(withExtra, "unknown property \"batch\"");
}

TEST_CASE("input observation arguments") {
   const auto defaults = PARSE_OK(single({{"type", "release_all"}}));
   CHECK(!defaults->region.has_value());

   const nlohmann::json observed{
       {"actions", nlohmann::json::array({{{"type", "release_all"}}})},
       {"region", regionJson(1, 2, 3, 4)},
   };
   const auto requested = PARSE_OK(observed);
   REQUIRE(requested->region.has_value());
   CHECK(requested->region->x == 1);
   CHECK(requested->region->y == 2);
   CHECK(requested->region->width == 3);
   CHECK(requested->region->height == 4);

   const nlohmann::json releaseOnly{
       {"actions", nlohmann::json::array({{{"type", "release_all"}}})},
   };

   nlohmann::json notAnObject = releaseOnly;
   notAnObject["region"] = 5;
   EXPECT_ERROR(notAnObject, "region must be an object");

   nlohmann::json missingHeight = regionJson(1, 2, 3, 4);
   missingHeight.erase("height");
   nlohmann::json missingHeightArguments = releaseOnly;
   missingHeightArguments["region"] = missingHeight;
   EXPECT_ERROR(missingHeightArguments, "height is required");

   nlohmann::json negativeX = regionJson(-1, 2, 3, 4);
   nlohmann::json negativeXArguments = releaseOnly;
   negativeXArguments["region"] = negativeX;
   EXPECT_ERROR(negativeXArguments, "region x and y must be at least 0");

   nlohmann::json zeroWidth = regionJson(1, 2, 0, 4);
   nlohmann::json zeroWidthArguments = releaseOnly;
   zeroWidthArguments["region"] = zeroWidth;
   EXPECT_ERROR(zeroWidthArguments, "region width and height must be at least 1");

   nlohmann::json extraRegion = regionJson(1, 2, 3, 4);
   extraRegion["zoom"] = 1;
   nlohmann::json extraRegionArguments = releaseOnly;
   extraRegionArguments["region"] = extraRegion;
   EXPECT_ERROR(extraRegionArguments, "unknown property \"zoom\"");
}

TEST_CASE("input rejects the removed timeout argument") {
   nlohmann::json arguments = single({{"type", "release_all"}});
   arguments["timeout_ms"] = 2000;
   EXPECT_ERROR(arguments, "unknown property \"timeout_ms\"");
}

TEST_CASE("input all action types in one batch") {
   nlohmann::json actions = nlohmann::json::array();
   actions.push_back(nlohmann::json{{"type", "key"}, {"key", "a"}});
   actions.push_back(nlohmann::json{{"type", "text"}, {"text", "hi"}});
   actions.push_back(nlohmann::json{{"type", "mouse_move"}, {"x", 1}, {"y", 2}});
   actions.push_back(nlohmann::json{{"type", "mouse_button"}, {"button", "left"}});
   actions.push_back(nlohmann::json{{"type", "scroll"}, {"y", 1}});
   actions.push_back(nlohmann::json{{"type", "release_all"}});

   const std::optional<InputRequest> request = PARSE_OK(argumentsWith(actions));
   CHECK(request->batch.actions.size() == 6);
   CHECK(std::holds_alternative<KeyInput>(request->batch.actions.at(0)));
   CHECK(std::holds_alternative<TextInput>(request->batch.actions.at(1)));
   CHECK(std::holds_alternative<MouseMoveInput>(request->batch.actions.at(2)));
   CHECK(std::holds_alternative<MouseButtonInput>(request->batch.actions.at(3)));
   CHECK(std::holds_alternative<ScrollInput>(request->batch.actions.at(4)));
   CHECK(std::holds_alternative<ReleaseAllInput>(request->batch.actions.at(5)));
}

TEST_CASE("capture arguments accept only a region") {
   const auto empty = CAPTURE_OK(nlohmann::json::object());
   CHECK(!empty->region.has_value());

   const auto cropped =
       CAPTURE_OK(nlohmann::json({{"region", regionJson(0, 0, 640, 360)}}));
   REQUIRE(cropped->region.has_value());
   CHECK(cropped->region->width == 640);
   CHECK(cropped->region->height == 360);

   CAPTURE_ERROR(nlohmann::json({{"timeout_ms", 2000}}),
                 "unknown property \"timeout_ms\"");
   CAPTURE_ERROR(nlohmann::json({{"region", 5}}), "region must be an object");
   CAPTURE_ERROR(nlohmann::json({{"region", regionJson(0, 0, 0, 1)}}),
                 "region width and height must be at least 1");
}

TEST_CASE("capture and input share one region parser") {
   /* A region the capture parser rejects is rejected identically by input. */
   const nlohmann::json badRegion = {{"x", 0}, {"y", 0}, {"width", -2}, {"height", 1}};
   CAPTURE_ERROR(nlohmann::json({{"region", badRegion}}),
                 "region width and height must be at least 1");
   nlohmann::json inputArguments = single({{"type", "release_all"}});
   inputArguments["region"] = badRegion;
   EXPECT_ERROR(inputArguments, "region width and height must be at least 1");
}
