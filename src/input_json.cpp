#include "input_json.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace sdl3mcp
{
namespace
{

using nlohmann::json;

std::string actionPrefix(std::size_t index)
{
    return "actions[" + std::to_string(index) + "]: ";
}

/*
  Rejects the first member the closed schema for this action type does not
  declare. Iteration order is the object's key order, so the reported name is
  stable for a given request.
*/
std::optional<std::string> rejectUnknownProperties(const json &value,
                                                   std::span<const std::string_view> allowed)
{
    for (auto entry = value.begin(); entry != value.end(); ++entry) {
        const std::string_view name = entry.key();
        bool isAllowed = false;
        for (const std::string_view candidate : allowed) {
            if (name == candidate) {
                isAllowed = true;
                break;
            }
        }
        if (!isAllowed) {
            return "unknown property \"" + entry.key() + "\"";
        }
    }
    return std::nullopt;
}

std::optional<std::string> readRequiredString(const json &value, const char *name,
                                              std::string &error)
{
    const auto entry = value.find(name);
    if (entry == value.end()) {
        error = std::string(name) + " is required";
        return std::nullopt;
    }
    if (!entry->is_string()) {
        error = std::string(name) + " must be a string";
        return std::nullopt;
    }
    return entry->get<std::string>();
}

/* A finite number as JSON writes it; the cast to float happens at the call site. */
std::optional<double> readNumber(const json &value, const char *name, std::string &error)
{
    const auto entry = value.find(name);
    if (entry == value.end() || !entry->is_number()) {
        error = std::string(name) + " must be a number";
        return std::nullopt;
    }
    const double number = entry->get<double>();
    if (!std::isfinite(number)) {
        error = std::string(name) + " must be finite";
        return std::nullopt;
    }
    return number;
}

std::optional<double> readRequiredNumber(const json &value, const char *name,
                                         std::string &error)
{
    if (value.find(name) == value.end()) {
        error = std::string(name) + " is required";
        return std::nullopt;
    }
    return readNumber(value, name, error);
}

std::optional<KeyAction> toKeyAction(std::string_view name)
{
    if (name == "down") {
        return KeyAction::Down;
    }
    if (name == "up") {
        return KeyAction::Up;
    }
    if (name == "press") {
        return KeyAction::Press;
    }
    return std::nullopt;
}

std::optional<MouseButton> toMouseButton(std::string_view name)
{
    if (name == "left") {
        return MouseButton::Left;
    }
    if (name == "middle") {
        return MouseButton::Middle;
    }
    if (name == "right") {
        return MouseButton::Right;
    }
    if (name == "x1") {
        return MouseButton::X1;
    }
    if (name == "x2") {
        return MouseButton::X2;
    }
    return std::nullopt;
}

std::optional<MouseButtonAction> toMouseButtonAction(std::string_view name)
{
    if (name == "down") {
        return MouseButtonAction::Down;
    }
    if (name == "up") {
        return MouseButtonAction::Up;
    }
    if (name == "click") {
        return MouseButtonAction::Click;
    }
    return std::nullopt;
}

std::optional<InputAction> parseKey(const json &value, std::string &error)
{
    KeyInput input;

    const std::optional<std::string> key = readRequiredString(value, "key", error);
    if (!key.has_value()) {
        return std::nullopt;
    }
    input.key = *key;

    if (const auto entry = value.find("action"); entry != value.end()) {
        if (!entry->is_string()) {
            error = "action must be a string";
            return std::nullopt;
        }
        const std::string name = entry->get<std::string>();
        const std::optional<KeyAction> action = toKeyAction(name);
        if (!action.has_value()) {
            error = "unknown key action \"" + name + "\"";
            return std::nullopt;
        }
        input.action = *action;
    }

    if (const auto entry = value.find("modifiers"); entry != value.end()) {
        if (!entry->is_array()) {
            error = "modifiers must be an array";
            return std::nullopt;
        }
        if (entry->size() > kMaxModifiersPerKey) {
            error = "modifiers has " + std::to_string(entry->size())
                    + " entries; the maximum is " + std::to_string(kMaxModifiersPerKey);
            return std::nullopt;
        }
        for (const json &item : *entry) {
            if (!item.is_string()) {
                error = "modifiers must be strings";
                return std::nullopt;
            }
            const std::string name = item.get<std::string>();
            const std::optional<Modifier> modifier = parseModifier(name);
            if (!modifier.has_value()) {
                error = "unknown modifier \"" + name + "\"";
                return std::nullopt;
            }
            input.modifiers.push_back(*modifier);
        }
    }

    return InputAction{std::move(input)};
}

std::optional<InputAction> parseText(const json &value, std::string &error)
{
    const std::optional<std::string> text = readRequiredString(value, "text", error);
    if (!text.has_value()) {
        return std::nullopt;
    }
    return InputAction{TextInput{*text}};
}

/*
  The two wire forms are exclusive: x/y are absolute window points, dx/dy are
  relative deltas. A half pair or a mix of both would leave the mode ambiguous,
  so every such shape is refused instead of being guessed at.
*/
std::optional<InputAction> parseMouseMove(const json &value, std::string &error)
{
    const bool hasX = value.contains("x");
    const bool hasY = value.contains("y");
    const bool hasDx = value.contains("dx");
    const bool hasDy = value.contains("dy");

    if ((hasX && hasDx) || (hasY && hasDy)) {
        error = "provide either x and y or dx and dy, not both";
        return std::nullopt;
    }
    if (hasX != hasY) {
        error = "x and y must be provided together";
        return std::nullopt;
    }
    if (hasDx != hasDy) {
        error = "dx and dy must be provided together";
        return std::nullopt;
    }
    if (!hasX && !hasDx) {
        error = "mouse_move requires x and y or dx and dy";
        return std::nullopt;
    }

    const std::optional<double> x = readNumber(value, hasX ? "x" : "dx", error);
    if (!x.has_value()) {
        return std::nullopt;
    }
    const std::optional<double> y = readNumber(value, hasX ? "y" : "dy", error);
    if (!y.has_value()) {
        return std::nullopt;
    }

    MouseMoveInput input;
    input.mode = hasX ? MouseMoveInput::Mode::Absolute : MouseMoveInput::Mode::Relative;
    input.x = static_cast<float>(*x);
    input.y = static_cast<float>(*y);
    return InputAction{input};
}

std::optional<InputAction> parseMouseButton(const json &value, std::string &error)
{
    MouseButtonInput input;

    const std::optional<std::string> button = readRequiredString(value, "button", error);
    if (!button.has_value()) {
        return std::nullopt;
    }
    const std::optional<MouseButton> parsedButton = toMouseButton(*button);
    if (!parsedButton.has_value()) {
        error = "unknown mouse button \"" + *button + "\"";
        return std::nullopt;
    }
    input.button = *parsedButton;

    if (const auto entry = value.find("action"); entry != value.end()) {
        if (!entry->is_string()) {
            error = "action must be a string";
            return std::nullopt;
        }
        const std::string name = entry->get<std::string>();
        const std::optional<MouseButtonAction> action = toMouseButtonAction(name);
        if (!action.has_value()) {
            error = "unknown mouse button action \"" + name + "\"";
            return std::nullopt;
        }
        input.action = *action;
    }

    const bool hasX = value.contains("x");
    const bool hasY = value.contains("y");
    if (hasX != hasY) {
        error = "x and y must be provided together";
        return std::nullopt;
    }
    if (hasX) {
        const std::optional<double> x = readNumber(value, "x", error);
        if (!x.has_value()) {
            return std::nullopt;
        }
        const std::optional<double> y = readNumber(value, "y", error);
        if (!y.has_value()) {
            return std::nullopt;
        }
        input.x = static_cast<float>(*x);
        input.y = static_cast<float>(*y);
    }

    /*
      Only the JSON type is checked here; the 1..3 range belongs to
      validateStructure so both entry points share one set of model rules.
    */
    if (const auto entry = value.find("clicks"); entry != value.end()) {
        if (!entry->is_number_integer()) {
            error = "clicks must be an integer";
            return std::nullopt;
        }
        const double clicks = entry->get<double>();
        if (clicks < static_cast<double>(std::numeric_limits<int>::min())
            || clicks > static_cast<double>(std::numeric_limits<int>::max())) {
            error = "clicks is out of range";
            return std::nullopt;
        }
        input.clicks = static_cast<int>(clicks);
    }

    return InputAction{input};
}

std::optional<InputAction> parseScroll(const json &value, std::string &error)
{
    ScrollInput input;

    const std::optional<double> y = readRequiredNumber(value, "y", error);
    if (!y.has_value()) {
        return std::nullopt;
    }
    input.y = static_cast<float>(*y);

    if (value.contains("x")) {
        const std::optional<double> x = readNumber(value, "x", error);
        if (!x.has_value()) {
            return std::nullopt;
        }
        input.x = static_cast<float>(*x);
    }

    return InputAction{input};
}

std::optional<InputAction> parseReleaseAll(const json &, std::string &)
{
    return InputAction{ReleaseAllInput{}};
}

using ActionParser = std::optional<InputAction> (*)(const json &value, std::string &error);

struct ActionType
{
    std::string_view name;
    std::span<const std::string_view> properties;
    ActionParser parse;
};

constexpr std::array<std::string_view, 4> kKeyProperties{"type", "key", "action", "modifiers"};
constexpr std::array<std::string_view, 2> kTextProperties{"type", "text"};
constexpr std::array<std::string_view, 5> kMouseMoveProperties{"type", "x", "y", "dx", "dy"};
constexpr std::array<std::string_view, 6> kMouseButtonProperties{"type", "button", "action",
                                                                 "x", "y", "clicks"};
constexpr std::array<std::string_view, 3> kScrollProperties{"type", "x", "y"};
constexpr std::array<std::string_view, 1> kReleaseAllProperties{"type"};
constexpr std::array<std::string_view, 1> kArgumentsProperties{"actions"};

constexpr std::array<ActionType, 6> kActionTypes{{
    {"key", kKeyProperties, parseKey},
    {"text", kTextProperties, parseText},
    {"mouse_move", kMouseMoveProperties, parseMouseMove},
    {"mouse_button", kMouseButtonProperties, parseMouseButton},
    {"scroll", kScrollProperties, parseScroll},
    {"release_all", kReleaseAllProperties, parseReleaseAll},
}};

std::optional<InputAction> parseAction(const json &value, std::string &error)
{
    if (!value.is_object()) {
        error = "action must be an object";
        return std::nullopt;
    }

    const auto typeEntry = value.find("type");
    if (typeEntry == value.end()) {
        error = "type is required";
        return std::nullopt;
    }
    if (!typeEntry->is_string()) {
        error = "type must be a string";
        return std::nullopt;
    }
    const std::string_view type = typeEntry->get_ref<const std::string &>();

    for (const ActionType &candidate : kActionTypes) {
        if (candidate.name != type) {
            continue;
        }
        if (const std::optional<std::string> unknown =
                rejectUnknownProperties(value, candidate.properties);
            unknown.has_value()) {
            error = *unknown;
            return std::nullopt;
        }
        return candidate.parse(value, error);
    }

    error = "unknown action type \"" + std::string(type) + "\"";
    return std::nullopt;
}

} // namespace

std::optional<InputBatch> parseInputArguments(const json &arguments, std::string &error)
{
    error.clear();

    if (!arguments.is_object()) {
        error = "arguments must be a JSON object";
        return std::nullopt;
    }
    if (const std::optional<std::string> unknown =
            rejectUnknownProperties(arguments, kArgumentsProperties);
        unknown.has_value()) {
        error = *unknown;
        return std::nullopt;
    }

    const auto actionsEntry = arguments.find("actions");
    if (actionsEntry == arguments.end()) {
        error = "actions is required";
        return std::nullopt;
    }
    if (!actionsEntry->is_array()) {
        error = "actions must be an array";
        return std::nullopt;
    }

    InputBatch batch;
    batch.actions.reserve(actionsEntry->size());
    for (std::size_t index = 0; index < actionsEntry->size(); ++index) {
        std::string actionError;
        std::optional<InputAction> action = parseAction((*actionsEntry)[index], actionError);
        if (!action.has_value()) {
            error = actionPrefix(index) + actionError;
            return std::nullopt;
        }
        batch.actions.push_back(std::move(*action));
    }

    /*
      Counts, byte limits, modifier placement, and the clicks range are model
      rules, so the parser does not restate them; validateStructure is the one
      authority and its text already names the action index.
    */
    if (const std::optional<std::string> structureError = validateStructure(batch);
        structureError.has_value()) {
        error = *structureError;
        return std::nullopt;
    }
    return batch;
}

} // namespace sdl3mcp
