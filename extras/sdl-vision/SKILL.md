---
name: SDLVision
description: Capture the running SDL3MCP app and answer questions about its frame through a vision model; use instead of the Vision skill when the picture comes from sdl_capture
---

# Seeing the SDL3MCP frame

Use this when a question is about what the running application looks like. The `sdl_capture` result is a content array: a text summary plus an `image/png` block. A text-only model's code only ever sees the summary, so the frame itself has to be read by a multimodal model. Delegate it. This requires the `sdl3mcp` server to be the supervisor, the only endpoint that returns a PNG; see the extras README.

## Workflow

1. Check the tool catalogue. If it is empty, a `spawn` is already in progress — wait for the `notifications/tools/list_changed` notification instead of calling anything. If it exposes only `spawn`, the supervisor is idle — call `spawn`, which is synchronous and bounded by `--startup-timeout-ms` (10 s by default) and reports the child's recent stderr on failure. Then call `sdl_info` and confirm a bound window.
2. Launch the `sdl-vision` subagent once with the exact questions and any context that changes the answer: the widget you care about, a region in captured-frame pixels from `sdl_info`'s `capture.lastWidth`/`capture.lastHeight`, an expected colour, text to quote verbatim. It captures the frame itself.
3. Report its answer as the source of truth for layout and visible text, but treat any colour it gives as advisory — verify numerically when the exact value matters. Never claim you looked at the frame yourself.

## Notes

- The subagent receives the PNG natively, so no file path or base64 handling is involved. Do not route a capture through client storage such as `~/.local/share/opencode`; that is private state, not an interface.
- The subagent runs its own configured model, and layout and visible text are its strength; colour and exact visual state are not — a live run against examples/minimal reported a red background as black and a green background as teal, so treat any colour, and any 'the frame is blank' claim, as advisory and verify it from a decoded capture instead.
- One capture per question unless the caller asks for regions or crops; extra captures cost time and tokens for no better answer.
- For exact colours, pixel values, and dimensions, decode the PNG numerically instead of using vision; vision is for layout and visible text.
- If the subagent reports a capture error, relay the error text verbatim. Do not guess at the picture.
- If it claims it cannot see the frame while `sdl_info` shows `capture.count` incremented, relaunch it once: the PNG is attached to the tool result and the claim is a misread of the summary string.
