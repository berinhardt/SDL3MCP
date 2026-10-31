#include "app_input.hpp"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace sdl3mcp
{
namespace
{

/* The replayer state a batch is applied against, shared by every action. */
struct BatchContext
{
    SDL_WindowID windowID;
    bool textInputActive;
    Uint64 timestamp;
    KeyReplayer &keyState;
    MouseReplayer &mouseState;
};

std::string actionPrefix(std::size_t index)
{
    return "actions[" + std::to_string(index) + "]: ";
}

/*
  Appends the events of one action. The caller hands in a fresh `warp`, so a
  move or a positioned button that warps sets it and every other action leaves
  it empty.
*/
bool appendActionEvents(const InputAction &action, BatchContext &context,
                        std::vector<SDL_Event> &events, std::optional<SDL_FPoint> &warp,
                        std::string &error)
{
    if (const auto *key = std::get_if<KeyInput>(&action)) {
        return context.keyState.apply(*key, context.timestamp, events, error);
    }
    if (const auto *text = std::get_if<TextInput>(&action)) {
        return buildTextEvent(*text, context.windowID, context.textInputActive,
                              context.timestamp, events, error);
    }
    if (const auto *move = std::get_if<MouseMoveInput>(&action)) {
        return context.mouseState.apply(*move, context.timestamp, events, warp, error);
    }
    if (const auto *button = std::get_if<MouseButtonInput>(&action)) {
        return context.mouseState.apply(*button, context.timestamp, events, warp, error);
    }
    if (const auto *scroll = std::get_if<ScrollInput>(&action)) {
        return context.mouseState.apply(*scroll, context.timestamp, events, error);
    }

    context.keyState.releaseAllKeys(context.timestamp, events);
    context.mouseState.releaseAllButtons(context.timestamp, events);
    return true;
}

} // namespace

bool buildBatchPlan(const InputBatch &batch, const KeyReplayer &keyReplayer,
                    const MouseReplayer &mouseReplayer, SDL_WindowID windowID,
                    bool textInputActive, Uint64 timestamp, AppliedBatchPlan &plan,
                    std::string &error)
{
    error.clear();

    KeyReplayer keyState = keyReplayer;
    MouseReplayer mouseState = mouseReplayer;
    BatchContext context{windowID, textInputActive, timestamp, keyState, mouseState};

    std::vector<std::vector<SDL_Event>> eventsByAction;
    std::vector<std::optional<SDL_FPoint>> warpsByAction;
    eventsByAction.reserve(batch.actions.size());
    warpsByAction.reserve(batch.actions.size());

    for (std::size_t index = 0; index < batch.actions.size(); ++index) {
        std::vector<SDL_Event> events;
        std::optional<SDL_FPoint> warp;
        if (!appendActionEvents(batch.actions[index], context, events, warp, error)) {
            error = actionPrefix(index) + error;
            return false;
        }
        eventsByAction.push_back(std::move(events));
        warpsByAction.push_back(warp);
    }

    plan.eventsByAction = std::move(eventsByAction);
    plan.warpsByAction = std::move(warpsByAction);
    plan.keyState = keyState;
    plan.mouseState = mouseState;
    return true;
}

} // namespace sdl3mcp
