/*
  The spawner's alias-to-command file.

  --targets=FILE names a JSON object whose keys are aliases and whose values are
  full argv arrays. The alias becomes an MCP tool name (app_spawn_<alias>), so a
  key is restricted to [a-z0-9_]+ and may not use the reserved app_spawn_ prefix.
  The arrays are the command line as written, including argv[0]; the spawner only
  appends --mcp=<endpoint>.

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
  Reads and validates `path`. On failure `targets` is empty and `error` names the
  file plus the offending key or value. Relative `path` resolves against CWD.
*/
bool loadSpawnTargets(const std::string &path, SpawnTargets &targets, std::string &error);

} // namespace sdl3mcp

#endif /* SDL3MCP_SUPERVISOR_TARGETS_HPP */
