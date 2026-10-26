#!/usr/bin/env python3
"""Check that README.md still describes the implemented SDL3MCP contract.

The README is the user-facing contract, so drift in a schema, a run command, or
the loop order is a bug. The checks are deliberately mechanical:

- both complete C examples are compiled with ``gcc -std=c11 -fsyntax-only``;
- every fenced ``json`` block parses;
- the ``sdl_input`` and ``sdl_capture`` schemas match the implemented limits;
- the quick start matches ``examples/minimal/main.c`` and contains R/G/B;
- the documented run command points at ``<build>/examples/minimal``;
- stale phrases from earlier drafts are gone;
- ``SDL3MCP_AppIterate`` is the last call, after ``SDL_RenderPresent``.
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

EXAMPLE_COMMAND = "./build/examples/minimal --mcp"
WRONG_EXAMPLE_PATH = "examples/minimal/minimal"

STALE_PHRASES = (
    "F+2",
    "120 per notch",
    "32 bytes",
    "requires window focus",
    "actions queued",
)


def fenced_blocks(readme: str) -> list[tuple[str, str]]:
    """Return ``(language, body)`` for every fenced block in the README."""
    blocks: list[tuple[str, str]] = []
    language = ""
    body: list[str] = []
    inside = False

    for line in readme.splitlines():
        if line.lstrip().startswith("```"):
            if inside:
                blocks.append((language, "\n".join(body)))
                inside = False
                language = ""
                body = []
            else:
                inside = True
                language = line.strip()[3:].strip()
            continue
        if inside:
            body.append(line)

    return blocks


def is_complete_example(block: str) -> bool:
    return "SDL_AppInit(void **appstate" in block or "int main(" in block


def contains_key(value: object, needle: str) -> bool:
    """True when a JSON value has a mapping key named ``needle`` anywhere."""
    if isinstance(value, dict):
        return any(key == needle or contains_key(item, needle)
                   for key, item in value.items())
    if isinstance(value, list):
        return any(contains_key(item, needle) for item in value)
    return False


def find_tool_schema(documents: list[object], name: str) -> object | None:
    for document in documents:
        if (isinstance(document, dict)
                and document.get("name") == name
                and "inputSchema" in document):
            return document["inputSchema"]
    return None


def find_action_variant(schema: dict, action_type: str) -> dict | None:
    variants = schema["properties"]["actions"]["items"]["oneOf"]
    for variant in variants:
        properties = variant.get("properties", {})
        if properties.get("type", {}).get("const") == action_type:
            return variant
    return None


def sdl_cflags(failures: list[str]) -> list[str]:
    pkg_config = shutil.which("pkg-config")
    if pkg_config is None:
        return []
    result = subprocess.run([pkg_config, "--cflags", "sdl3"],
                            capture_output=True, text=True)
    if result.returncode != 0:
        failures.append("pkg-config --cflags sdl3 failed; install SDL3's pkg-config file")
        return []
    return result.stdout.split()


def compile_examples(blocks: list[str], source: Path, build: Path,
                     failures: list[str]) -> None:
    compiler = shutil.which("gcc")
    if compiler is None:
        failures.append("gcc is required to compile the README examples")
        return

    include_dirs = [source / "include", build / "generated"]
    for include_dir in include_dirs:
        if not include_dir.is_dir():
            failures.append(f"missing include directory {include_dir}; configure the build first")
            return

    cflags = sdl_cflags(failures)

    for index, block in enumerate(blocks, start=1):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "readme_example.c"
            path.write_text(block + "\n", encoding="utf-8")
            command = [
                compiler,
                "-std=c11",
                "-fsyntax-only",
                *cflags,
                *(f"-I{include_dir}" for include_dir in include_dirs),
                str(path),
            ]
            result = subprocess.run(command, capture_output=True, text=True)
            if result.returncode != 0:
                failures.append(
                    f"complete C example {index} does not compile:\n"
                    f"{result.stderr.strip()}")


def check_json_blocks(blocks: list[tuple[str, str]],
                      failures: list[str]) -> list[object]:
    documents: list[object] = []
    for index, (language, body) in enumerate(blocks, start=1):
        if language != "json":
            continue
        try:
            documents.append(json.loads(body))
        except json.JSONDecodeError as error:
            failures.append(f"JSON block {index} does not parse: {error}")
    return documents


def check_input_schema(documents: list[object], failures: list[str]) -> None:
    schema = find_tool_schema(documents, "sdl_input")
    if not isinstance(schema, dict):
        failures.append("no sdl_input tool schema with an inputSchema in the README")
        return

    if contains_key(schema, "repeat"):
        failures.append("sdl_input schema still has a repeat property")

    actions = schema.get("properties", {}).get("actions")
    if not isinstance(actions, dict):
        failures.append("sdl_input schema has no actions property")
        return
    if actions.get("minItems") != 1:
        failures.append("sdl_input actions minItems must be 1")
    if actions.get("maxItems") != 256:
        failures.append("sdl_input actions maxItems must be 256")

    text = find_action_variant(schema, "text")
    if text is None:
        failures.append("sdl_input schema has no text action")
    else:
        text_schema = text["properties"].get("text", {})
        if text_schema.get("maxLength") != 1024:
            failures.append("sdl_input text maxLength must be 1024")
        if text_schema.get("minLength") != 1:
            failures.append("sdl_input text minLength must be 1")


def check_capture_schema(documents: list[object], failures: list[str]) -> None:
    schema = find_tool_schema(documents, "sdl_capture")
    if not isinstance(schema, dict):
        failures.append("no sdl_capture tool schema with an inputSchema in the README")
        return

    properties = schema.get("properties", {})
    max_width = properties.get("max_width", {})
    if (max_width.get("minimum"), max_width.get("maximum")) != (16, 16384):
        failures.append("sdl_capture max_width must be an integer in 16..16384")

    timeout = properties.get("timeout_ms", {})
    if (timeout.get("minimum"), timeout.get("maximum")) != (1, 60000):
        failures.append("sdl_capture timeout_ms must be an integer in 1..60000")

    region = properties.get("region")
    if not isinstance(region, dict):
        failures.append("sdl_capture schema has no region property")
        return
    if set(region.get("required", [])) != {"x", "y", "width", "height"}:
        failures.append("sdl_capture region must require x, y, width, and height")
    region_properties = region.get("properties", {})
    for field in ("x", "y", "width", "height"):
        if region_properties.get(field, {}).get("type") != "integer":
            failures.append(f"sdl_capture region {field} must be an integer")


def check_example_command(readme: str, failures: list[str]) -> None:
    if EXAMPLE_COMMAND not in readme:
        failures.append(f"README must document the example command '{EXAMPLE_COMMAND}'")
    if WRONG_EXAMPLE_PATH in readme:
        failures.append(f"README must not reference '{WRONG_EXAMPLE_PATH}'")


def check_stale_phrases(readme: str, failures: list[str]) -> None:
    haystack = readme.lower()
    for phrase in STALE_PHRASES:
        if phrase.lower() in haystack:
            failures.append(f"README still contains the stale phrase '{phrase}'")


def check_quick_start(quick_start: str, source: Path, failures: list[str]) -> None:
    # SDL spells the keycodes SDLK_R/G/B; compare case-insensitively so the check
    # still fails when a toggle disappears.
    haystack = quick_start.lower()
    for key in ("SDLK_r", "SDLK_g", "SDLK_b"):
        if key.lower() not in haystack:
            failures.append(f"quick start does not handle {key}")

    example = source / "examples" / "minimal" / "main.c"
    if not example.is_file():
        failures.append(f"missing example source {example}")
        return
    if quick_start.strip() != example.read_text(encoding="utf-8").strip():
        failures.append("quick start does not match examples/minimal/main.c")


def check_loop_order(block: str, index: int, failures: list[str]) -> None:
    presented = block.rfind("SDL_RenderPresent")
    iterated = block.rfind("SDL3MCP_AppIterate")
    if presented < 0:
        failures.append(f"complete C example {index} does not call SDL_RenderPresent")
    if iterated < 0:
        failures.append(f"complete C example {index} does not call SDL3MCP_AppIterate")
    if presented >= 0 and iterated >= 0 and iterated < presented:
        failures.append(
            f"complete C example {index} must call SDL3MCP_AppIterate after SDL_RenderPresent")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Verify README.md against the implemented SDL3MCP contract.")
    parser.add_argument("--source", required=True, type=Path,
                        help="repository source directory")
    parser.add_argument("--build", required=True, type=Path,
                        help="CMake build directory with generated headers")
    arguments = parser.parse_args(argv)

    readme_path = arguments.source / "README.md"
    if not readme_path.is_file():
        print(f"README check failed: {readme_path} does not exist", file=sys.stderr)
        return 1

    readme = readme_path.read_text(encoding="utf-8")
    blocks = fenced_blocks(readme)
    failures: list[str] = []

    complete_examples = [body for _, body in blocks if is_complete_example(body)]
    if not any("SDL_AppInit(void **appstate" in body for body in complete_examples):
        failures.append("README has no complete main-callback example")
    if not any("int main(" in body for body in complete_examples):
        failures.append("README has no complete manual-loop example")

    documents = check_json_blocks(blocks, failures)
    check_input_schema(documents, failures)
    check_capture_schema(documents, failures)
    check_example_command(readme, failures)
    check_stale_phrases(readme, failures)

    for index, body in enumerate(complete_examples, start=1):
        check_loop_order(body, index, failures)

    quick_start = next((body for body in complete_examples
                        if "SDL_AppInit(void **appstate" in body), None)
    if quick_start is not None:
        check_quick_start(quick_start, arguments.source, failures)

    compile_examples(complete_examples, arguments.source, arguments.build, failures)

    if failures:
        print("README check failed:", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1

    print("README matches the implemented contract.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
