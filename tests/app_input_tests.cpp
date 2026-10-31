/*
  buildBatchPlan is the pure half of the input path: one batch in, per-action
  events and the resulting replayer state out, with no SDL call that needs a
  window and no event ever pushed.

  These tests pin the parts AppIterate relies on: the action order survives, a
  warp is recorded only for an action that moves the cursor, text events keep
  pointing at the batch's own string, and a failure in any action leaves both
  the plan and the live replayers exactly as they were.
*/

#include "app_input.hpp"

#include "test_util.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

using sdl3mcp::AppliedBatchPlan;
using sdl3mcp::InputAction;
using sdl3mcp::InputBatch;
using sdl3mcp::KeyAction;
using sdl3mcp::KeyInput;
using sdl3mcp::KeyMap;
using sdl3mcp::KeyReplayer;
using sdl3mcp::MouseButton;
using sdl3mcp::MouseButtonAction;
using sdl3mcp::MouseButtonInput;
using sdl3mcp::MouseMetrics;
using sdl3mcp::MouseMoveInput;
using sdl3mcp::MouseReplayer;
using sdl3mcp::ReleaseAllInput;
using sdl3mcp::ScrollInput;
using sdl3mcp::TextInput;

constexpr SDL_WindowID kWindowID = 42;
constexpr Uint64 kTimestamp = 123456789;

/* 100x100 window points, no relative mode, warp on unless a test changes it. */
constexpr MouseMetrics kMetrics{100.0F, 100.0F, false, true};

struct Harness
{
    KeyMap keyMap;
    KeyReplayer keys{keyMap, kWindowID};
    MouseReplayer mouse{kWindowID, kMetrics};

    /*
      Builds and commits a batch the way AppIterate does. The returned plan's
      text pointers borrow `batch`, so callers keep it alive.
    */
    AppliedBatchPlan apply(const InputBatch &batch, bool textInputActive = true)
    {
        AppliedBatchPlan plan{keys, mouse};
        std::string error;
        CHECK(buildBatchPlan(batch, keys, mouse, kWindowID, textInputActive, kTimestamp, plan,
                             error));
        CHECK(error.empty());
        keys = plan.keyState;
        mouse = plan.mouseState;
        return plan;
    }

    /* Builds a plan, requires failure, and returns the error. */
    std::string reject(const InputBatch &batch, bool textInputActive = true)
    {
        AppliedBatchPlan plan{keys, mouse};
        std::string error;
        CHECK(!buildBatchPlan(batch, keys, mouse, kWindowID, textInputActive, kTimestamp, plan,
                              error));
        CHECK(!error.empty());
        CHECK(error.find('\n') == std::string::npos);
        CHECK(plan.eventsByAction.empty());
        CHECK(plan.warpsByAction.empty());
        return error;
    }
};

InputAction down(std::string key)
{
    return KeyInput{std::move(key), KeyAction::Down, {}};
}

InputAction up(std::string key)
{
    return KeyInput{std::move(key), KeyAction::Up, {}};
}

InputAction press(std::string key)
{
    return KeyInput{std::move(key), KeyAction::Press, {}};
}

InputAction text(std::string body)
{
    return TextInput{std::move(body)};
}

InputAction moveTo(float x, float y)
{
    return MouseMoveInput{MouseMoveInput::Mode::Absolute, x, y};
}

InputAction moveBy(float dx, float dy)
{
    return MouseMoveInput{MouseMoveInput::Mode::Relative, dx, dy};
}

InputAction button(MouseButton which, MouseButtonAction action, std::optional<float> x,
                   std::optional<float> y)
{
    return MouseButtonInput{which, action, x, y, 1};
}

InputAction scroll(float x, float y)
{
    return ScrollInput{x, y};
}

InputAction releaseAll()
{
    return ReleaseAllInput{};
}

bool heldEquals(const std::vector<SDL_Scancode> &held,
                std::initializer_list<SDL_Scancode> expected)
{
    return held.size() == expected.size()
           && std::equal(expected.begin(), expected.end(), held.begin());
}

void testKeyPress()
{
    Harness harness;
    InputBatch batch;
    batch.actions.push_back(press("a"));

    const AppliedBatchPlan plan = harness.apply(batch);

    CHECK(plan.eventsByAction.size() == 1);
    const std::vector<SDL_Event> &events = plan.eventsByAction[0];
    CHECK(events.size() == 2);
    CHECK(events[0].type == SDL_EVENT_KEY_DOWN);
    CHECK(events[0].key.scancode == SDL_SCANCODE_A);
    CHECK(events[0].key.windowID == kWindowID);
    CHECK(events[0].key.timestamp == kTimestamp);
    CHECK(events[1].type == SDL_EVENT_KEY_UP);
    CHECK(events[1].key.scancode == SDL_SCANCODE_A);

    CHECK(plan.warpsByAction.size() == 1);
    CHECK(!plan.warpsByAction[0].has_value());
    CHECK(harness.keys.heldKeys().empty());
}

void testHeldKeyCarriesAcrossBatches()
{
    Harness harness;

    InputBatch downBatch;
    downBatch.actions.push_back(down("a"));
    const AppliedBatchPlan first = harness.apply(downBatch);
    CHECK(first.eventsByAction[0].size() == 1);
    CHECK(first.eventsByAction[0][0].type == SDL_EVENT_KEY_DOWN);
    CHECK(heldEquals(harness.keys.heldKeys(), {SDL_SCANCODE_A}));

    /* The next batch starts from the committed state, so this down repeats. */
    InputBatch repeatBatch;
    repeatBatch.actions.push_back(down("a"));
    const AppliedBatchPlan second = harness.apply(repeatBatch);
    CHECK(second.eventsByAction[0].size() == 1);
    CHECK(second.eventsByAction[0][0].key.repeat);
    CHECK(heldEquals(harness.keys.heldKeys(), {SDL_SCANCODE_A}));

    InputBatch upBatch;
    upBatch.actions.push_back(up("a"));
    const AppliedBatchPlan third = harness.apply(upBatch);
    CHECK(third.eventsByAction[0].size() == 1);
    CHECK(third.eventsByAction[0][0].type == SDL_EVENT_KEY_UP);
    CHECK(harness.keys.heldKeys().empty());
}

void testTextEventBorrowsTheBatch()
{
    Harness harness;
    InputBatch batch;
    batch.actions.push_back(text("hi"));

    const AppliedBatchPlan plan = harness.apply(batch);

    CHECK(plan.eventsByAction.size() == 1);
    const std::vector<SDL_Event> &events = plan.eventsByAction[0];
    CHECK(events.size() == 1);
    CHECK(events[0].type == SDL_EVENT_TEXT_INPUT);
    CHECK(events[0].text.windowID == kWindowID);
    CHECK(events[0].text.timestamp == kTimestamp);
    CHECK(std::string(events[0].text.text) == "hi");

    const std::string &stored = std::get<TextInput>(batch.actions[0]).text;
    CHECK(events[0].text.text == stored.c_str());
}

void testInactiveTextIsRejected()
{
    Harness harness;
    InputBatch batch;
    batch.actions.push_back(text("hi"));

    const std::string error = harness.reject(batch, false);
    CHECK(error == "actions[0]: text input is not active on the bound window");
}

void testAbsoluteMoveWarps()
{
    Harness harness;
    InputBatch batch;
    batch.actions.push_back(moveTo(10.0F, 20.0F));

    const AppliedBatchPlan plan = harness.apply(batch);

    CHECK(plan.eventsByAction[0].size() == 1);
    const SDL_Event &motion = plan.eventsByAction[0][0];
    CHECK(motion.type == SDL_EVENT_MOUSE_MOTION);
    CHECK(motion.motion.windowID == kWindowID);
    CHECK(motion.motion.x == 10.0F);
    CHECK(motion.motion.y == 20.0F);
    /* The platform's warp event carries the delta, so the synthetic one does not. */
    CHECK(motion.motion.xrel == 0.0F);
    CHECK(motion.motion.yrel == 0.0F);

    CHECK(plan.warpsByAction[0].has_value());
    CHECK(plan.warpsByAction[0]->x == 10.0F);
    CHECK(plan.warpsByAction[0]->y == 20.0F);
    CHECK(harness.mouse.cursorX() == 10.0F);
    CHECK(harness.mouse.cursorY() == 20.0F);
}

void testAbsoluteMoveWithoutWarp()
{
    Harness harness;
    harness.mouse.setMetrics(MouseMetrics{100.0F, 100.0F, false, false});

    InputBatch batch;
    batch.actions.push_back(moveTo(10.0F, 20.0F));

    const AppliedBatchPlan plan = harness.apply(batch);

    const SDL_Event &motion = plan.eventsByAction[0][0];
    CHECK(motion.motion.x == 10.0F);
    CHECK(motion.motion.y == 20.0F);
    CHECK(motion.motion.xrel == 10.0F);
    CHECK(motion.motion.yrel == 20.0F);
    CHECK(!plan.warpsByAction[0].has_value());
}

void testAbsoluteMoveInRelativeMode()
{
    Harness harness;
    harness.mouse.setMetrics(MouseMetrics{100.0F, 100.0F, true, true});

    InputBatch batch;
    batch.actions.push_back(moveTo(10.0F, 20.0F));

    const AppliedBatchPlan plan = harness.apply(batch);

    const SDL_Event &motion = plan.eventsByAction[0][0];
    CHECK(motion.motion.x == 10.0F);
    CHECK(motion.motion.y == 20.0F);
    CHECK(motion.motion.xrel == 10.0F);
    CHECK(motion.motion.yrel == 20.0F);
    /* A warping replayer still must not warp while relative mode is on. */
    CHECK(!plan.warpsByAction[0].has_value());
}

void testRelativeMovePassesDelta()
{
    Harness harness;

    InputBatch batch;
    batch.actions.push_back(moveBy(5.0F, 7.0F));
    const AppliedBatchPlan plan = harness.apply(batch);

    const SDL_Event &motion = plan.eventsByAction[0][0];
    CHECK(motion.motion.x == 5.0F);
    CHECK(motion.motion.y == 7.0F);
    CHECK(motion.motion.xrel == 5.0F);
    CHECK(motion.motion.yrel == 7.0F);
    CHECK(!plan.warpsByAction[0].has_value());
}

void testWarpStaysWithItsAction()
{
    Harness harness;

    InputBatch batch;
    batch.actions.push_back(moveTo(10.0F, 20.0F));
    batch.actions.push_back(press("a"));
    batch.actions.push_back(scroll(0.0F, 120.0F));
    batch.actions.push_back(moveTo(50.0F, 60.0F));

    const AppliedBatchPlan plan = harness.apply(batch);

    CHECK(plan.warpsByAction.size() == 4);
    CHECK(plan.warpsByAction[0].has_value());
    CHECK(plan.warpsByAction[0]->x == 10.0F);
    CHECK(!plan.warpsByAction[1].has_value());
    CHECK(!plan.warpsByAction[2].has_value());
    CHECK(plan.warpsByAction[3].has_value());
    CHECK(plan.warpsByAction[3]->x == 50.0F);
    CHECK(plan.warpsByAction[3]->y == 60.0F);

    CHECK(plan.eventsByAction.size() == 4);
    CHECK(plan.eventsByAction[0].size() == 1);
    CHECK(plan.eventsByAction[1].size() == 2);
    CHECK(plan.eventsByAction[2].size() == 1);
    CHECK(plan.eventsByAction[3].size() == 1);
}

void testButtonAndScroll()
{
    Harness harness;

    InputBatch batch;
    batch.actions.push_back(button(MouseButton::Left, MouseButtonAction::Click, 10.0F, 20.0F));
    batch.actions.push_back(scroll(0.0F, 120.0F));

    const AppliedBatchPlan plan = harness.apply(batch);

    const std::vector<SDL_Event> &clicks = plan.eventsByAction[0];
    CHECK(clicks.size() == 3);

    /* A positioned click moves first: motion at the target, then the pair. */
    CHECK(clicks[0].type == SDL_EVENT_MOUSE_MOTION);
    CHECK(clicks[0].motion.x == 10.0F);
    CHECK(clicks[0].motion.y == 20.0F);
    CHECK(clicks[0].motion.state == 0);
    CHECK(clicks[1].type == SDL_EVENT_MOUSE_BUTTON_DOWN);
    CHECK(clicks[1].button.button == SDL_BUTTON_LEFT);
    CHECK(clicks[1].button.x == 10.0F);
    CHECK(clicks[1].button.y == 20.0F);
    CHECK(clicks[2].type == SDL_EVENT_MOUSE_BUTTON_UP);

    /* The warp stays with the click's own action. */
    CHECK(plan.warpsByAction.size() == 2);
    CHECK(plan.warpsByAction[0].has_value());
    CHECK(plan.warpsByAction[0]->x == 10.0F);
    CHECK(plan.warpsByAction[0]->y == 20.0F);
    CHECK(!plan.warpsByAction[1].has_value());

    const std::vector<SDL_Event> &wheels = plan.eventsByAction[1];
    CHECK(wheels.size() == 1);
    CHECK(wheels[0].type == SDL_EVENT_MOUSE_WHEEL);
    CHECK(wheels[0].wheel.y == 120.0F);
    CHECK(wheels[0].wheel.mouse_x == 10.0F);
    CHECK(wheels[0].wheel.mouse_y == 20.0F);

    /* A click ends released, so nothing stays held. */
    CHECK(harness.mouse.heldButtons().empty());
}

void testReleaseAllClearsBothStates()
{
    Harness harness;

    InputBatch held;
    held.actions.push_back(down("a"));
    held.actions.push_back(button(MouseButton::Right, MouseButtonAction::Down, std::nullopt,
                                  std::nullopt));
    harness.apply(held);
    CHECK(heldEquals(harness.keys.heldKeys(), {SDL_SCANCODE_A}));
    CHECK(harness.mouse.heldButtons().size() == 1);

    InputBatch release;
    release.actions.push_back(releaseAll());
    const AppliedBatchPlan plan = harness.apply(release);

    CHECK(plan.eventsByAction[0].size() == 2);
    CHECK(plan.eventsByAction[0][0].type == SDL_EVENT_KEY_UP);
    CHECK(plan.eventsByAction[0][0].key.scancode == SDL_SCANCODE_A);
    CHECK(plan.eventsByAction[0][1].type == SDL_EVENT_MOUSE_BUTTON_UP);
    CHECK(plan.eventsByAction[0][1].button.button == SDL_BUTTON_RIGHT);

    CHECK(harness.keys.heldKeys().empty());
    CHECK(harness.keys.modifiers() == SDL_KMOD_NONE);
    CHECK(harness.mouse.heldButtons().empty());
}

void testActionsKeepOrder()
{
    Harness harness;

    InputBatch batch;
    batch.actions.push_back(down("a"));
    batch.actions.push_back(scroll(0.0F, 120.0F));
    batch.actions.push_back(up("a"));

    const AppliedBatchPlan plan = harness.apply(batch);

    CHECK(plan.eventsByAction.size() == batch.actions.size());
    CHECK(plan.eventsByAction[0][0].type == SDL_EVENT_KEY_DOWN);
    CHECK(plan.eventsByAction[1][0].type == SDL_EVENT_MOUSE_WHEEL);
    CHECK(plan.eventsByAction[2][0].type == SDL_EVENT_KEY_UP);
}

void testFailureLeavesEverythingUntouched()
{
    Harness harness;

    InputBatch seed;
    seed.actions.push_back(down("a"));
    harness.apply(seed);

    InputBatch batch;
    batch.actions.push_back(moveTo(50.0F, 60.0F));
    batch.actions.push_back(down("b"));
    batch.actions.push_back(up("c"));

    /* The plan starts from the live state and must still hold exactly that. */
    AppliedBatchPlan plan{harness.keys, harness.mouse};
    std::string error;
    CHECK(!buildBatchPlan(batch, harness.keys, harness.mouse, kWindowID, true, kTimestamp, plan,
                          error));
    CHECK(error == "actions[2]: key is not held");

    CHECK(plan.eventsByAction.empty());
    CHECK(plan.warpsByAction.empty());
    CHECK(heldEquals(plan.keyState.heldKeys(), {SDL_SCANCODE_A}));
    CHECK(plan.keyState.modifiers() == SDL_KMOD_NONE);
    CHECK(plan.mouseState.cursorX() == 0.0F);
    CHECK(plan.mouseState.cursorY() == 0.0F);
    CHECK(plan.mouseState.heldButtons().empty());

    /* The live replayers were never asked to change either. */
    CHECK(heldEquals(harness.keys.heldKeys(), {SDL_SCANCODE_A}));
    CHECK(harness.mouse.cursorX() == 0.0F);
    CHECK(harness.mouse.cursorY() == 0.0F);
    CHECK(harness.mouse.heldButtons().empty());
}

} // namespace

int main()
{
    testKeyPress();
    testHeldKeyCarriesAcrossBatches();
    testTextEventBorrowsTheBatch();
    testInactiveTextIsRejected();
    testAbsoluteMoveWarps();
    testAbsoluteMoveWithoutWarp();
    testAbsoluteMoveInRelativeMode();
    testRelativeMovePassesDelta();
    testWarpStaysWithItsAction();
    testButtonAndScroll();
    testReleaseAllClearsBothStates();
    testActionsKeepOrder();
    testFailureLeavesEverythingUntouched();

    std::printf("app_input_tests passed\n");
    return 0;
}
