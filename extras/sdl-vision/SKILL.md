---
name: SDLVision
description: Capture the running SDL3MCP app and answer questions about its frame through a vision model; use instead of the Vision skill when the picture comes from sdl_capture
---

# Seeing the SDL3MCP frame

Use this when a question is about what the running application looks like. The `sdl_capture` result is an image content block: it reaches the client, not your tools, so a text-only model cannot read it. Delegate it. This requires the `sdl3mcp` server to be the supervisor, the only endpoint that returns a PNG; see the extras README.

## Workflow

1. Check the tool catalogue. If it exposes only `spawn`, the supervisor is idle — call `spawn`, which is synchronous and bounded by `--startup-timeout-ms` (10 s by default) and reports the child's recent stderr on failure. Then call `sdl_info` and confirm a bound window.
2. Launch the `sdl-vision` subagent once with the exact questions and any context that changes the answer: the widget you care about, a region in full-frame pixels from `sdl_info`'s `pixelWidth`/`pixelHeight`, an expected colour, text to quote verbatim. It captures the frame itself.
3. Report its answer as the source of truth for layout, text, and visual state, but treat any colour it gives as advisory — verify numerically when the exact value matters. Never claim you looked at the frame yourself.

## Notes

- The subagent receives the PNG natively, so no file path or base64 handling is involved. Do not route a capture through client storage such as `~/.local/share/opencode`; that is private state, not an interface.
- The subagent runs its own configured model. It reads layout, text, and visual state reliably, but its colour estimates are rough — never quote its hex as a measured value.
- One capture per question unless the caller asks for regions or crops; extra captures cost time and tokens for no better answer.
- For exact colours, pixel values, and dimensions, decode the PNG numerically instead of using vision; vision is for layout, text, and visual state.
- If the subagent reports a capture error, relay the error text verbatim. Do not guess at the picture.
- If it claims it cannot see the frame while `sdl_info` shows `capture.count` incremented, relaunch it once: the PNG is attached to the tool result and the claim is a misread of the summary string.
