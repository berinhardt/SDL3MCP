/*
  The spawner's alias-to-command sources.

  --targets=PATH may be repeated. A path that is a regular file names a JSON
  object whose keys are aliases and whose values are full argv arrays; a path
  that is a directory contributes every top-level *.json file in sorted filename
  order. The alias becomes an MCP tool name (app_spawn_<alias>), so a key is
  restricted to [a-z0-9_]+ and may not use the reserved app_spawn_ prefix. The
  arrays are the command line as written, including argv[0]; the spawner only
  appends --mcp=<endpoint>.

  All sources accumulate into one map. An alias defined by two sources is an
  error naming both files rather than a silent last-one-wins, because that would
  make one of the two binaries unreachable.

  Validation happens once at startup and every failure is fatal. Relative paths
  inside an entry are left untouched, so the child resolves them against the
  spawner's working directory because the spawner never chdirs.
*/

#ifndef SDL3MCP_SUPERVISOR_TARGETS_HPP
#define SDL3MCP_SUPERVISOR_TARGETS_HPP

#include <string>
#include <vector>

namespace sdl3mcp
{

struct SpawnTarget
{
    /* The tool suffix and the app_spawn_<alias> name. */
    std::string alias;
    /* argv including argv[0], exactly as the file lists it. */
    std::vector<std::string> argv;
};

using SpawnTargets = std::vector<SpawnTarget>;

/*
  Reads and validates every source in `paths` and accumulates their entries. On
  failure `targets` is empty and `error` names the offending path plus the
  offending key or value. Relative paths resolve against CWD.
*/
bool loadSpawnTargets(const std::vector<std::string> &paths, SpawnTargets &targets,
                      std::string &error);

/* The single-source form, equivalent to passing a one-element vector. */
bool loadSpawnTargets(const std::string &path, SpawnTargets &targets, std::string &error);

} // namespace sdl3mcp

#endif /* SDL3MCP_SUPERVISOR_TARGETS_HPP */
