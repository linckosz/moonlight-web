/*
 * MoonlightWeb — native capture & encoding engine.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "GamescopeSession.h"

#include "../../core/Log.h"

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

extern char** environ;

namespace mw::native::capture {
namespace {

using Clock = std::chrono::steady_clock;

std::string runtimeDir()
{
    const char* dir = std::getenv("XDG_RUNTIME_DIR");
    if (dir && *dir) return dir;
    return "/run/user/" + std::to_string(::getuid());
}

/// The user's home, where Steam and its library are. A process the package's
/// launcher starts (the stream worker, the probe) runs with HOME fixed, and
/// finds the user's under MW_USER_HOME instead (moonlightweb-launch-env.h).
std::string homeDir()
{
    const char* user = std::getenv("MW_USER_HOME");
    if (user && *user == '/') return user;
    const char* home = std::getenv("HOME");
    return home && *home ? std::string(home) : std::string();
}

std::string readFile(const std::string& path, size_t limit = 1 << 20)
{
    std::string text;
    if (FILE* f = std::fopen(path.c_str(), "re")) {
        char buffer[4096];
        size_t n = 0;
        while (text.size() < limit && (n = std::fread(buffer, 1, sizeof(buffer), f)) > 0)
            text.append(buffer, n);
        std::fclose(f);
    }
    return text;
}

bool isExecutable(const std::string& path)
{
    return !path.empty() && ::access(path.c_str(), X_OK) == 0;
}

bool exists(const std::string& path)
{
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0;
}

int64_t mtimeOf(const std::string& path)
{
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) return 0;
    return static_cast<int64_t>(st.st_mtime);
}

std::string realPath(const std::string& path)
{
    char resolved[PATH_MAX];
    return ::realpath(path.c_str(), resolved) ? std::string(resolved) : std::string();
}

std::string dirOf(const std::string& path)
{
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

/// Run @p argv and wait for it, @p timeoutMs at most (then it is killed).
/// Its output, both streams, in @p output when asked for; @p home, when
/// given, replaces HOME in its environment. The exit code, or -1.
int run(const std::vector<std::string>& argv, std::string* output, int timeoutMs,
        const std::string& home = std::string())
{
    if (argv.empty()) return -1;
    int fds[2] = {-1, -1};
    if (output && ::pipe2(fds, O_CLOEXEC) != 0) return -1;

    // Nothing of ours goes along: the capture's descriptors, the server's
    // sockets — the child holds its pipe and the standard streams only.
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    if (output) {
        posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&actions, fds[1], STDERR_FILENO);
    } else {
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    }
    posix_spawn_file_actions_addclosefrom_np(&actions, STDERR_FILENO + 1);

    std::vector<char*> args;
    for (const std::string& a : argv)
        args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);

    std::vector<std::string> envStrings;
    std::vector<char*> envp;
    char** env = environ;
    if (!home.empty()) {
        for (char** e = environ; e && *e; ++e)
            if (std::strncmp(*e, "HOME=", 5) != 0) envStrings.emplace_back(*e);
        envStrings.push_back("HOME=" + home);
        for (std::string& s : envStrings)
            envp.push_back(s.data());
        envp.push_back(nullptr);
        env = envp.data();
    }

    pid_t child = -1;
    const int rc = ::posix_spawnp(&child, args[0], &actions, nullptr, args.data(), env);
    posix_spawn_file_actions_destroy(&actions);
    if (output) ::close(fds[1]);
    if (rc != 0) {
        if (output) {
            ::close(fds[0]);
            *output = argv[0] + ": " + std::strerror(rc);
        }
        return -1;
    }

    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    bool open = output != nullptr;
    while (open) {
        const auto left =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (left <= 0) break;
        pollfd p{fds[0], POLLIN, 0};
        const int ready = ::poll(&p, 1, static_cast<int>(left));
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) break;
        char buffer[4096];
        const ssize_t n = ::read(fds[0], buffer, sizeof(buffer));
        if (n <= 0) {
            open = false;
            break;
        }
        if (output->size() < (1 << 16)) output->append(buffer, static_cast<size_t>(n));
    }
    if (output) ::close(fds[0]);

    int status = 0;
    for (;;) {
        const pid_t done = ::waitpid(child, &status, WNOHANG);
        if (done == child) break;
        if (done < 0 && errno != EINTR) return -1;
        if (Clock::now() >= deadline) {
            ::kill(child, SIGKILL);
            ::waitpid(child, &status, 0);
            return -1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/// A process of this user, as /proc shows it.
struct UserProcess
{
    int pid = 0;
    std::string argv0;
    std::string cmdline; ///< arguments joined by spaces
};

std::vector<UserProcess> userProcesses()
{
    std::vector<UserProcess> out;
    DIR* proc = ::opendir("/proc");
    if (!proc) return out;
    const uid_t me = ::getuid();
    while (dirent* entry = ::readdir(proc)) {
        const char* name = entry->d_name;
        if (*name < '0' || *name > '9') continue;
        const std::string dir = std::string("/proc/") + name;
        struct stat st{};
        if (::stat(dir.c_str(), &st) != 0 || st.st_uid != me) continue;
        UserProcess p;
        p.pid = std::atoi(name);
        std::string raw = readFile(dir + "/cmdline", 1 << 14);
        if (raw.empty()) continue;
        p.argv0 = raw.substr(0, raw.find('\0'));
        for (char& c : raw)
            if (c == '\0') c = ' ';
        p.cmdline = std::move(raw);
        out.push_back(std::move(p));
    }
    ::closedir(proc);
    return out;
}

std::string environValue(const std::string& environ, const std::string& name)
{
    const std::string key = name + "=";
    size_t at = 0;
    while (at < environ.size()) {
        const size_t end = environ.find('\0', at);
        const std::string entry =
            environ.substr(at, end == std::string::npos ? std::string::npos : end - at);
        if (entry.rfind(key, 0) == 0) return entry.substr(key.size());
        if (end == std::string::npos) break;
        at = end + 1;
    }
    return std::string();
}

bool alive(int pid)
{
    return pid > 0 && (::kill(pid, 0) == 0 || errno == EPERM);
}

std::string recordPath(const std::string& card)
{
    return runtimeDir() + "/moonlightweb-gamescope-" + card;
}

std::string unitFor(const std::string& card)
{
    return "moonlightweb-gamescope-" + card;
}

bool unitActive(const std::string& unit)
{
    return run({"systemctl", "--user", "is-active", "--quiet", unit + ".service"}, nullptr, 3000) ==
           0;
}

int mainPidOf(const std::string& unit)
{
    std::string said;
    if (run({"systemctl", "--user", "show", "--property=MainPID", "--value", unit + ".service"},
            &said, 3000) != 0)
        return 0;
    return std::atoi(said.c_str());
}

/// The last lines of gamescope's own log, for a refusal the viewer reads.
std::string logTail(const std::string& path)
{
    const std::string text = readFile(path);
    std::vector<std::string> lines;
    size_t at = 0;
    while (at < text.size()) {
        const size_t end = text.find('\n', at);
        std::string line = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
        // xkbcomp's warnings fill the log and say nothing about a failure.
        if (!line.empty() && line.find("keysym") == std::string::npos &&
            line.find("xkbcomp") == std::string::npos && line[0] != '>')
            lines.push_back(line);
        if (end == std::string::npos) break;
        at = end + 1;
    }
    std::string tail;
    for (size_t i = lines.size() > 3 ? lines.size() - 3 : 0; i < lines.size(); ++i)
        tail += (tail.empty() ? "" : " | ") + lines[i];
    return tail;
}

/// The one lock a session is started or found under: Steam moves between the
/// desktop and a session, and two streams doing it at once would each find it
/// still open, or start it twice.
class SessionLock
{
public:
    ~SessionLock()
    {
        if (m_Fd >= 0) {
            ::flock(m_Fd, LOCK_UN);
            ::close(m_Fd);
        }
    }
    bool take(int waitMs, std::string& why)
    {
        const std::string path = runtimeDir() + "/moonlightweb-gamescope.lock";
        m_Fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (m_Fd < 0) {
            why = path + ": " + std::strerror(errno);
            return false;
        }
        for (int waited = 0;; waited += 50) {
            if (::flock(m_Fd, LOCK_EX | LOCK_NB) == 0) return true;
            if (errno != EWOULDBLOCK && errno != EINTR) break;
            if (waited >= waitMs) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        why = "another stream is starting the same session";
        ::close(m_Fd);
        m_Fd = -1;
        return false;
    }

private:
    int m_Fd = -1;
};

bool writeRecord(const std::string& card, const GamescopeRecord& record)
{
    const std::string path = recordPath(card);
    const std::string aside = path + "." + std::to_string(::getpid());
    const std::string text = formatGamescopeRecord(record);
    const int fd = ::open(aside.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    const bool written = ::write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size());
    ::close(fd);
    if (!written || ::rename(aside.c_str(), path.c_str()) != 0) {
        ::unlink(aside.c_str());
        return false;
    }
    return true;
}

/// A game a Steam started: refused while one runs, the desktop's player
/// would lose it.
bool steamGameRunning()
{
    for (const UserProcess& p : userProcesses())
        if (isSteamGame(p.cmdline)) return true;
    return false;
}

std::string steamRemoteFor(const SteamInstall& steam)
{
    for (const std::string& candidate : steamRemoteCandidates(steam))
        if (isExecutable(candidate)) return candidate;
    return std::string();
}

/// Ask the Steam of @p steam, running as @p pid, to quit, and wait for it
/// @p waitMs at most. Through its own pipe first, then its launcher.
bool askSteamToQuit(const SteamInstall& steam, int pid, int waitMs)
{
    const std::string remote = steamRemoteFor(steam);
    std::string said;
    if (!remote.empty()) {
        run({remote, "-shutdown"}, &said, 5000, steam.home);
    } else {
        std::vector<std::string> command = steam.launcher;
        command.push_back("-shutdown");
        run(command, &said, 15000);
    }
    const auto deadline = Clock::now() + std::chrono::milliseconds(waitMs);
    while (Clock::now() < deadline) {
        if (!alive(pid)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    return !alive(pid);
}

} // namespace

bool findGamescope(GamescopeBinary& out, std::string& why)
{
    std::vector<std::string> candidates;
    if (const char* forced = std::getenv("MW_GAMESCOPE_BIN"); forced && *forced)
        candidates.emplace_back(forced);
    if (const char* path = std::getenv("PATH"); path && *path) {
        std::string dirs = path;
        size_t at = 0;
        while (at <= dirs.size()) {
            const size_t end = dirs.find(':', at);
            const std::string dir = dirs.substr(at, end == std::string::npos ? end : end - at);
            if (!dir.empty()) candidates.push_back(dir + "/gamescope");
            if (end == std::string::npos) break;
            at = end + 1;
        }
    }
    // A build of the user's own, where a service's PATH does not look.
    if (const std::string home = homeDir(); !home.empty())
        candidates.push_back(home + "/.local/bin/gamescope");
    candidates.emplace_back("/usr/games/gamescope");
    candidates.emplace_back("/usr/bin/gamescope");

    // --version is asked once per binary and modification: the probe runs at
    // every look at the host list.
    static std::mutex cacheMutex;
    static std::map<std::string, std::pair<int64_t, GamescopeVersion>> cache;

    // The first usable one in that order, not the newest: MW_GAMESCOPE_BIN
    // and PATH say which one the user meant.
    std::string tooOld;
    for (const std::string& candidate : candidates) {
        if (!isExecutable(candidate)) continue;
        const std::string path = realPath(candidate);
        if (path.empty()) continue;
        const int64_t mtime = mtimeOf(path);
        GamescopeVersion version;
        bool known = false;
        {
            std::lock_guard<std::mutex> lock(cacheMutex);
            const auto it = cache.find(path);
            if (it != cache.end() && it->second.first == mtime) {
                version = it->second.second;
                known = true;
            }
        }
        if (!known) {
            std::string banner;
            run({path, "--version"}, &banner, 3000);
            if (!parseGamescopeVersion(banner, version)) continue;
            std::lock_guard<std::mutex> lock(cacheMutex);
            cache[path] = {mtime, version};
        }
        if (!version.atLeast(kGamescopeMinimum)) {
            tooOld = path + " is " + version.text();
            continue;
        }
        out.path = path;
        out.version = version;
        return true;
    }
    why = tooOld.empty()
              ? "no gamescope is installed"
              : "gamescope " + kGamescopeMinimum.text() + " or later is needed (" + tooOld + ")";
    return false;
}

std::vector<SteamInstall> findSteamInstalls()
{
    std::vector<SteamInstall> installs;
    const std::string home = homeDir();
    if (home.empty()) return installs;
    const auto signedIn = [](const std::string& dataDir) {
        return mtimeOf(dataDir + "/config/loginusers.vdf");
    };

    // Valve's package (Debian's under /usr/games) — unless the name is the
    // snap's own alias.
    for (const char* launcher : {"/usr/bin/steam", "/usr/games/steam"}) {
        if (!isExecutable(launcher)) continue;
        const std::string target = realPath(launcher);
        if (target.find("/snap") != std::string::npos) continue;
        SteamInstall steam;
        steam.packaging = SteamPackaging::Native;
        steam.home = home;
        steam.launcher = {launcher};
        const std::string root = realPath(home + "/.steam/root");
        steam.lastUsed = signedIn(root.empty() ? home + "/.local/share/Steam" : root);
        installs.push_back(steam);
        break;
    }
    if (isExecutable("/snap/bin/steam")) {
        SteamInstall steam;
        steam.packaging = SteamPackaging::Snap;
        steam.home = home + "/snap/steam/common";
        steam.launcher = {"/snap/bin/steam"};
        steam.lastUsed = signedIn(steam.home + "/.local/share/Steam");
        installs.push_back(steam);
    }
    const std::string flatpakHome = home + "/.var/app/com.valvesoftware.Steam";
    if (exists(flatpakHome)) {
        for (const char* flatpak : {"/usr/bin/flatpak", "/usr/local/bin/flatpak"}) {
            if (!isExecutable(flatpak)) continue;
            SteamInstall steam;
            steam.packaging = SteamPackaging::Flatpak;
            steam.home = flatpakHome;
            steam.launcher = {flatpak, "run", "com.valvesoftware.Steam"};
            steam.lastUsed = signedIn(flatpakHome + "/.local/share/Steam");
            installs.push_back(steam);
            break;
        }
    }
    return installs;
}

std::string runningDesktopSteam(int& pid)
{
    pid = 0;
    for (UserProcess& p : userProcesses()) {
        if (!isSteamClient(p.argv0)) continue;
        const std::string env = readFile("/proc/" + std::to_string(p.pid) + "/environ", 1 << 18);
        // One in a gamescope of ours is not the desktop's.
        if (!environValue(env, "GAMESCOPE_WAYLAND_DISPLAY").empty()) continue;
        pid = p.pid;
        return environValue(env, "HOME");
    }
    return std::string();
}

bool openGamescopeSession(const GamescopeApp& app, int width, int height, int refreshHz,
                          GamescopeSession& out, std::string& why)
{
    out = GamescopeSession{};
    // Long enough for another stream moving Steam: it waits up to 20 s for
    // Steam to quit, then for gamescope's node.
    SessionLock lock;
    if (!lock.take(40000, why)) return false;

    const std::string unit = unitFor(app.card);
    GamescopeRecord found;
    if (parseGamescopeRecord(readFile(recordPath(app.card)), found) && found.unit == unit &&
        unitActive(unit)) {
        out.record = found;
        out.pid = mainPidOf(unit);
        log::info("[native] gamescope: the " + app.card + " session runs — " +
                  std::to_string(found.width) + "x" + std::to_string(found.height) + " at " +
                  std::to_string(found.refreshHz) + " Hz, node " + std::to_string(found.node));
        return true;
    }

    GamescopeBinary binary;
    if (!findGamescope(binary, why)) return false;

    GamescopeLaunch launch;
    launch.unit = unit;
    launch.binary = binary.path;
    // gamescope starts its app through gamescopereaper, found on PATH.
    const char* path = std::getenv("PATH");
    launch.path =
        dirOf(binary.path) + ":" + (path && *path ? path : "/usr/local/bin:/usr/bin:/bin");
    launch.width = width;
    launch.height = height;
    launch.refreshHz = refreshHz;
    launch.infoPath = recordPath(app.card) + ".app";
    launch.logPath = recordPath(app.card) + ".log";

    if (app.steam) {
        const std::vector<SteamInstall> installs = findSteamInstalls();
        int steamPid = 0;
        const std::string runningHome = runningDesktopSteam(steamPid);
        const int chosen = chooseSteam(installs, runningHome);
        if (chosen < 0) {
            why = installs.empty() ? "Steam is not installed on the host"
                                   : "the host has several Steam installs and none was ever "
                                     "signed in to";
            return false;
        }
        const SteamInstall& steam = installs[static_cast<size_t>(chosen)];
        launch.steam = true;
        launch.app = steamBigPictureCommand(steam);
        launch.stopSteamHome = steam.home;
        launch.stopSteamRemote = steamRemoteFor(steam);
        if (!runningHome.empty()) {
            if (steamGameRunning()) {
                why = "a game is running from Steam on the host's desktop — quit it there first";
                return false;
            }
            log::info("[native] gamescope: Steam (" + std::string(toString(steam.packaging)) +
                      ") is open on the desktop — asking it to quit, it comes back after");
            // 20 s: the browser gives up on a start at 25, and a refusal it
            // reads beats a time-out.
            if (!askSteamToQuit(steam, steamPid, 20000)) {
                why = "Steam is open on the host's desktop and did not quit when asked — close it "
                      "there, then start again";
                return false;
            }
            launch.steamBack = steam.launcher;
        }
    } else {
        launch.app = app.command;
    }
    if (launch.app.empty()) {
        why = "nothing to run in gamescope";
        return false;
    }

    // A session that left its pieces behind: a unit failed, a timer pending.
    run({"systemctl", "--user", "stop", unit + "-linger.timer", unit + "-linger.service",
         unit + ".service"},
        nullptr, 50000);
    run({"systemctl", "--user", "reset-failed", unit + ".service"}, nullptr, 3000);
    ::unlink(launch.infoPath.c_str());
    ::unlink(launch.logPath.c_str());
    ::unlink(recordPath(app.card).c_str());

    std::string said;
    if (run(gamescopeUnitCommand(launch), &said, 15000) != 0) {
        why = "systemd could not start gamescope: " + said;
        return false;
    }

    // The node first — gamescope makes it before Xwayland — then what the
    // wrapper wrote, once Xwayland took its app.
    GamescopeRecord record;
    record.unit = unit;
    record.width = width;
    record.height = height;
    record.refreshHz = refreshHz;
    const auto deadline = Clock::now() + std::chrono::seconds(15);
    while (Clock::now() < deadline) {
        if (record.node == 0) {
            const std::string log = readFile(launch.logPath);
            size_t at = 0;
            while (at < log.size()) {
                const size_t end = log.find('\n', at);
                uint32_t node = 0;
                if (parseGamescopeNodeLine(
                        log.substr(at, end == std::string::npos ? end : end - at), node))
                    record.node = node;
                if (end == std::string::npos) break;
                at = end + 1;
            }
        }
        parseGamescopeEndpoints(readFile(launch.infoPath), record.endpoints);
        if (record.valid()) break;
        if (!unitActive(unit)) {
            why = "gamescope stopped as it started: " + logTail(launch.logPath);
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!record.valid()) {
        why = "gamescope gave no picture in 15 s: " + logTail(launch.logPath);
        run({"systemctl", "--user", "stop", unit + ".service"}, nullptr, 50000);
        return false;
    }
    writeRecord(app.card, record);
    out.record = record;
    out.started = true;
    out.pid = mainPidOf(unit);
    log::info("[native] gamescope " + binary.version.text() + ": " + app.card + " session at " +
              std::to_string(width) + "x" + std::to_string(height) + " " +
              std::to_string(refreshHz) + " Hz — node " + std::to_string(record.node) + ", " +
              record.endpoints.xDisplay + ", " + record.endpoints.eisSocket);
    return true;
}

bool gamescopeSessionAlive(const GamescopeRecord& record)
{
    return unitActive(record.unit);
}

void keepGamescopeSession(const GamescopeRecord& record, int lingerSeconds)
{
    run({"systemctl", "--user", "stop", record.unit + "-linger.timer",
         record.unit + "-linger.service"},
        nullptr, 3000);
    run(gamescopeLingerCommand(record.unit, lingerSeconds), nullptr, 3000);
}

std::string gamescopeSocketPath(const std::string& eisSocket)
{
    if (!eisSocket.empty() && eisSocket[0] == '/') return eisSocket;
    return runtimeDir() + "/" + eisSocket;
}

} // namespace mw::native::capture
