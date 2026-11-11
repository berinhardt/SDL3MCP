#include "web_app.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

#include <sys/stat.h>
#include <unistd.h>

namespace sdl3mcp
{
namespace
{

constexpr const char *kLoopbackHost = "127.0.0.1";
constexpr const char *kPythonExecutable = "python3";
constexpr const char *kWebSocatExecutable = "websocat";
constexpr const char *kBrowserWidth = "1280";
constexpr const char *kBrowserHeight = "800";

/* The template mkdtemp() fills; its suffix must be the last six characters. */
constexpr const char *kProfileTemplate = "sdl3mcp-web-XXXXXX";
constexpr const char *kFallbackProfileParent = "/tmp";

/*
  One line per preference. The first is the reason the profile exists: the page
  calls window.close() when the application shuts down, and Firefox only allows
  that for a window a script opened or when this preference says so. The rest
  keep a throwaway profile from stopping on first-run prompts.
*/
constexpr const char *kProfilePreferences =
    "user_pref(\"dom.allow_scripts_to_close_windows\", true);\n"
    "user_pref(\"browser.tabs.warnOnClose\", false);\n"
    "user_pref(\"browser.shell.checkDefaultBrowser\", false);\n"
    "user_pref(\"browser.startup.page\", 0);\n"
    "user_pref(\"datareporting.policy.dataSubmissionEnabled\", false);\n"
    "user_pref(\"app.update.auto\", false);\n";

/*
  "HOST:PORT" with an IPv6 literal bracketed, which is the only spelling the
  application's endpoint parser and websocat's TCP address both accept.
*/
std::string endpointText(const std::string &host, std::uint16_t port)
{
    std::string text;
    if (host.find(':') != std::string::npos) {
        text += '[';
        text += host;
        text += ']';
    } else {
        text = host;
    }
    text += ':';
    text += std::to_string(port);
    return text;
}

bool fail(std::string message, std::string &error)
{
    error = std::move(message);
    return false;
}

std::string baseName(const std::string &path)
{
    const std::size_t separator = path.find_last_of('/');
    return separator == std::string::npos ? path : path.substr(separator + 1);
}

bool isExecutable(const std::string &path)
{
    return !path.empty() && ::access(path.c_str(), X_OK) == 0;
}

/* realpath() turns a PATH hit into the absolute path the plan prints. */
bool absolutePath(const std::string &path, std::string &absolute)
{
    char *resolved = ::realpath(path.c_str(), nullptr);
    if (resolved == nullptr) {
        return false;
    }
    absolute.assign(resolved);
    std::free(resolved);
    return true;
}

bool findOnPath(const char *name, std::string &found)
{
    const char *path = std::getenv("PATH");
    if (path == nullptr) {
        return false;
    }

    /* An empty entry would mean the current directory; skip it. */
    std::string_view remaining(path);
    while (!remaining.empty()) {
        const std::size_t separator = remaining.find(':');
        const std::string_view entry = remaining.substr(0, separator);
        if (!entry.empty()) {
            std::string candidate(entry);
            candidate += '/';
            candidate += name;
            if (isExecutable(candidate) && absolutePath(candidate, found)) {
                return true;
            }
        }
        if (separator == std::string_view::npos) {
            break;
        }
        remaining.remove_prefix(separator + 1);
    }
    return false;
}

bool isWritableDirectory(const char *path)
{
    if (path == nullptr || path[0] == '\0') {
        return false;
    }
    struct stat status {};
    return ::stat(path, &status) == 0 && S_ISDIR(status.st_mode) &&
           ::access(path, W_OK) == 0;
}

std::string profileParent()
{
    const char *runtime = std::getenv("XDG_RUNTIME_DIR");
    if (isWritableDirectory(runtime)) {
        return runtime;
    }
    return kFallbackProfileParent;
}

bool writePreferences(const std::string &directory, std::string &error)
{
    const std::string path = directory + "/user.js";
    std::FILE *file = std::fopen(path.c_str(), "w");
    if (file == nullptr) {
        const int openError = errno;
        return fail("cannot write " + path + ": " + std::strerror(openError), error);
    }

    const std::size_t length = std::strlen(kProfilePreferences);
    const bool written =
        std::fwrite(kProfilePreferences, 1, length, file) == length;
    const bool closed = std::fclose(file) == 0;
    if (!written || !closed) {
        return fail("cannot write " + path, error);
    }
    return true;
}

} // namespace

bool makeLaunchPlan(const WebAppOptions &options,
                    std::vector<std::vector<std::string>> &plan,
                    std::string &error)
{
    plan.clear();

    if (options.pageRoot.empty()) {
        return fail("pageRoot must not be empty", error);
    }
    if (options.entry.empty()) {
        return fail("entry must not be empty", error);
    }
    if (options.browser.empty()) {
        return fail("browser must not be empty", error);
    }
    if (options.relayPort == 0) {
        return fail("relayPort must not be zero", error);
    }
    if (options.pagePort == 0) {
        return fail("pagePort must not be zero", error);
    }
    if (options.bridgePort == 0) {
        return fail("bridgePort must not be zero", error);
    }
    if (options.pagePort == options.bridgePort) {
        return fail("pagePort and bridgePort must differ", error);
    }

    const std::string pagePort = std::to_string(options.pagePort);
    const std::string bridgePort = std::to_string(options.bridgePort);
    const std::string bridgeEndpoint = endpointText(kLoopbackHost, options.bridgePort);

    std::vector<std::string> pageServer{
        kPythonExecutable,
        "-m",
        "http.server",
        pagePort,
        "--bind",
        kLoopbackHost,
        "--directory",
        options.pageRoot,
    };

    std::vector<std::string> bridge{
        kWebSocatExecutable,
        "--binary",
        "ws-l:" + std::string(kLoopbackHost) + ":" + bridgePort,
        "tcp:" + endpointText(options.relayHost, options.relayPort),
    };

    std::vector<std::string> browser{
        options.browser,
        "--no-remote",
        "--new-instance",
        "--profile",
        options.profileDir,
        "--width",
        kBrowserWidth,
        "--height",
        kBrowserHeight,
        "http://" + std::string(kLoopbackHost) + ":" + pagePort + "/" + options.entry +
            "?mcp=" + bridgeEndpoint,
    };

    plan.push_back(std::move(pageServer));
    plan.push_back(std::move(bridge));
    plan.push_back(std::move(browser));
    return true;
}

bool resolveBrowser(std::string &path, std::string &error)
{
    const char *configured = std::getenv("BROWSER");
    if (configured != nullptr) {
        const std::string candidate(configured);
        const std::string name = baseName(candidate);
        std::string absolute;
        if ((name == "firefox" || name == "firefox-esr") && isExecutable(candidate) &&
            absolutePath(candidate, absolute)) {
            path = std::move(absolute);
            return true;
        }
    }

    for (const char *name : {"firefox", "firefox-esr"}) {
        std::string found;
        if (findOnPath(name, found)) {
            path = std::move(found);
            return true;
        }
    }

    return fail("no isolated browser available: install firefox; "
                "no other browser is supported",
                error);
}

bool seedProfile(std::string &directory, std::string &error)
{
    const std::string parent = profileParent();
    std::string candidate = parent + "/" + kProfileTemplate;
    if (::mkdtemp(candidate.data()) == nullptr) {
        const int createError = errno;
        return fail("cannot create a profile directory under " + parent + ": " +
                        std::strerror(createError),
                    error);
    }
    if (!writePreferences(candidate, error)) {
        return false;
    }

    directory = std::move(candidate);
    return true;
}

} // namespace sdl3mcp
