/*
  The Handler's web source: the alias-to-root dictionary, the launch plan, and
  the throwaway Firefox profile. Every case here is a pure value operation or a
  filesystem fact the test creates itself, so no browser, no bridge and no page
  server is involved; the end-to-end web spawn lives in handler_tests.
*/

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "Web.h"

#include <dirent.h>
#include <doctest/doctest.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace {

/* A private file the test owns, removed on destruction. */
struct TempPath {
   std::string path;

   TempPath() {
      char buffer[] = "/tmp/sdl3mcp-web-test-XXXXXX";
      const int fd = ::mkstemp(buffer);
      REQUIRE(fd >= 0);
      if (fd >= 0) {
         ::close(fd);
      }
      path = buffer;
   }

   ~TempPath() {
      if (!path.empty()) {
         ::unlink(path.c_str());
      }
   }

   TempPath(const TempPath&) = delete;
   TempPath& operator=(const TempPath&) = delete;
};

void writeFile(const std::string& path, const std::string& contents) {
   std::FILE* file = std::fopen(path.c_str(), "wb");
   REQUIRE(file != nullptr);
   CHECK(std::fwrite(contents.data(), 1, contents.size(), file) == contents.size());
   CHECK(std::fclose(file) == 0);
}

bool exists(const std::string& path) {
   struct stat status{};
   return ::stat(path.c_str(), &status) == 0;
}

std::string readFile(const std::string& path) {
   std::FILE* file = std::fopen(path.c_str(), "rb");
   REQUIRE(file != nullptr);
   std::string contents;
   char chunk[256];
   std::size_t count = 0;
   while ((count = std::fread(chunk, 1, sizeof chunk, file)) > 0) {
      contents.append(chunk, count);
   }
   std::fclose(file);
   return contents;
}

/* A private directory emptied and removed on destruction. */
struct TempDir {
   std::string path;

   TempDir() {
      char buffer[] = "/tmp/sdl3mcp-web-dir-XXXXXX";
      const char* created = ::mkdtemp(buffer);
      REQUIRE(created != nullptr);
      path = created;
   }

   ~TempDir() {
      if (path.empty()) {
         return;
      }
      if (DIR* directory = ::opendir(path.c_str())) {
         while (const dirent* entry = ::readdir(directory)) {
            const std::string name = entry->d_name;
            if (name != "." && name != "..") {
               ::unlink((path + "/" + name).c_str());
            }
         }
         ::closedir(directory);
      }
      ::rmdir(path.c_str());
   }

   TempDir(const TempDir&) = delete;
   TempDir& operator=(const TempDir&) = delete;

   std::string file(const std::string& name) const { return path + "/" + name; }
};

}  // namespace

TEST_CASE("web files validate and accumulate") {
   using sdl3mcp::loadWebApps;
   using sdl3mcp::WebApps;

   const auto failsWith = [](const std::string& contents,
                             const std::string& needle) {
      TempPath file;
      writeFile(file.path, contents);
      WebApps apps;
      std::string error;
      CHECK(!loadWebApps({file.path}, apps, error));
      if (error.find(needle) == std::string::npos) {
         std::fprintf(stderr, "error for %s did not name %s: %s\n",
                      contents.c_str(), needle.c_str(), error.c_str());
      }
      CHECK(error.find(needle) != std::string::npos);
      CHECK(apps.empty());
   };

   failsWith("{", "not valid JSON");
   failsWith("[]", "JSON object");
   failsWith("{}", "defines no aliases");
   failsWith(R"({"Bad":"build/web"})", "Bad");
   failsWith(R"({"bad-key":"build/web"})", "bad-key");
   failsWith(R"({"app_spawn_x":"build/web"})", "app_spawn_x");
   failsWith(R"({"x":5})", "x");
   failsWith(R"({"x":""})", "x");

   WebApps apps;
   std::string error;
   CHECK(!loadWebApps({"/nonexistent/sdl3mcp-web.json"}, apps, error));
   CHECK(error.find("cannot open") != std::string::npos);

   /* A --web path names one JSON file, never a directory. */
   TempDir directory;
   CHECK(!loadWebApps({directory.path}, apps, error));
   CHECK(error.find(directory.path) != std::string::npos);
   CHECK(error.find("not a file") != std::string::npos);

   /* The root is kept exactly as written, relative or absolute. */
   TempPath valid;
   writeFile(valid.path, R"({"alpha":"build/web","beta":"./web-two"})");
   CHECK(loadWebApps({valid.path}, apps, error));
   REQUIRE(apps.size() == 2);
   CHECK(apps[0].alias == "alpha");
   CHECK(apps[0].root == "build/web");
   CHECK(apps[0].source == valid.path);
   CHECK(apps[1].alias == "beta");
   CHECK(apps[1].root == "./web-two");

   /* Repeating --web unions the files in command-line order. */
   TempPath extra;
   writeFile(extra.path, R"({"gamma":"web-three"})");
   CHECK(loadWebApps({valid.path, extra.path}, apps, error));
   REQUIRE(apps.size() == 3);
   CHECK(apps[2].alias == "gamma");
   CHECK(apps[2].root == "web-three");

   /* A duplicate alias across two files names both and replaces neither. */
   TempPath first;
   TempPath second;
   writeFile(first.path, R"({"alpha":"a"})");
   writeFile(second.path, R"({"alpha":"b"})");
   CHECK(!loadWebApps({first.path, second.path}, apps, error));
   CHECK(apps.empty());
   CHECK(error.find(first.path) != std::string::npos);
   CHECK(error.find(second.path) != std::string::npos);

   /* The second malformed file is named, not the first. */
   TempPath good;
   TempPath broken;
   writeFile(good.path, R"({"good":"root"})");
   writeFile(broken.path, "{");
   CHECK(!loadWebApps({good.path, broken.path}, apps, error));
   CHECK(error.find(broken.path) != std::string::npos);
}

namespace {

/* Saves and restores one environment variable, so PATH/BROWSER cases do not
   leak into another test. */
struct EnvGuard {
   std::string name;
   bool hadValue = false;
   std::string value;

   explicit EnvGuard(std::string variable) : name(std::move(variable)) {
      const char* existing = std::getenv(name.c_str());
      if (existing != nullptr) {
         hadValue = true;
         value = existing;
      }
   }

   ~EnvGuard() {
      if (hadValue) {
         ::setenv(name.c_str(), value.c_str(), 1);
      } else {
         ::unsetenv(name.c_str());
      }
   }

   EnvGuard(const EnvGuard&) = delete;
   EnvGuard& operator=(const EnvGuard&) = delete;
};

void makeExecutable(const std::string& path) {
   REQUIRE(::chmod(path.c_str(), 0755) == 0);
}

/* A page root holding the mandatory index.html. */
struct WebRoot {
   TempDir directory;

   WebRoot() { writeFile(directory.file("index.html"), "<!doctype html>"); }
};

}  // namespace

TEST_CASE("the web launch plan is the three documented commands") {
   sdl3mcp::WebLaunchOptions options;
   options.pageRoot = "/srv/page root";
   options.python = "/usr/bin/python3";
   options.websocat = "/usr/bin/websocat";
   options.browser = "/usr/bin/firefox";
   options.handlerEndpoint = "127.0.0.1:7777";
   options.profileDir = "/tmp/sdl3mcp-web-abc123";
   options.pagePort = 18080;
   options.bridgePort = 18081;

   std::vector<std::vector<std::string>> plan;
   std::string error;
   REQUIRE(sdl3mcp::makeWebLaunchPlan(options, plan, error));
   REQUIRE(plan.size() == 3);

   const std::vector<std::string> expectedPage{
       "/usr/bin/python3", "-m", "http.server", "18080",
       "--bind", "127.0.0.1", "--directory", "/srv/page root"};
   const std::vector<std::string> expectedBridge{
       "/usr/bin/websocat", "--binary", "ws-l:127.0.0.1:18081",
       "tcp:127.0.0.1:7777"};
   const std::vector<std::string> expectedBrowser{
       "/usr/bin/firefox", "--no-remote", "--new-instance",
       "--profile", "/tmp/sdl3mcp-web-abc123", "--width", "1280",
       "--height", "800",
       "http://127.0.0.1:18080/index.html?mcp=127.0.0.1:18081"};
   CHECK(plan[0] == expectedPage);
   CHECK(plan[1] == expectedBridge);
   CHECK(plan[2] == expectedBrowser);

   /* The entry is always index.html, even when an option object is fresh. */
   CHECK(options.entry == "index.html");

   /* An IPv6 handler endpoint stays bracketed for websocat's TCP address. */
   options.handlerEndpoint = "[::1]:7777";
   REQUIRE(sdl3mcp::makeWebLaunchPlan(options, plan, error));
   CHECK(plan[1][3] == "tcp:[::1]:7777");

   const auto failsWith = [&options](const std::string& needle) {
      std::vector<std::vector<std::string>> rejected;
      std::string failure;
      CHECK(!sdl3mcp::makeWebLaunchPlan(options, rejected, failure));
      CHECK(rejected.empty());
      if (failure.find(needle) == std::string::npos) {
         std::fprintf(stderr, "plan error did not name %s: %s\n", needle.c_str(),
                      failure.c_str());
      }
      CHECK(failure.find(needle) != std::string::npos);
   };

   sdl3mcp::WebLaunchOptions valid = options;
   options.pageRoot.clear();
   failsWith("pageRoot");
   options = valid;
   options.python = "/nonexistent/python3";
   failsWith("python");
   options = valid;
   options.websocat = "/nonexistent/websocat";
   failsWith("websocat");
   options = valid;
   options.browser.clear();
   failsWith("browser");
   options = valid;
   options.handlerEndpoint.clear();
   failsWith("handlerEndpoint");
   options = valid;
   options.pagePort = 0;
   failsWith("pagePort");
   options = valid;
   options.bridgePort = 0;
   failsWith("bridgePort");
   options = valid;
   options.bridgePort = options.pagePort;
   failsWith("must differ");
}

TEST_CASE("the web profile is seeded, locked, and removed") {
   std::string directory;
   int lockFd = -1;
   std::string error;
   REQUIRE(sdl3mcp::seedProfile(directory, lockFd, error));
   CHECK(lockFd >= 0);
   CHECK(directory.find("sdl3mcp-web-") != std::string::npos);

   const std::string preferences = readFile(directory + "/user.js");
   CHECK(preferences.find("dom.allow_scripts_to_close_windows") !=
         std::string::npos);
   CHECK(preferences.find("true") != std::string::npos);
   CHECK(exists(directory + "/.lock"));

   /* The lock is ownership: a second launcher cannot take it while we hold it. */
   const int other = ::open((directory + "/.lock").c_str(), O_RDWR | O_CLOEXEC);
   REQUIRE(other >= 0);
   CHECK(::flock(other, LOCK_EX | LOCK_NB) != 0);
   ::close(other);

   CHECK(sdl3mcp::removeProfileTree(directory, error));
   CHECK(!exists(directory));
   ::close(lockFd);

   /* A second seed makes a different directory. */
   std::string second;
   int secondLock = -1;
   REQUIRE(sdl3mcp::seedProfile(second, secondLock, error));
   CHECK(second != directory);
   CHECK(sdl3mcp::removeProfileTree(second, error));
   ::close(secondLock);
}

TEST_CASE("the sweep reclaims abandoned profiles and keeps live ones") {
   const std::string templatePath = sdl3mcp::plannedProfilePath();
   CHECK(templatePath.size() > std::strlen("sdl3mcp-web-XXXXXX"));
   CHECK(templatePath.compare(templatePath.size() - std::strlen("sdl3mcp-web-XXXXXX"),
                              std::strlen("sdl3mcp-web-XXXXXX"),
                              "sdl3mcp-web-XXXXXX") == 0);
   const std::string parent =
       templatePath.substr(0, templatePath.rfind('/'));

   /* A directory with no .lock is swept only once it is a minute old. */
   std::string oldPath = parent + "/sdl3mcp-web-XXXXXX";
   REQUIRE(::mkdtemp(oldPath.data()) != nullptr);
   CHECK(exists(oldPath));
   const struct timespec twoMinutesAgo{};
   struct timespec times[2] = {twoMinutesAgo, {}};
   times[0].tv_sec = ::time(nullptr) - 120;
   times[1].tv_sec = ::time(nullptr) - 120;
   REQUIRE(::utimensat(AT_FDCWD, oldPath.c_str(), times, 0) == 0);

   std::string error;
   CHECK(sdl3mcp::sweepAbandonedProfiles(error) >= 0);
   CHECK(!exists(oldPath));

   /* A fresh directory may be a launcher between mkdtemp and its lock. */
   std::string freshPath = parent + "/sdl3mcp-web-XXXXXX";
   REQUIRE(::mkdtemp(freshPath.data()) != nullptr);
   CHECK(sdl3mcp::sweepAbandonedProfiles(error) >= 0);
   CHECK(exists(freshPath));
   ::rmdir(freshPath.c_str());

   /* A locked profile belongs to a live run and is left alone. */
   std::string livePath;
   int liveLock = -1;
   REQUIRE(sdl3mcp::seedProfile(livePath, liveLock, error));
   CHECK(sdl3mcp::sweepAbandonedProfiles(error) >= 0);
   CHECK(exists(livePath));
   CHECK(sdl3mcp::removeProfileTree(livePath, error));
   ::close(liveLock);
}

TEST_CASE("the browser is resolved only to Firefox") {
   EnvGuard path("PATH");
   EnvGuard browser("BROWSER");
   TempDir bin;

   const std::string firefox = bin.file("firefox");
   writeFile(firefox, "#!/bin/sh\n");
   makeExecutable(firefox);
   const std::string firefoxEsr = bin.file("firefox-esr");
   writeFile(firefoxEsr, "#!/bin/sh\n");
   makeExecutable(firefoxEsr);
   const std::string chromium = bin.file("chromium");
   writeFile(chromium, "#!/bin/sh\n");
   makeExecutable(chromium);

   ::setenv("PATH", bin.path.c_str(), 1);

   std::string resolved;
   std::string error;

   /* $BROWSER naming Firefox wins. */
   ::setenv("BROWSER", firefoxEsr.c_str(), 1);
   REQUIRE(sdl3mcp::resolveBrowser(resolved, error));
   CHECK(resolved == firefoxEsr);

   /* Any other browser in $BROWSER is ignored in favour of PATH. */
   ::setenv("BROWSER", chromium.c_str(), 1);
   REQUIRE(sdl3mcp::resolveBrowser(resolved, error));
   CHECK(resolved == firefox);

   /* With Firefox missing there is no fallback. */
   ::unsetenv("BROWSER");
   REQUIRE(::unlink(firefox.c_str()) == 0);
   REQUIRE(::unlink(firefoxEsr.c_str()) == 0);
   CHECK(!sdl3mcp::resolveBrowser(resolved, error));
   CHECK(error.find("firefox") != std::string::npos);
}

TEST_CASE("the web preflight resolves tools and checks the root") {
   EnvGuard path("PATH");
   EnvGuard browser("BROWSER");
   TempDir bin;

   const std::string python = bin.file("python3");
   writeFile(python, "#!/bin/sh\n");
   makeExecutable(python);
   const std::string websocat = bin.file("websocat");
   writeFile(websocat, "#!/bin/sh\n");
   makeExecutable(websocat);
   const std::string firefox = bin.file("firefox");
   writeFile(firefox, "#!/bin/sh\n");
   makeExecutable(firefox);

   ::setenv("PATH", bin.path.c_str(), 1);
   ::unsetenv("BROWSER");

   WebRoot root;
   sdl3mcp::WebLaunchOptions options;
   options.pageRoot = root.directory.path;
   options.handlerEndpoint = "127.0.0.1:7777";

   std::string error;
   REQUIRE(sdl3mcp::preflightWebLaunch(options, error));
   CHECK(options.python == python);
   CHECK(options.websocat == websocat);
   CHECK(options.browser == firefox);
   CHECK(options.entry == "index.html");

   /* A root without the entry is one sentence naming it. */
   sdl3mcp::WebLaunchOptions missing = options;
   missing.pageRoot = bin.path;
   CHECK(!sdl3mcp::preflightWebLaunch(missing, error));
   CHECK(error.find("index.html") != std::string::npos);

   sdl3mcp::WebLaunchOptions notDirectory = options;
   notDirectory.pageRoot = root.directory.file("index.html");
   CHECK(!sdl3mcp::preflightWebLaunch(notDirectory, error));
   CHECK(error.find("not a directory") != std::string::npos);

   /* A missing tool is named before anything is created. */
   REQUIRE(::unlink(websocat.c_str()) == 0);
   sdl3mcp::WebLaunchOptions noWebsocat = options;
   CHECK(!sdl3mcp::preflightWebLaunch(noWebsocat, error));
   CHECK(error.find("websocat") != std::string::npos);

   REQUIRE(::unlink(python.c_str()) == 0);
   sdl3mcp::WebLaunchOptions neither = options;
   CHECK(!sdl3mcp::preflightWebLaunch(neither, error));
   CHECK(error.find("websocat") != std::string::npos);
}

TEST_CASE("web ports are ephemeral and distinct") {
   std::string error;
   std::uint16_t page = 0;
   std::uint16_t bridge = 0;
   REQUIRE(sdl3mcp::pickLoopbackPort(page, error));
   CHECK(page != 0);

   sdl3mcp::WebLaunchOptions options;
   REQUIRE(sdl3mcp::pickWebPorts(options, error));
   CHECK(options.pagePort != 0);
   CHECK(options.bridgePort != 0);
   CHECK(options.pagePort != options.bridgePort);
}

TEST_CASE("the prepared launch owns a profile and two ports") {
   EnvGuard path("PATH");
   EnvGuard browser("BROWSER");
   TempDir bin;

   for (const char* name : {"python3", "websocat", "firefox"}) {
      const std::string executable = bin.file(name);
      writeFile(executable, "#!/bin/sh\n");
      makeExecutable(executable);
   }
   ::setenv("PATH", bin.path.c_str(), 1);
   ::unsetenv("BROWSER");

   WebRoot root;
   sdl3mcp::WebLaunchOptions options;
   options.pageRoot = root.directory.path;
   options.handlerEndpoint = "127.0.0.1:7777";

   std::string error;
   REQUIRE(sdl3mcp::prepareWebLaunch(options, error));
   CHECK(options.profileLockFd >= 0);
   CHECK(exists(options.profileDir));
   CHECK(options.pagePort != 0);
   CHECK(options.bridgePort != 0);
   CHECK(options.pagePort != options.bridgePort);

   std::vector<std::vector<std::string>> plan;
   REQUIRE(sdl3mcp::makeWebLaunchPlan(options, plan, error));
   CHECK(plan.size() == 3);

   CHECK(sdl3mcp::removeProfileTree(options.profileDir, error));
   ::close(options.profileLockFd);
}
