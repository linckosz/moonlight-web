/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 */
#include "test_framework.h"
#include "server/HostAppLauncher.h"
#include "server/LauncherDetect.h"

#include <QHash>
#include <QSet>

namespace {

/// A machine described by hand: registry values, subkeys, files, variables.
struct FakeMachine
{
    QHash<QString, QString> registry; // "key|name" → value
    QHash<QString, QStringList> subkeys;
    QSet<QString> files;
    QHash<QString, QString> env;

    LauncherDetect::Lookup lookup() const
    {
        LauncherDetect::Lookup l;
        l.registry = [this](const QString& key, const QString& name) {
            return registry.value(key + QLatin1Char('|') + name);
        };
        l.subkeys = [this](const QString& key) { return subkeys.value(key); };
        l.exists = [this](const QString& path) { return files.contains(path); };
        l.env = [this](const QString& name) { return env.value(name); };
        return l;
    }
};

} // namespace

void run_launcher_detect_tests()
{
    SECTION("LauncherDetect");

    // The program of a protocol handler's command line.
    CHECK_EQ(LauncherDetect::programOf("\"C:\\Program Files (x86)\\Steam\\steam.exe\" -- \"%1\""),
             QString("C:\\Program Files (x86)\\Steam\\steam.exe"));
    CHECK_EQ(LauncherDetect::programOf("C:\\Program Files\\Playnite\\Playnite.DesktopApp.exe %1"),
             QString("C:\\Program Files\\Playnite\\Playnite.DesktopApp.exe"));

    // Steam on Windows: the user's own SteamExe first, written with forward
    // slashes as Steam writes it.
    {
        FakeMachine m;
        m.registry["HKCU\\Software\\Valve\\Steam|SteamExe"] =
            "c:/program files (x86)/steam/steam.exe";
        m.files << "c:\\program files (x86)\\steam\\steam.exe";
        const auto steam = LauncherDetect::steamWindows(m.lookup());
        CHECK(steam.has_value());
        CHECK_EQ(steam->source, QString("steam"));
        CHECK_EQ(steam->command, QString("steam://open/bigpicture"));
        CHECK_EQ(steam->quitCommand, QString("steam://close/bigpicture"));
        CHECK_EQ(steam->iconPath, QString("c:\\program files (x86)\\steam\\steam.exe"));
    }
    // Without HKCU (a server run as SYSTEM): the machine's InstallPath.
    {
        FakeMachine m;
        m.registry["HKLM\\SOFTWARE\\WOW6432Node\\Valve\\Steam|InstallPath"] = "D:\\Steam\\";
        m.files << "D:\\Steam\\steam.exe";
        const auto steam = LauncherDetect::steamWindows(m.lookup());
        CHECK(steam.has_value());
        CHECK_EQ(steam->iconPath, QString("D:\\Steam\\steam.exe"));
    }
    // A registry left behind by an uninstalled Steam is no Steam.
    {
        FakeMachine m;
        m.registry["HKCU\\Software\\Valve\\Steam|SteamExe"] = "c:/steam/steam.exe";
        CHECK(!LauncherDetect::steamWindows(m.lookup()).has_value());
    }

    // Playnite: its uninstall entry, then its default place.
    {
        FakeMachine m;
        const QString uninstall = "HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
        m.subkeys[uninstall] = QStringList{"Other", "{1234}"};
        m.registry[uninstall + "\\{1234}|DisplayName"] = "Playnite";
        m.registry[uninstall + "\\{1234}|InstallLocation"] = "E:\\Apps\\Playnite\\";
        m.files << "E:\\Apps\\Playnite\\Playnite.FullscreenApp.exe";
        const auto playnite = LauncherDetect::playniteWindows(m.lookup());
        CHECK(playnite.has_value());
        CHECK_EQ(playnite->command, QString("\"E:\\Apps\\Playnite\\Playnite.FullscreenApp.exe\""));
        CHECK(playnite->quitCommand.isEmpty());
    }
    {
        FakeMachine m;
        m.env["LOCALAPPDATA"] = "C:\\Users\\u\\AppData\\Local";
        m.files << "C:\\Users\\u\\AppData\\Local\\Playnite\\Playnite.FullscreenApp.exe";
        CHECK(LauncherDetect::playniteWindows(m.lookup()).has_value());
        m.files.clear();
        CHECK(!LauncherDetect::playniteWindows(m.lookup()).has_value());
    }

    // macOS and Linux.
    {
        FakeMachine m;
        m.env["HOME"] = "/Users/u";
        m.files << "/Users/u/Applications/Steam.app";
        const auto steam = LauncherDetect::steamMac(m.lookup());
        CHECK(steam.has_value());
        CHECK_EQ(steam->iconPath, QString("/Users/u/Applications/Steam.app"));
    }
    {
        FakeMachine m;
        m.env["HOME"] = "/home/u";
        m.files << "/usr/bin/steam" << "/usr/share/icons/hicolor/128x128/apps/steam.png"
                << "/usr/share/icons/hicolor/48x48/apps/steam.png";
        const auto steam = LauncherDetect::steamLinux(m.lookup());
        CHECK(steam.has_value());
        CHECK_EQ(steam->iconPath, QString("/usr/share/icons/hicolor/128x128/apps/steam.png"));
        FakeMachine none;
        CHECK(!LauncherDetect::steamLinux(none.lookup()).has_value());
    }
    // The Flatpak alone, installed for the user, with its icon where Flatpak exports it.
    {
        FakeMachine m;
        m.env["HOME"] = "/home/u";
        m.files << "/home/u/.local/share/flatpak/app/com.valvesoftware.Steam"
                << "/home/u/.local/share/flatpak/exports/share/icons/hicolor/128x128/apps/"
                   "com.valvesoftware.Steam.png";
        const auto steam = LauncherDetect::steamLinux(m.lookup());
        CHECK(steam.has_value());
        CHECK_EQ(steam->command, QString("steam://open/bigpicture"));
        CHECK_EQ(steam->iconPath,
                 QString("/home/u/.local/share/flatpak/exports/share/icons/hicolor/128x128/apps/"
                         "com.valvesoftware.Steam.png"));
    }

    // What AppSettings takes.
    {
        LauncherDetect::Launcher l{"steam", "Steam Big Picture", "steam://open/bigpicture",
                                   "steam://close/bigpicture", "x"};
        const QJsonObject json = LauncherDetect::toJson(l);
        CHECK_EQ(json.value("source").toString(), QString("steam"));
        CHECK_EQ(json.value("quit_command").toString(), QString("steam://close/bigpicture"));
        CHECK(!json.contains("icon"));
    }

    // A card's command: a URL, or a program and its arguments.
    CHECK(HostAppLauncher::isUrl("steam://open/bigpicture"));
    CHECK(HostAppLauncher::isUrl("  https://example.com/x "));
    CHECK(!HostAppLauncher::isUrl("C:\\Games\\game.exe"));
    CHECK(!HostAppLauncher::isUrl("lutris lutris:rungameid/3"));
    CHECK(!HostAppLauncher::isUrl("retroarch"));

    // Linux: a link's handler outlives xdg-open, which returns as soon as it
    // has started it; everything before "--" belongs to systemd-run.
    {
        const QStringList run{QString("xdg-open"), QString("steam://open/bigpicture")};
        const QStringList arguments = HostAppLauncher::systemdRunArguments(
            QString("moonlightweb-app-1"),
            {QString("DISPLAY=:0"), QString("XDG_CURRENT_DESKTOP=ubuntu:GNOME")}, run);
        const qsizetype split = arguments.indexOf(QString("--"));
        CHECK(split > 0);
        const QStringList own = arguments.mid(0, split);
        CHECK(own.contains(QString("--user")));
        CHECK(own.contains(QString("--unit=moonlightweb-app-1")));
        CHECK(own.contains(QString("--property=KillMode=process")));
        CHECK(own.contains(QString("--setenv=DISPLAY=:0")));
        CHECK(own.contains(QString("--setenv=XDG_CURRENT_DESKTOP=ubuntu:GNOME")));
        CHECK(arguments.mid(split + 1) == run);
    }
    {
        QString program, args;
        HostAppLauncher::splitWindowsCommand(
            "\"C:\\Program Files\\RetroArch\\retroarch.exe\" -f --menu", &program, &args);
        CHECK_EQ(program, QString("C:\\Program Files\\RetroArch\\retroarch.exe"));
        CHECK_EQ(args, QString("-f --menu"));
        HostAppLauncher::splitWindowsCommand("notepad.exe C:\\a.txt", &program, &args);
        CHECK_EQ(program, QString("notepad.exe"));
        CHECK_EQ(args, QString("C:\\a.txt"));
        HostAppLauncher::splitWindowsCommand("C:\\Games\\Shortcut.lnk", &program, &args);
        CHECK_EQ(program, QString("C:\\Games\\Shortcut.lnk"));
        CHECK(args.isEmpty());
    }

    // macOS: which of the app's windows go onto the virtual display. Steam
    // already running: its own window stays on the built-in screen, Big
    // Picture, opened next to it, is moved.
    {
        const QRect virtualDisplay(0, 0, 800, 600);
        const QRect builtIn(-1512, 0, 1512, 982);
        const HostAppLauncher::SeenWindow client{11, QRect(-1400, 60, 1000, 700)};
        const HostAppLauncher::SeenWindow bigPicture{42, builtIn};
        const HostAppLauncher::SeenWindow tooltip{43, QRect(-900, 300, 80, 20)};
        const HostAppLauncher::SeenWindow alreadyThere{44, QRect(0, 0, 800, 600)};
        const auto move = HostAppLauncher::windowsToMove(
            {11}, {}, {client, bigPicture, tooltip, alreadyThere}, virtualDisplay);
        CHECK_EQ(move.size(), qsizetype(1));
        CHECK_EQ(move.value(0).number, 42);
        // Moved once: never again, should its owner drag it back.
        CHECK(HostAppLauncher::windowsToMove({11}, {42}, {client, bigPicture}, virtualDisplay)
                  .isEmpty());
        // A cold start: every window is new, the client's too.
        CHECK_EQ(
            HostAppLauncher::windowsToMove({}, {}, {client, bigPicture}, virtualDisplay).size(),
            qsizetype(2));
    }

    // Linux: where a moved window lands, and the request that puts it there.
    {
        const QRect virtualDisplay(0, 0, 800, 600);
        CHECK(HostAppLauncher::placedOn(QSize(1920, 1080), virtualDisplay) == virtualDisplay);
        CHECK(HostAppLauncher::placedOn(QSize(446, 225), QRect(2560, 0, 800, 600)) ==
              QRect(2560, 0, 446, 225));
        const auto values = HostAppLauncher::moveResizeValues(QRect(10, 20, 800, 600));
        CHECK_EQ(values[0] & 0xFF, 1L);         // north-west gravity
        CHECK_EQ((values[0] >> 8) & 0xF, 0xFL); // x, y, width and height are all set
        CHECK_EQ((values[0] >> 12) & 0xF, 2L);  // from a pager
        CHECK_EQ(values[1], 10L);
        CHECK_EQ(values[2], 20L);
        CHECK_EQ(values[3], 800L);
        CHECK_EQ(values[4], 600L);
    }

    // Linux: Big Picture in full screen on another screen leaves it, moves, and
    // goes back to it; a window that never moves is given up on.
    {
        using HostAppLauncher::nextWindowStep;
        using HostAppLauncher::WindowStep;
        HostAppLauncher::WindowMove move;
        CHECK(nextWindowStep(move, true, false, 8) == WindowStep::LeaveFullScreen);
        move.wasFullScreen = true;
        move.tries = 1;
        CHECK(nextWindowStep(move, true, false, 8) == WindowStep::LeaveFullScreen);
        CHECK(nextWindowStep(move, false, false, 8) == WindowStep::Move);
        CHECK(nextWindowStep(move, false, true, 8) == WindowStep::EnterFullScreen);
        CHECK(nextWindowStep(move, true, true, 8) == WindowStep::Done);
        move.tries = 8;
        CHECK(nextWindowStep(move, false, false, 8) == WindowStep::GiveUp);
        CHECK(nextWindowStep(move, false, true, 8) == WindowStep::EnterFullScreen);
        HostAppLauncher::WindowMove windowed;
        CHECK(nextWindowStep(windowed, false, false, 8) == WindowStep::Move);
        CHECK(nextWindowStep(windowed, false, true, 8) == WindowStep::Done);
    }
}
