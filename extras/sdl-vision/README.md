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

```json
{
  "mcp": {
    "servers": {
      "sdl3mcp": {
        "type": "local",
        "command": [
          "/path/to/SDL3MCP/build/sdl3mcp-supervisor",
          "--target",
          "/path/to/SDL3MCP/build/examples/minimal",
          "--mcp"
        ]
      }
    }
  }
}
```

Use absolute paths, or keep the repository's relative paths and start the client
from the repository root, because the supervisor resolves them against the
workspace directory. The `--mcp` after `--target` is the app's own flag, not a
supervisor option: `--target` consumes everything after it. Build both with
`-DSDL3MCP_BUILD_SUPERVISOR=ON`, as the main README describes.

The agent needs an MCP server named `sdl3mcp` and a vision-capable model. It
ships configured for `opencode-go/minimax-m3`.

The server must be `build/sdl3mcp-supervisor --target <app> --mcp`, as in the
repository's `opencode.json`. Pointing the client at the app's own `--mcp`
server is not supported: there `sdl_capture` returns the library's raw RGBA
framing instead of an `image/png` block, and rejects `max_width`.

## Limits

The model is strongest on layout and visible text. Its colour and visual-state reports are unreliable: in a test against examples/minimal it called a red background black and a green background teal. Decode the PNG numerically for any colour, pixel value, or dimension, and swap the model in the agent frontmatter for a stronger vision model if state accuracy matters.
