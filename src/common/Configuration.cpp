#include "Configuration.h"

#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

#include "Configuration-Limits.h"

namespace sdl3mcp {
namespace {

using Status = Configuration::Status;

constexpr std::string_view kMcpPrefix = "--mcp";

}  // namespace

Configuration::Configuration() noexcept : value_(SDL3MCP_DefaultConfiguration()) {
}

Configuration::Configuration(const SDL3MCP_Configuration& source) noexcept
    : value_(source) {
}

Status Configuration::parseCommandLine(int argc, char** argv) {
   message_.clear();

   SDL3MCP_Configuration candidate = value_;
   if (argv != nullptr) {
      for (int index = 1; index < argc; ++index) {
         if (argv[index] == nullptr) {
            continue;
         }
         const std::string_view argument(argv[index]);
         if (!argument.starts_with(kMcpPrefix)) {
            continue;
         }
         const Status status = applyArgument(candidate, argument);
         if (status != Status::Continue) {
            return status;
         }
      }
   }

   const Status status = validateValues(candidate);
   if (status != Status::Continue) {
      return status;
   }

   value_ = candidate;
   return Status::Continue;
}

Status Configuration::validate() {
   message_.clear();
   return validateValues(value_);
}

Status Configuration::validateValues(const SDL3MCP_Configuration& candidate) {
   if (candidate.connect_timeout_ms < kMinConnectTimeoutMs ||
       candidate.connect_timeout_ms > kMaxConnectTimeoutMs) {
      return fail("connect_timeout_ms must be in 1..60000, got " +
                  std::to_string(candidate.connect_timeout_ms));
   }
   if (candidate.max_frame_bytes < kMinMaxFrameBytes ||
       candidate.max_frame_bytes > kMaxMaxFrameBytes) {
      return fail("max_frame_bytes must be in 1048576..1073741824, got " +
                  std::to_string(candidate.max_frame_bytes));
   }
   if (candidate.log_level < kMinLogLevel || candidate.log_level > kMaxLogLevel) {
      return fail("log_level must be in -1..4, got " +
                  std::to_string(candidate.log_level));
   }

   const void* terminator =
       std::memchr(candidate.connect_host, '\0', sizeof(candidate.connect_host));
   const std::size_t hostLength =
       terminator == nullptr
           ? sizeof(candidate.connect_host)
           : static_cast<std::size_t>(static_cast<const char*>(terminator) -
                                      candidate.connect_host);

   if (terminator == nullptr) {
      return fail("connect_host must be a non-empty string shorter than 64 bytes");
   }
   if (hostLength == 0 && candidate.connect_port != 0) {
      return fail("connect_port is set without a connect_host");
   }
   if (hostLength != 0 && candidate.connect_port == 0) {
      return fail("connect_port must be in 1..65535, got 0");
   }

   return Status::Continue;
}

Status Configuration::fail(std::string message) {
   message_ = std::move(message);
   return Status::Error;
}

}  // namespace sdl3mcp
