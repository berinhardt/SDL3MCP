#include "web_app.hpp"

#include <string>
#include <utility>

namespace sdl3mcp
{
namespace
{

constexpr const char *kLoopbackHost = "127.0.0.1";
constexpr const char *kPythonExecutable = "python3";
constexpr const char *kWebSocatExecutable = "websocat";
constexpr const char *kBrowserWidth = "1280";
constexpr const char *kBrowserHeight = "800";

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

} // namespace sdl3mcp
