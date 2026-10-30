#!/usr/bin/env python3
"""Check that README.md still describes the implemented SDL3MCP contract.

The README is the user-facing contract, so drift in a schema, a run command, or
the loop order is a bug. The checks are deliberately mechanical:

- both complete C examples are compiled with ``gcc -std=c11 -fsyntax-only``;
- every fenced ``json`` block parses;
- the ``sdl_input`` schema matches the implemented limits;
- the ``sdl_capture`` schemas match both sides: region + timeout_ms for the
  library's raw framing and max_width for the supervisor's image block;
- the region prose names the captured frame's dimensions, not the window's;
- the public ``SDL3MCP_WriteFrame`` signature takes explicit width and height;
- the ``sdl_terminate`` schema is the empty object with all four hints;
- the quick start matches ``examples/minimal/main.c`` and contains R/G/B;
- the documented run command points at ``<build>/examples/minimal``;
- the supervisor section documents ``sdl3mcp-supervisor --target`` and its
  Idle/Starting/Running states;
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
SUPERVISOR_STATES = ("Idle", "Starting", "Running")

STALE_PHRASES = (
    "F+2",
    "120 per notch",
    "32 bytes",
    "requires window focus",
    "actions queued",
    "at SDL_GetWindowSizeInPixels",
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


def find_tool_document(documents: list[object], name: str) -> dict | None:
    for document in documents:
        if (isinstance(document, dict)
                and document.get("name") == name
                and "inputSchema" in document):
            return document
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


def check_capture_schemas(documents: list[object], failures: list[str]) -> None:
    """Both captures are contract: raw framing in the library, images via the supervisor."""
    library = None
    supervisor = None
    for document in documents:
        if not (isinstance(document, dict)
                and document.get("name") == "sdl_capture"
                and isinstance(document.get("inputSchema"), dict)):
            continue
        properties = document["inputSchema"].get("properties", {})
        if "max_width" in properties:
            supervisor = document
        elif "timeout_ms" in properties:
            library = document

    if library is None:
        failures.append("no library sdl_capture schema with region and timeout_ms")
    else:
        schema = library["inputSchema"]
        properties = schema.get("properties", {})
        if set(properties) != {"region", "timeout_ms"}:
            failures.append(
                "library sdl_capture must advertise only region and timeout_ms")
        if schema.get("additionalProperties") is not False:
            failures.append("library sdl_capture must set additionalProperties false")
        if schema.get("required"):
            failures.append("library sdl_capture has no required members")
        timeout = properties.get("timeout_ms", {})
        if (timeout.get("minimum"), timeout.get("maximum")) != (1, 60000):
            failures.append("library sdl_capture timeout_ms must be 1..60000")
        check_region_schema(schema, "library sdl_capture", failures)
        description = library.get("description", "")
        if "rgba8888" not in description or "byteLength" not in description:
            failures.append(
                "library sdl_capture description must document the raw "
                "format/byteLength framing")

    if supervisor is None:
        failures.append("no supervisor sdl_capture schema with max_width")
    else:
        schema = supervisor["inputSchema"]
        properties = schema.get("properties", {})
        max_width = properties.get("max_width", {})
        if (max_width.get("minimum"), max_width.get("maximum")) != (16, 16384):
            failures.append("supervisor sdl_capture max_width must be 16..16384")
        timeout = properties.get("timeout_ms", {})
        if (timeout.get("minimum"), timeout.get("maximum")) != (1, 60000):
            failures.append("supervisor sdl_capture timeout_ms must be 1..60000")
        check_region_schema(schema, "supervisor sdl_capture", failures)
        if "PNG" not in supervisor.get("description", ""):
            failures.append("supervisor sdl_capture description must promise a PNG")


def check_region_schema(schema: dict, label: str, failures: list[str]) -> None:
    region = schema.get("properties", {}).get("region")
    if not isinstance(region, dict):
        failures.append(f"{label} schema has no region property")
        return
    if set(region.get("required", [])) != {"x", "y", "width", "height"}:
        failures.append(f"{label} region must require x, y, width, and height")
    if region.get("additionalProperties") is not False:
        failures.append(f"{label} region must set additionalProperties false")
    region_properties = region.get("properties", {})
    for field in ("x", "y", "width", "height"):
        if region_properties.get(field, {}).get("type") != "integer":
            failures.append(f"{label} region {field} must be an integer")


def check_capture_framing_prose(readme: str, failures: list[str]) -> None:
    """The raw framing and its supervisor split are prose the contract depends on."""
    for required in ('"format":"rgba8888"', '"byteLength"', "raw RGBA"):
        if required not in readme:
            failures.append(f"README does not document the capture framing phrase {required!r}")
    if "max_width" not in readme:
        failures.append("README does not document the supervisor's max_width")
    lowered = readme.lower()
    if "library" not in lowered or "supervisor" not in lowered:
        failures.append("README must state which side owns the capture presentation")
    # The region is in pixels of the frame the app wrote, not of the window, so
    # the documented bounds must be the capture statistics.
    for required in ("capture.lastWidth", "capture.lastHeight"):
        if required not in readme:
            failures.append(f"README does not document the region against {required!r}")


def check_write_frame_signature(readme: str, failures: list[str]) -> None:
    """Explicit dimensions are the contract: the app, not the window, decides."""
    signature = "SDL3MCP_WriteFrame(const uint8_t *data, size_t size, int width, int height)"
    if signature not in readme:
        failures.append(f"README must document '{signature}'")
    if "SDL3MCP_WriteFrame(const uint8_t *data, size_t size);" in readme:
        failures.append("README still documents the two-argument SDL3MCP_WriteFrame")


def check_terminate_schema(documents: list[object], failures: list[str]) -> None:
    tool = find_tool_document(documents, "sdl_terminate")
    if tool is None:
        failures.append("no sdl_terminate tool schema with an inputSchema in the README")
        return

    schema = tool["inputSchema"]
    if not isinstance(schema, dict):
        failures.append("sdl_terminate inputSchema must be an object")
    else:
        if schema.get("type") != "object":
            failures.append("sdl_terminate schema type must be object")
        if schema.get("properties") != {}:
            failures.append("sdl_terminate schema properties must be empty")
        if schema.get("additionalProperties") is not False:
            failures.append("sdl_terminate schema must set additionalProperties false")

    annotations = tool.get("annotations")
    if not isinstance(annotations, dict):
        failures.append("sdl_terminate schema has no annotations")
        return
    expected = {
        "readOnlyHint": False,
        "destructiveHint": True,
        "idempotentHint": False,
        "openWorldHint": False,
    }
    for key, value in expected.items():
        if annotations.get(key) != value:
            failures.append(f"sdl_terminate annotation {key} must be {value}")


def check_example_command(readme: str, failures: list[str]) -> None:
    if EXAMPLE_COMMAND not in readme:
        failures.append(f"README must document the example command '{EXAMPLE_COMMAND}'")
    if WRONG_EXAMPLE_PATH in readme:
        failures.append(f"README must not reference '{WRONG_EXAMPLE_PATH}'")


def check_supervisor_docs(readme: str, failures: list[str]) -> None:
    """The supervisor is part of the shipped binary, so its contract is checked."""
    if "## Supervisor" not in readme:
        failures.append("README has no Supervisor section")
    if "sdl3mcp-supervisor" not in readme:
        failures.append("README does not document the sdl3mcp-supervisor command")
    if "--target" not in readme:
        failures.append("README does not document the supervisor's --target option")
    for state in SUPERVISOR_STATES:
        if f"`{state}`" not in readme:
            failures.append(f"README does not document the supervisor {state} state")


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
    check_capture_schemas(documents, failures)
    check_capture_framing_prose(readme, failures)
    check_write_frame_signature(readme, failures)
    check_terminate_schema(documents, failures)
    check_example_command(readme, failures)
    check_supervisor_docs(readme, failures)
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
