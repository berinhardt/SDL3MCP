/*
  The --listen and --connect address arguments, lifted out of HandlerCli.cpp's
  parse loop.

  applyEndpointOption has exactly one caller, but inlining it would take
  HandlerCli.cpp past the file-size hard limit, so it lives here with its own
  header (STYLE.md §Helper placement).
*/

#pragma once

#include <string_view>

#include "HandlerCli.h"

namespace sdl3mcp {

/*
  Applies one argument when it names --listen or --connect.

  Returns false when the argument belongs to another option and the parse loop
  should keep matching. Returns true when it was an endpoint option: either
  result.options now carries the parsed value, or result.error names why the
  argument was rejected.
*/
bool applyEndpointOption(std::string_view argument, HandlerCliResult& result);

}  // namespace sdl3mcp
