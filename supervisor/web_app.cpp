#include "web_app.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <string_view>
#include <utility>

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace sdl3mcp
{
namespace
{

constexpr const char *kPythonExecutable = "python3";
constexpr const char *kWebSocatExecutable = "websocat";
constexpr const char *kBrowserWidth = "1280";
constexpr const char *kBrowserHeight = "800";

/* The template mkdtemp() fills; its suffix must be the last six characters. */
constexpr const char *kProfileTemplate = "sdl3mcp-web-XXXXXX";
constexpr const char *kLockFile = ".lock";
constexpr const char *kFallbackProfileParent = "/tmp";

/* The prefix a seeded profile carries, which is also what the sweep looks for. */
constexpr const char *kProfilePrefix = "sdl3mcp-web-";
/* Long enough that a directory still being created is never mistaken for one. */
constexpr int kUnlockedGraceSeconds = 60;

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

/* The mtime test that keeps the sweep off a directory a launcher is creating. */
bool olderThan(const std::string &path, int seconds)
{
    struct stat status {};
    if (::stat(path.c_str(), &status) != 0) {
        return false;
    }
    return std::time(nullptr) - status.st_mtime > seconds;
}

} // namespace

bool preflightWebLaunch(WebAppOptions &options, std::string &error)
{
    std::string websocat;
    if (!findOnPath(kWebSocatExecutable, websocat)) {
        return fail("websocat not found: install websocat (cargo install websocat)", error);
    }

    std::string python;
    if (!findOnPath(kPythonExecutable, python)) {
        return fail("python3 not found: install python3", error);
    }

    struct stat status {};
    if (::stat(options.pageRoot.c_str(), &status) != 0 || !S_ISDIR(status.st_mode)) {
        return fail("page root " + options.pageRoot + " is not a directory", error);
    }
    const std::string entryPath = options.pageRoot + "/" + options.entry;
    if (::stat(entryPath.c_str(), &status) != 0) {
        return fail("page root " + options.pageRoot + " does not contain " +
                        options.entry,
                    error);
    }

    options.websocat = std::move(websocat);
    options.python = std::move(python);
    return true;
}

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
    if (!isExecutable(options.python)) {
        return fail("python must name an executable", error);
    }
    if (!isExecutable(options.websocat)) {
        return fail("websocat must name an executable", error);
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
    const std::string bridgeEndpoint = endpointText(kWebLoopbackHost, options.bridgePort);

    std::vector<std::string> pageServer{
        options.python,
        "-m",
        "http.server",
        pagePort,
        "--bind",
        kWebLoopbackHost,
        "--directory",
        options.pageRoot,
    };

    std::vector<std::string> bridge{
        options.websocat,
        "--binary",
        "ws-l:" + std::string(kWebLoopbackHost) + ":" + bridgePort,
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
        "http://" + std::string(kWebLoopbackHost) + ":" + pagePort + "/" + options.entry +
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

bool seedProfile(std::string &directory, int &lockFd, std::string &error)
{
    const std::string parent = profileParent();
    std::string candidate = parent + "/" + kProfileTemplate;
    if (::mkdtemp(candidate.data()) == nullptr) {
        const int createError = errno;
        return fail("cannot create a profile directory under " + parent + ": " +
                        std::strerror(createError),
                    error);
    }

    /*
      The lock is the profile's ownership: the kernel drops it when the descriptor
      closes, including on SIGKILL, which a PID file cannot promise. O_CLOEXEC keeps
      the three children from inheriting it and making a dead run look alive.
    */
    const int fd =
        ::open((candidate + "/" + kLockFile).c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0) {
        const int openError = errno;
        ::rmdir(candidate.c_str());
        return fail("cannot create the profile lock: " +
                        std::string(std::strerror(openError)),
                    error);
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        const int lockError = errno;
        ::close(fd);
        ::rmdir(candidate.c_str());
        return fail("cannot lock the profile directory: " +
                        std::string(std::strerror(lockError)),
                    error);
    }
    if (!writePreferences(candidate, error)) {
        ::close(fd);
        return false;
    }

    directory = std::move(candidate);
    lockFd = fd;
    return true;
}

int sweepAbandonedProfiles(std::string &error)
{
    error.clear();
    const std::string parent = profileParent();
    DIR *directory = ::opendir(parent.c_str());
    if (directory == nullptr) {
        error = "cannot scan " + parent + ": " + std::strerror(errno);
        return -1;
    }

    int removed = 0;
    while (const dirent *entry = ::readdir(directory)) {
        const std::string name(entry->d_name);
        if (name.rfind(kProfilePrefix, 0) != 0) {
            continue;
        }
        const std::string path = parent + "/" + name;
        std::string ignored;
        const int fd = ::open((path + "/" + kLockFile).c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            if (errno == ENOENT && olderThan(path, kUnlockedGraceSeconds)) {
                if (removeProfileTree(path, ignored)) {
                    ++removed;
                }
            }
            continue;
        }
        const bool owned = ::flock(fd, LOCK_EX | LOCK_NB) != 0;
        ::close(fd);
        if (owned) {
            continue;
        }
        if (removeProfileTree(path, ignored)) {
            ++removed;
        }
    }
    ::closedir(directory);
    return removed;
}

std::string plannedProfilePath()
{
    return profileParent() + "/" + kProfileTemplate;
}

} // namespace sdl3mcp
