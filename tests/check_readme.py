#!/usr/bin/env python3
"""Check that README.md still describes the implemented SDL3MCP 1.0 contract.

The README is the user-facing contract, so drift in a schema, a run command, or
the loop order is a bug. The checks are deliberately mechanical:

- both complete C examples compile with ``gcc -std=c11 -fsyntax-only`` and call
  ``SDL3MCP_AppIterate`` after ``SDL_RenderPresent``;
- every fenced ``json`` block parses;
- the README documents exactly the four application tools, and the library's
  ``sdl_input`` and ``sdl_capture`` carry no ``timeout_ms`` while the Handler's
  copies carry one (1..60000);
- ``sdl_info`` takes no arguments and its ``limits`` carry only
  ``maxFrameBytes``: 1.0 has no library frame timeout;
- every tool description in the README and in the Handler catalog (read with
  ``--dump-tools``) states an ``Example:`` JSON object and names each argument
  its schema accepts;
- the capture framing prose names the raw header and the Handler's PNG policy,
  and says captures are never resized;
- the ``sdl_input`` text names the applied frame, the attached frame, and the
  fact that the attached frame was rendered after the actions were delivered;
- the public ``SDL3MCP_WriteFrame`` signature takes explicit width and height;
- the ``sdl_terminate`` schema is the empty object with all four hints;
- the README names no bundled example path or example app; the example
  application is Catalyst, outside this tree;
- the Handler section documents Server/Relay modes, ``--targets``, ``--web``,
  ``--dry-run`` and ``--dump-tools``;
- the release is 1.0.0;
- stale phrases from v0 are gone.
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

# No bundled example remains: Catalyst is the example application, so a README
# that names an example path or the old `minimal` app has drifted.
EXAMPLE_REFERENCES = (
    re.compile(r"\bexamples/", re.IGNORECASE),
    re.compile(r"\bminimal\b", re.IGNORECASE),
)
EXAMPLE_MARKER = "Example:"
LIBRARY_TOOLS = ("sdl_info", "sdl_input", "sdl_capture", "sdl_terminate")
# clang-format decides the pointer spacing, so match the callback rather than
# one spelling of it.
MAIN_CALLBACK = re.compile(r"SDL_AppInit\s*\(\s*void\s*\*\*\s*appstate")

STALE_PHRASES = (
    "F+2",
    "120 per notch",
    "32 bytes",
    "requires window focus",
    "actions queued",
    "at SDL_GetWindowSizeInPixels",
    "--print-plan",
    "--startup-timeout-ms",
    "--terminate-timeout-ms",
    "--mcp-timeout-ms",
    "--web-root",
    "frame_timeout_ms",
    "frameTimeoutMs",
    "SDL3MCP_TRANSPORT",
    "SDL3MCP_Transport",
    "--mcp=stdio",
    "the supervisor",
)

# --target is gone but --targets stays, so match the option and not its prefix.
STALE_OPTIONS = (re.compile(r"--target(?![s=\w])"),)


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
    return bool(MAIN_CALLBACK.search(block)) or "int main(" in block


def contains_key(value: object, needle: str) -> bool:
    """True when a JSON value has a mapping key named ``needle`` anywhere."""
    if isinstance(value, dict):
        return any(key == needle or contains_key(item, needle)
                   for key, item in value.items())
    if isinstance(value, list):
        return any(contains_key(item, needle) for item in value)
    return False


def tool_documents(documents: list[object]) -> list[dict]:
    return [document for document in documents
            if isinstance(document, dict)
            and isinstance(document.get("name"), str)
            and isinstance(document.get("inputSchema"), dict)]


def find_tool_documents(documents: list[object], name: str) -> list[dict]:
    return [document for document in tool_documents(documents)
            if document["name"] == name]


def find_action_variant(schema: dict, action_type: str) -> dict | None:
    variants = schema["properties"]["actions"]["items"]["oneOf"]
    for variant in variants:
        properties = variant.get("properties", {})
        if properties.get("type", {}).get("const") == action_type:
            return variant
    return None


def example_arguments(description: str) -> object | None:
    """Return the JSON value after the description's ``Example:`` marker."""
    marker = description.rfind(EXAMPLE_MARKER)
    if marker < 0:
        return None
    text = description[marker + len(EXAMPLE_MARKER):].lstrip()
    if not text:
        return None
    try:
        value, _ = json.JSONDecoder().raw_decode(text)
    except json.JSONDecodeError:
        return None
    return value


def check_tool_description(tool: dict, label: str, failures: list[str]) -> None:
    """Every tool entry documents its purpose, arguments, and one example."""
    name = tool.get("name")
    if not isinstance(name, str) or not name:
        failures.append(f"{label} has a tool definition without a name")
        return

    description = tool.get("description")
    if not isinstance(description, str) or not description.strip():
        failures.append(f"{label} tool {name} has no description")
        return

    if not isinstance(example_arguments(description), dict):
        failures.append(
            f"{label} tool {name} description has no parseable "
            "'Example: {...}' JSON object")

    schema = tool.get("inputSchema")
    if not isinstance(schema, dict):
        return
    arguments = set(schema.get("properties", {}))
    required = schema.get("required", [])
    if isinstance(required, list):
        arguments.update(argument for argument in required if isinstance(argument, str))
    for argument in sorted(arguments):
        if argument not in description:
            failures.append(
                f"{label} tool {name} description does not name its argument "
                f"'{argument}'")


def check_tool_descriptions(documents: list[object], label: str,
                            failures: list[str]) -> None:
    for document in tool_documents(documents):
        check_tool_description(document, label, failures)


def find_tool_helper(build: Path) -> Path | None:
    """The Handler's --dump-tools is the catalog tools/list uses.

    Only this build tree is searched: a sibling tree may be a different
    configuration, and its catalog would not describe this build.
    """
    candidate = build / "sdl3mcp"
    return candidate if candidate.is_file() else None


def load_tool_catalog(helper: Path | None, targets: Path | None,
                      failures: list[str]) -> list[object]:
    if helper is None:
        print("Handler catalog dump not built; checked README tool documents only")
        return []
    command = [str(helper), "--dump-tools"]
    if targets is not None:
        command.append(f"--targets={targets}")
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        failures.append(f"{helper} --dump-tools failed:\n{result.stderr.strip()}")
        return []
    try:
        catalog = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        failures.append(f"{helper} --dump-tools did not print JSON: {error}")
        return []
    if not isinstance(catalog, list) or not catalog:
        failures.append(f"{helper} --dump-tools returned no tool definitions")
        return []
    return catalog


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


def check_tool_set(documents: list[object], failures: list[str]) -> None:
    """Exactly the four application tools, each with a library and a Handler copy."""
    documented = {document["name"] for document in tool_documents(documents)}
    for name in LIBRARY_TOOLS:
        if name not in documented:
            failures.append(f"README documents no {name} tool schema")
    for name in sorted(documented - set(LIBRARY_TOOLS)):
        # The Handler adds wait_session and one app_spawn_<alias> per source.
        if name == "wait_session" or name.startswith("app_spawn_"):
            continue
        failures.append(f"README documents a tool 1.0 does not have: {name}")

    for name in ("sdl_input", "sdl_capture"):
        copies = find_tool_documents(documents, name)
        library = [copy for copy in copies
                   if "timeout_ms" not in copy["inputSchema"].get("properties", {})]
        handler = [copy for copy in copies
                   if "timeout_ms" in copy["inputSchema"].get("properties", {})]
        if not library:
            failures.append(
                f"no library {name} schema without timeout_ms; the library has no "
                "frame timeout")
        if not handler:
            failures.append(
                f"no Handler {name} schema with timeout_ms; the Handler owns the "
                "forward budget")
        for copy in handler:
            timeout = copy["inputSchema"]["properties"]["timeout_ms"]
            if (timeout.get("minimum"), timeout.get("maximum")) != (1, 60000):
                failures.append(f"Handler {name} timeout_ms must be 1..60000")


def check_input_schema(documents: list[object], failures: list[str]) -> None:
    schemas = [copy["inputSchema"] for copy in find_tool_documents(documents, "sdl_input")]
    if not schemas:
        failures.append("no sdl_input tool schema with an inputSchema in the README")
        return

    for schema in schemas:
        if contains_key(schema, "repeat"):
            failures.append("sdl_input schema still has a repeat property")
        actions = schema.get("properties", {}).get("actions")
        if not isinstance(actions, dict):
            failures.append("sdl_input schema has no actions property")
            continue
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


def check_capture_schemas(documents: list[object], failures: list[str]) -> None:
    """Both captures are contract: raw framing in the library, PNG in the Handler."""
    copies = find_tool_documents(documents, "sdl_capture")
    if not copies:
        failures.append("no sdl_capture tool schema with an inputSchema in the README")
        return

    library = [copy for copy in copies
               if "timeout_ms" not in copy["inputSchema"].get("properties", {})]
    handler = [copy for copy in copies
               if "timeout_ms" in copy["inputSchema"].get("properties", {})]
    if not library:
        failures.append("no library sdl_capture schema without timeout_ms")
    if not handler:
        failures.append("no Handler sdl_capture schema with timeout_ms")

    for copy in library:
        schema = copy["inputSchema"]
        if set(schema.get("properties", {})) != {"region"}:
            failures.append("library sdl_capture must advertise only region")
        if schema.get("additionalProperties") is not False:
            failures.append("library sdl_capture must set additionalProperties false")
        if schema.get("required"):
            failures.append("library sdl_capture has no required members")
        check_region_schema(schema, "library sdl_capture", failures)
        description = copy.get("description", "")
        if "rgba8888" not in description or "byteLength" not in description:
            failures.append(
                "library sdl_capture description must document the raw "
                "format/byteLength framing")

    for copy in handler:
        schema = copy["inputSchema"]
        if set(schema.get("properties", {})) != {"region", "timeout_ms"}:
            failures.append(
                "Handler sdl_capture must advertise only region and timeout_ms")
        check_region_schema(schema, "Handler sdl_capture", failures)
        if "PNG" not in copy.get("description", ""):
            failures.append("Handler sdl_capture description must promise a PNG")


def check_info_schema(documents: list[object], failures: list[str]) -> None:
    copies = find_tool_documents(documents, "sdl_info")
    if not copies:
        failures.append("no sdl_info tool schema with an inputSchema in the README")
        return
    for copy in copies:
        schema = copy["inputSchema"]
        if schema.get("properties") != {}:
            failures.append("sdl_info schema properties must be empty")
        if schema.get("additionalProperties") is not False:
            failures.append("sdl_info schema must set additionalProperties false")
        if contains_key(schema, "frameTimeoutMs"):
            failures.append("sdl_info schema still has frameTimeoutMs")


def check_capture_framing_prose(readme: str, failures: list[str]) -> None:
    """The raw framing and the Handler's PNG policy are prose the contract depends on."""
    for required in ('"format":"rgba8888"', '"byteLength"', "raw RGBA"):
        if required not in readme:
            failures.append(f"README does not document the capture framing phrase {required!r}")
    lowered = readme.lower()
    if ("native pixel size" not in lowered
            and "native-resolution" not in lowered):
        failures.append(
            "README must state that captures are returned at the app's native pixel size")
    if "never resized" not in lowered and "never downscaled" not in lowered:
        failures.append("README must state that captures are never resized or downscaled")
    if "base64" not in lowered:
        failures.append("README must state where base64/PNG encoding happens")
    # The region is in pixels of the frame the app wrote, not of the window, so
    # the documented bounds must be the capture statistics.
    for required in ("capture.lastWidth", "capture.lastHeight"):
        if required not in readme:
            failures.append(f"README does not document the region against {required!r}")


def check_frame_attribution_prose(readme: str, failures: list[str]) -> None:
    """A stale frame must not be mistakable for an action that had no effect."""
    lowered = readme.lower()
    if "rendered after the actions were delivered" not in lowered:
        failures.append(
            "README must state that the attached frame is rendered after the "
            "actions were delivered")
    if "attached frame" not in lowered:
        failures.append(
            "README must name the attached frame next to the applied frame")
    if "had no effect" not in lowered:
        failures.append(
            "README must say that an unchanged frame means the actions had no effect")


def check_write_frame_signature(readme: str, failures: list[str]) -> None:
    """Explicit dimensions are the contract: the app, not the window, decides."""
    signature = "SDL3MCP_WriteFrame(const uint8_t *data, size_t size, int width, int height)"
    if signature not in readme:
        failures.append(f"README must document '{signature}'")
    if "SDL3MCP_WriteFrame(const uint8_t *data, size_t size);" in readme:
        failures.append("README still documents the two-argument SDL3MCP_WriteFrame")


def check_terminate_schema(documents: list[object], failures: list[str]) -> None:
    copies = find_tool_documents(documents, "sdl_terminate")
    if not copies:
        failures.append("no sdl_terminate tool schema with an inputSchema in the README")
        return

    expected_annotations = {
        "readOnlyHint": False,
        "destructiveHint": True,
        "idempotentHint": False,
        "openWorldHint": False,
    }
    for copy in copies:
        schema = copy["inputSchema"]
        if schema.get("type") != "object":
            failures.append("sdl_terminate schema type must be object")
        if schema.get("properties") != {}:
            failures.append("sdl_terminate schema properties must be empty")
        if schema.get("additionalProperties") is not False:
            failures.append("sdl_terminate schema must set additionalProperties false")

        annotations = copy.get("annotations")
        if not isinstance(annotations, dict):
            failures.append("sdl_terminate schema has no annotations")
            continue
        for key, value in expected_annotations.items():
            if annotations.get(key) != value:
                failures.append(f"sdl_terminate annotation {key} must be {value}")


def check_catalog(catalog: list[object], expect_spawn: bool,
                  failures: list[str]) -> None:
    """The catalog the Handler advertises agrees with the documented tools."""
    by_name: dict[str, list[dict]] = {}
    for document in tool_documents(catalog):
        by_name.setdefault(document["name"], []).append(document)

    for name in (*LIBRARY_TOOLS, "wait_session"):
        if name not in by_name:
            failures.append(f"Handler catalog does not advertise {name}")

    if expect_spawn:
        spawners = [name for name in by_name if name.startswith("app_spawn_")]
        if not spawners:
            failures.append("Handler catalog has no app_spawn_<alias> entry")

    for name in ("sdl_input", "sdl_capture"):
        for copy in by_name.get(name, []):
            if "timeout_ms" not in copy["inputSchema"].get("properties", {}):
                failures.append(
                    f"Handler catalog {name} must carry timeout_ms")

    for copy in by_name.get("sdl_info", []):
        if contains_key(copy["inputSchema"], "frameTimeoutMs"):
            failures.append("Handler catalog sdl_info still has frameTimeoutMs")


def check_catalog_schemas(documents: list[object], catalog: list[object],
                          failures: list[str]) -> None:
    """A documented schema must be byte-identical to the catalog's definition."""
    by_name: dict[str, list[dict]] = {}
    for entry in tool_documents(catalog):
        by_name.setdefault(entry["name"], []).append(entry)

    for name, entries in by_name.items():
        documented = find_tool_documents(documents, name)
        if not documented:
            continue
        for entry in entries:
            if not any(entry["inputSchema"] == doc["inputSchema"]
                       for doc in documented):
                failures.append(
                    f"README {name} schema does not match the Handler catalog")
                break


def check_example_references(readme: str, failures: list[str]) -> None:
    """The bundled example is gone, so naming one is drift."""
    for pattern in EXAMPLE_REFERENCES:
        match = pattern.search(readme)
        if match:
            failures.append(
                f"README must not name an example path or app: "
                f"'{match.group(0)}'")


def check_handler_docs(readme: str, failures: list[str]) -> None:
    """The Handler is the shipped binary, so its contract is checked."""
    if "## The Handler" not in readme:
        failures.append("README has no Handler section")
    for option in ("--listen", "--connect", "--targets", "--web", "--dry-run",
                   "--timeout-ms", "--dump-tools"):
        if option not in readme:
            failures.append(f"README does not document the Handler's {option} option")
    for mode in ("Server", "Relay"):
        if f"`{mode}`" not in readme:
            failures.append(f"README does not document the Handler's {mode} mode")
    for name in ("wait_session", "app_spawn_"):
        if name not in readme:
            failures.append(f"README does not document the Handler tool {name}")
    for message in (
            "no application session is attached; call wait_session first",
            "an application is already attached; one spawn at a time",
            "the application does not provide",
            "spawn-and-forget",
    ):
        if message not in readme:
            failures.append(f"README does not document the Handler message {message!r}")


def check_library_docs(readme: str, failures: list[str]) -> None:
    for phrase in ("--mcp=off", "--mcp=HOST:PORT", "--mcp-version",
                   "SDL3MCP_DefaultConfiguration", "connect_timeout_ms",
                   "max_frame_bytes", "SDL3MCP_AppIterate"):
        if phrase not in readme:
            failures.append(f"README does not document {phrase}")
    if "1.0.0" not in readme:
        failures.append("README does not name the 1.0.0 release")


def check_stale_phrases(readme: str, failures: list[str]) -> None:
    haystack = readme.lower()
    for phrase in STALE_PHRASES:
        if phrase.lower() in haystack:
            failures.append(f"README still contains the stale phrase '{phrase}'")
    for pattern in STALE_OPTIONS:
        if pattern.search(readme):
            failures.append(
                f"README still contains the stale option {pattern.pattern!r}")


def check_quick_start(quick_start: str, failures: list[str]) -> None:
    # SDL spells the keycodes SDLK_R/G/B; compare case-insensitively so the check
    # still fails when a toggle disappears.
    haystack = quick_start.lower()
    for key in ("SDLK_r", "SDLK_g", "SDLK_b"):
        if key.lower() not in haystack:
            failures.append(f"quick start does not handle {key}")


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
        description="Verify README.md against the implemented SDL3MCP 1.0 contract.")
    parser.add_argument("--source", required=True, type=Path,
                        help="repository source directory")
    parser.add_argument("--build", required=True, type=Path,
                        help="CMake build directory with generated headers")
    parser.add_argument("--tools", type=Path,
                        help="Handler binary whose --dump-tools prints the catalog")
    parser.add_argument("--targets", type=Path,
                        help="targets file --dump-tools reads to add app_spawn_* entries")
    arguments = parser.parse_args(argv)

    readme_path = arguments.source / "README.md"
    if not readme_path.is_file():
        print(f"README check failed: {readme_path} does not exist", file=sys.stderr)
        return 1

    readme = readme_path.read_text(encoding="utf-8")
    blocks = fenced_blocks(readme)
    failures: list[str] = []

    complete_examples = [body for _, body in blocks if is_complete_example(body)]
    if not any(MAIN_CALLBACK.search(body) for body in complete_examples):
        failures.append("README has no complete main-callback example")
    if not any("int main(" in body for body in complete_examples):
        failures.append("README has no complete manual-loop example")

    documents = check_json_blocks(blocks, failures)
    check_tool_descriptions(documents, "README", failures)
    if arguments.tools is not None:
        helper = arguments.tools
        if not helper.is_file():
            failures.append(f"tool catalog helper {helper} does not exist")
    else:
        helper = find_tool_helper(arguments.build)
    catalog = load_tool_catalog(helper, arguments.targets, failures)
    if catalog:
        check_tool_descriptions(catalog, "catalog", failures)
        check_catalog(catalog, arguments.targets is not None, failures)
        check_catalog_schemas(documents, catalog, failures)
    check_tool_set(documents, failures)
    check_input_schema(documents, failures)
    check_capture_schemas(documents, failures)
    check_info_schema(documents, failures)
    check_capture_framing_prose(readme, failures)
    check_frame_attribution_prose(readme, failures)
    check_write_frame_signature(readme, failures)
    check_terminate_schema(documents, failures)
    check_example_references(readme, failures)
    check_handler_docs(readme, failures)
    check_library_docs(readme, failures)
    check_stale_phrases(readme, failures)

    for index, body in enumerate(complete_examples, start=1):
        check_loop_order(body, index, failures)

    quick_start = next((body for body in complete_examples
                        if MAIN_CALLBACK.search(body)), None)
    if quick_start is not None:
        check_quick_start(quick_start, failures)

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
