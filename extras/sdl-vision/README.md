# SDLVision extras

Optional OpenCode integration that lets a vision model look at the running
SDL3MCP application: the `sdl-vision` subagent calls `sdl_capture` and answers
questions about the returned frame, and the `sdl-vision` skill tells a text-only
parent agent when and how to delegate.

Nothing here is part of the library build; it is editor/agent configuration.

## Install

```sh
mkdir -p ~/.config/opencode/agents ~/.config/opencode/skills/sdl-vision
cp extras/sdl-vision/sdl-vision.md ~/.config/opencode/agents/sdl-vision.md
cp extras/sdl-vision/SKILL.md ~/.config/opencode/skills/sdl-vision/SKILL.md
```

The agent needs an MCP server named `sdl3mcp` and a vision-capable model. It
ships configured for `opencode-go/minimax-m3`.

The server must be `build/sdl3mcp-supervisor --target <app> --mcp`, as in the
repository's `opencode.json`. Pointing the client at the app's own `--mcp`
server is not supported: there `sdl_capture` returns the library's raw RGBA
framing instead of an `image/png` block, and rejects `max_width`.

## Limits

The model reads layout, text, and visual state reliably; its colour estimates
are rough, so exact colours and pixel values should be decoded numerically from
the PNG instead.
