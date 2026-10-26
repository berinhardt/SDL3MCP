/*
  The sdl_input argument parser is pure, so every schema rule, default, and
  action index can be pinned without a thread, a window, or a queue. These tests
  mirror the published tool schema: a valid batch of all six action types, each
  rejected field shape, and the exact index a client is told to fix.
*/

#include "input_json.hpp"

#include "test_util.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace
{

using sdl3mcp::InputBatch;
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

/* A call with exactly one action, which is the shape most checks need. */
nlohmann::json single(const nlohmann::json &action)
{
    return nlohmann::json{{"actions", nlohmann::json::array({action})}};
}

nlohmann::json argumentsWith(const nlohmann::json &actions)
{
    return nlohmann::json{{"actions", actions}};
}

nlohmann::json repeatedKeyActions(std::size_t count)
{
    nlohmann::json actions = nlohmann::json::array();
    for (std::size_t index = 0; index < count; ++index) {
        actions.push_back(nlohmann::json{{"type", "key"}, {"key", "a"}});
    }
    return actions;
}

void checkErrorText(const std::string &error,
                    std::string_view fragment,
                    const char *file,
                    int line)
{
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

std::optional<InputBatch> parseOk(const nlohmann::json &arguments,
                                  const char *file,
                                  int line)
{
    std::string error;
    std::optional<InputBatch> batch = sdl3mcp::parseInputArguments(arguments, error);
    if (!batch.has_value() || !error.empty()) {
        std::fprintf(stderr, "%s:%d: unexpected error \"%s\"\n", file, line, error.c_str());
        std::exit(1);
    }
    return batch;
}

void expectError(const nlohmann::json &arguments,
                 std::string_view fragment,
                 const char *file,
                 int line)
{
    std::string error;
    const std::optional<InputBatch> batch = sdl3mcp::parseInputArguments(arguments, error);
    if (batch.has_value()) {
        std::fprintf(stderr,
                     "%s:%d: expected an error matching \"%.*s\"\n",
                     file,
                     line,
                     static_cast<int>(fragment.size()),
                     fragment.data());
        std::exit(1);
    }
    checkErrorText(error, fragment, file, line);
}

void expectIndexedError(const nlohmann::json &arguments,
                        std::size_t index,
                        std::string_view fragment,
                        const char *file,
                        int line)
{
    const std::string prefix = "actions[" + std::to_string(index) + "]: ";
    std::string error;
    const std::optional<InputBatch> batch = sdl3mcp::parseInputArguments(arguments, error);
    if (batch.has_value() || error.compare(0, prefix.size(), prefix) != 0) {
        std::fprintf(stderr,
                     "%s:%d: expected an error prefixed \"%s\", got \"%s\"\n",
                     file,
                     line,
                     prefix.c_str(),
                     error.c_str());
        std::exit(1);
    }
    checkErrorText(error, fragment, file, line);
}

#define PARSE_OK(arguments) parseOk((arguments), __FILE__, __LINE__)
#define EXPECT_ERROR(arguments, fragment) \
    expectError((arguments), (fragment), __FILE__, __LINE__)
#define EXPECT_INDEXED_ERROR(arguments, index, fragment) \
    expectIndexedError((arguments), (index), (fragment), __FILE__, __LINE__)

void testKeyDefaults()
{
    const std::optional<InputBatch> batch = PARSE_OK(single({{"type", "key"}, {"key", "a"}}));
    CHECK(batch->actions.size() == 1);
    const auto &key = std::get<KeyInput>(batch->actions.at(0));
    CHECK(key.key == "a");
    CHECK(key.action == KeyAction::Press);
    CHECK(key.modifiers.empty());
}

void testKeyActions()
{
    const auto down =
        PARSE_OK(single({{"type", "key"}, {"key", "space"}, {"action", "down"}}));
    const auto &downKey = std::get<KeyInput>(down->actions.at(0));
    CHECK(downKey.key == "space");
    CHECK(downKey.action == KeyAction::Down);

    const auto up = PARSE_OK(single({{"type", "key"}, {"key", "space"}, {"action", "up"}}));
    CHECK(std::get<KeyInput>(up->actions.at(0)).action == KeyAction::Up);

    const auto press =
        PARSE_OK(single({{"type", "key"}, {"key", "a"}, {"action", "press"}}));
    CHECK(std::get<KeyInput>(press->actions.at(0)).action == KeyAction::Press);
}

void testKeyModifiers()
{
    const auto batch = PARSE_OK(single({{"type", "key"},
                                        {"key", "s"},
                                        {"action", "press"},
                                        {"modifiers", {"shift", "ctrl", "alt", "gui"}}}));
    const auto &key = std::get<KeyInput>(batch->actions.at(0));
    CHECK(key.modifiers.size() == 4);
    CHECK(key.modifiers.at(0) == Modifier::Shift);
    CHECK(key.modifiers.at(1) == Modifier::Ctrl);
    CHECK(key.modifiers.at(2) == Modifier::Alt);
    CHECK(key.modifiers.at(3) == Modifier::Gui);
}

void testKeyErrors()
{
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
    /* The parser passes modifiers on a held key through; validateStructure refuses them. */
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
    EXPECT_INDEXED_ERROR(single({{"type", "key"}, {"key", ""}}), 0, "key must not be empty");
    EXPECT_INDEXED_ERROR(single({{"type", "key"}, {"key", std::string(65, 'k')}}),
                         0, "key is 65 bytes; the maximum is 64");
}

void testText()
{
    const auto one = PARSE_OK(single({{"type", "text"}, {"text", "a"}}));
    CHECK(std::get<TextInput>(one->actions.at(0)).text == "a");

    const auto longest = PARSE_OK(single({{"type", "text"}, {"text", std::string(1024, 'x')}}));
    CHECK(std::get<TextInput>(longest->actions.at(0)).text.size() == 1024);

    EXPECT_INDEXED_ERROR(single({{"type", "text"}, {"text", ""}}),
                         0, "text must not be empty");
    EXPECT_INDEXED_ERROR(single({{"type", "text"}, {"text", std::string(1025, 'x')}}),
                         0, "text is 1025 bytes; the maximum is 1024");
    EXPECT_INDEXED_ERROR(single({{"type", "text"}, {"text", 7}}),
                         0, "text must be a string");
    EXPECT_INDEXED_ERROR(single({{"type", "text"}}), 0, "text is required");
    EXPECT_INDEXED_ERROR(single({{"type", "text"}, {"text", "a"}, {"value", "b"}}),
                         0, "unknown property \"value\"");
}

void testMouseMove()
{
    const auto absolute = PARSE_OK(single({{"type", "mouse_move"}, {"x", 10}, {"y", 20}}));
    const auto &move = std::get<MouseMoveInput>(absolute->actions.at(0));
    CHECK(move.mode == MouseMoveInput::Mode::Absolute);
    CHECK(move.x == 10.0F);
    CHECK(move.y == 20.0F);

    const auto relative = PARSE_OK(single({{"type", "mouse_move"}, {"dx", -3}, {"dy", 4}}));
    const auto &delta = std::get<MouseMoveInput>(relative->actions.at(0));
    CHECK(delta.mode == MouseMoveInput::Mode::Relative);
    CHECK(delta.x == -3.0F);
    CHECK(delta.y == 4.0F);

    EXPECT_INDEXED_ERROR(
        single({{"type", "mouse_move"}, {"x", 1}, {"y", 2}, {"dx", 3}}),
        0, "provide either x and y or dx and dy, not both");
    EXPECT_INDEXED_ERROR(single({{"type", "mouse_move"}, {"x", 1}}),
                         0, "x and y must be provided together");
    EXPECT_INDEXED_ERROR(single({{"type", "mouse_move"}, {"y", 2}}),
                         0, "x and y must be provided together");
    EXPECT_INDEXED_ERROR(single({{"type", "mouse_move"}, {"dx", 1}}),
                         0, "dx and dy must be provided together");
    EXPECT_INDEXED_ERROR(single({{"type", "mouse_move"}}),
                         0, "mouse_move requires x and y or dx and dy");
    EXPECT_INDEXED_ERROR(single({{"type", "mouse_move"}, {"x", 1}, {"y", 2}, {"button", "left"}}),
                         0, "unknown property \"button\"");
    EXPECT_INDEXED_ERROR(single({{"type", "mouse_move"}, {"x", "1"}, {"y", 2}}),
                         0, "x must be a number");
    EXPECT_INDEXED_ERROR(single({{"type", "mouse_move"},
                                 {"x", std::numeric_limits<double>::infinity()},
                                 {"y", 2}}),
                         0, "x must be finite");
}

void expectButton(const char *name, MouseButton expected)
{
    const std::optional<InputBatch> batch =
        PARSE_OK(single({{"type", "mouse_button"}, {"button", name}}));
    const auto &input = std::get<MouseButtonInput>(batch->actions.at(0));
    CHECK(input.button == expected);
    CHECK(input.action == MouseButtonAction::Click);
    CHECK(input.clicks == 1);
    CHECK(!input.x.has_value());
    CHECK(!input.y.has_value());
}

void expectButtonAction(const char *name, MouseButtonAction expected)
{
    const std::optional<InputBatch> batch = PARSE_OK(
        single({{"type", "mouse_button"}, {"button", "left"}, {"action", name}}));
    const auto &input = std::get<MouseButtonInput>(batch->actions.at(0));
    CHECK(input.action == expected);
}

void testMouseButton()
{
    expectButton("left", MouseButton::Left);
    expectButton("middle", MouseButton::Middle);
    expectButton("right", MouseButton::Right);
    expectButton("x1", MouseButton::X1);
    expectButton("x2", MouseButton::X2);

    expectButtonAction("down", MouseButtonAction::Down);
    expectButtonAction("up", MouseButtonAction::Up);
    expectButtonAction("click", MouseButtonAction::Click);

    const auto positioned = PARSE_OK(single({{"type", "mouse_button"},
                                             {"button", "right"},
                                             {"action", "down"},
                                             {"x", 12},
                                             {"y", 34},
                                             {"clicks", 2}}));
    const auto &input = std::get<MouseButtonInput>(positioned->actions.at(0));
    CHECK(input.button == MouseButton::Right);
    CHECK(input.action == MouseButtonAction::Down);
    CHECK(input.x.has_value());
    CHECK(*input.x == 12.0F);
    CHECK(input.y.has_value());
    CHECK(*input.y == 34.0F);
    CHECK(input.clicks == 2);

    const auto triple =
        PARSE_OK(single({{"type", "mouse_button"}, {"button", "left"}, {"clicks", 3}}));
    CHECK(std::get<MouseButtonInput>(triple->actions.at(0)).clicks == 3);

    EXPECT_INDEXED_ERROR(single({{"type", "mouse_button"}, {"button", "left"}, {"x", 1}}),
                         0, "x and y must be provided together");
    EXPECT_INDEXED_ERROR(single({{"type", "mouse_button"}, {"button", "left"}, {"y", 1}}),
                         0, "x and y must be provided together");
    EXPECT_INDEXED_ERROR(
        single({{"type", "mouse_button"}, {"button", "left"}, {"x", "1"}, {"y", 1}}),
        0, "x must be a number");
    EXPECT_INDEXED_ERROR(single({{"type", "mouse_button"}, {"button", "left"}, {"clicks", 0}}),
                         0, "clicks must be between 1 and 3");
    EXPECT_INDEXED_ERROR(single({{"type", "mouse_button"}, {"button", "left"}, {"clicks", 4}}),
                         0, "clicks must be between 1 and 3");
    EXPECT_INDEXED_ERROR(single({{"type", "mouse_button"}, {"button", "left"}, {"clicks", 1.5}}),
                         0, "clicks must be an integer");
    EXPECT_INDEXED_ERROR(single({{"type", "mouse_button"}, {"button", "left"}, {"clicks", "2"}}),
                         0, "clicks must be an integer");
    EXPECT_INDEXED_ERROR(single({{"type", "mouse_button"}, {"button", "back"}}),
                         0, "unknown mouse button \"back\"");
    EXPECT_INDEXED_ERROR(single({{"type", "mouse_button"}}), 0, "button is required");
    EXPECT_INDEXED_ERROR(single({{"type", "mouse_button"}, {"button", 1}}),
                         0, "button must be a string");
    EXPECT_INDEXED_ERROR(single({{"type", "mouse_button"}, {"button", "left"}, {"action", "hold"}}),
                         0, "unknown mouse button action \"hold\"");
    EXPECT_INDEXED_ERROR(
        single({{"type", "mouse_button"},
                {"button", "left"},
                {"modifiers", nlohmann::json::array()}}),
        0, "unknown property \"modifiers\"");
}

void testScroll()
{
    const auto both = PARSE_OK(single({{"type", "scroll"}, {"x", 2}, {"y", -3}}));
    const auto &full = std::get<ScrollInput>(both->actions.at(0));
    CHECK(full.x == 2.0F);
    CHECK(full.y == -3.0F);

    const auto vertical = PARSE_OK(single({{"type", "scroll"}, {"y", 1.5}}));
    const auto &partial = std::get<ScrollInput>(vertical->actions.at(0));
    CHECK(partial.x == 0.0F);
    CHECK(partial.y == 1.5F);

    EXPECT_INDEXED_ERROR(single({{"type", "scroll"}, {"x", 1}}), 0, "y is required");
    EXPECT_INDEXED_ERROR(single({{"type", "scroll"}, {"y", "1"}}), 0, "y must be a number");
    EXPECT_INDEXED_ERROR(single({{"type", "scroll"}, {"x", "1"}, {"y", 1}}),
                         0, "x must be a number");
    EXPECT_INDEXED_ERROR(single({{"type", "scroll"}, {"y", 1}, {"dx", 2}}),
                         0, "unknown property \"dx\"");
}

void testReleaseAll()
{
    const auto batch = PARSE_OK(single({{"type", "release_all"}}));
    CHECK(std::holds_alternative<ReleaseAllInput>(batch->actions.at(0)));

    EXPECT_INDEXED_ERROR(single({{"type", "release_all"}, {"count", 1}}),
                         0, "unknown property \"count\"");
}

void testBatchRules()
{
    EXPECT_ERROR(argumentsWith(nlohmann::json::array()), "at least one action");
    EXPECT_ERROR(argumentsWith(repeatedKeyActions(257)), "the maximum is 256");

    const auto one = PARSE_OK(argumentsWith(repeatedKeyActions(1)));
    CHECK(one->actions.size() == 1);
    const auto full = PARSE_OK(argumentsWith(repeatedKeyActions(256)));
    CHECK(full->actions.size() == 256);

    EXPECT_INDEXED_ERROR(single({{"type", "tap"}}), 0, "unknown action type \"tap\"");
    EXPECT_ERROR(nlohmann::json::object(), "actions is required");
    EXPECT_ERROR(nlohmann::json::array(), "arguments must be a JSON object");
    EXPECT_ERROR(nlohmann::json("actions"), "arguments must be a JSON object");
    EXPECT_ERROR(nlohmann::json(7), "arguments must be a JSON object");
    EXPECT_ERROR(argumentsWith("key"), "actions must be an array");
    const nlohmann::json withExtra = {{"actions", nlohmann::json::array()}, {"batch", 1}};
    EXPECT_ERROR(withExtra, "unknown property \"batch\"");

    EXPECT_INDEXED_ERROR(argumentsWith(nlohmann::json::array({nlohmann::json(5)})),
                         0, "action must be an object");
    EXPECT_INDEXED_ERROR(single({{"key", "a"}}), 0, "type is required");
    EXPECT_INDEXED_ERROR(single({{"type", 1}, {"key", "a"}}), 0, "type must be a string");
}

void testErrorIndices()
{
    nlohmann::json actions = nlohmann::json::array();
    actions.push_back(nlohmann::json{{"type", "key"}, {"key", "a"}});
    actions.push_back(nlohmann::json{{"type", "nope"}});
    EXPECT_INDEXED_ERROR(argumentsWith(actions), 1, "unknown action type \"nope\"");

    nlohmann::json three = nlohmann::json::array();
    three.push_back(nlohmann::json{{"type", "release_all"}});
    three.push_back(nlohmann::json{{"type", "text"}, {"text", "ok"}});
    three.push_back(nlohmann::json{{"type", "scroll"}});
    EXPECT_INDEXED_ERROR(argumentsWith(three), 2, "y is required");

    /* A stale error from an earlier call must not survive a successful parse. */
    std::string error = "stale";
    const auto batch = sdl3mcp::parseInputArguments(
        single({{"type", "key"}, {"key", "a"}}), error);
    CHECK(batch.has_value());
    CHECK(error.empty());
}

void testAllTypesInOneBatch()
{
    nlohmann::json actions = nlohmann::json::array();
    actions.push_back(nlohmann::json{{"type", "key"}, {"key", "a"}});
    actions.push_back(nlohmann::json{{"type", "text"}, {"text", "hi"}});
    actions.push_back(nlohmann::json{{"type", "mouse_move"}, {"x", 1}, {"y", 2}});
    actions.push_back(nlohmann::json{{"type", "mouse_button"}, {"button", "left"}});
    actions.push_back(nlohmann::json{{"type", "scroll"}, {"y", 1}});
    actions.push_back(nlohmann::json{{"type", "release_all"}});

    const std::optional<InputBatch> batch = PARSE_OK(argumentsWith(actions));
    CHECK(batch->actions.size() == 6);
    CHECK(std::holds_alternative<KeyInput>(batch->actions.at(0)));
    CHECK(std::holds_alternative<TextInput>(batch->actions.at(1)));
    CHECK(std::holds_alternative<MouseMoveInput>(batch->actions.at(2)));
    CHECK(std::holds_alternative<MouseButtonInput>(batch->actions.at(3)));
    CHECK(std::holds_alternative<ScrollInput>(batch->actions.at(4)));
    CHECK(std::holds_alternative<ReleaseAllInput>(batch->actions.at(5)));
}

} // namespace

int main()
{
    testKeyDefaults();
    testKeyActions();
    testKeyModifiers();
    testKeyErrors();
    testText();
    testMouseMove();
    testMouseButton();
    testScroll();
    testReleaseAll();
    testBatchRules();
    testErrorIndices();
    testAllTypesInOneBatch();

    std::puts("input_json_tests passed");
    return 0;
}
