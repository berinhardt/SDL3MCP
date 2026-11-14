/*
  The Handler's --web sources and the alias dictionary they make.

  A --web path is a JSON file mapping each alias to a page root. Every entry
  contributes app_spawn_<alias>, exactly like a --targets entry, and both
  sources accumulate into one alias map. The page entry is always index.html;
  a relative root is left as written and resolves against the Handler's working
  directory, because the Handler never chdirs.

  A web alias cannot exec a binary, so the loaded roots feed the launch plan in
  Web-Plan.h rather than a spawn command.
*/

#pragma once

#include <string>
#include <vector>

#include "Result.h"

namespace sdl3mcp {

struct WebApp {
   /* The tool suffix and the app_spawn_<alias> name. */
   std::string alias;
   /* The page root exactly as the file wrote it. */
   std::string root;
   /* The --web file that defined the alias. */
   std::string source;
};

using WebApps = std::vector<WebApp>;

/*
  Reads and validates every --web file in `paths` and accumulates its entries.
  A failure names the offending path plus the offending key or value.
*/
Result<WebApps, ProtocolError> loadWebApps(const std::vector<std::string>& paths);

}  // namespace sdl3mcp
