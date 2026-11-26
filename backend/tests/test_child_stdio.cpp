/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * A child of the server receives only its standard channels
 * (common/ChildStdio.h). This process opens witnesses that a child could
 * inherit: a file, a UDP socket, a listening TCP socket, all made inheritable
 * on purpose, and a QUdpSocket. A second process of this test (--child-stdio-probe)
 * then says which of them it holds, each one named by what it is (the file's
 * name or its device and inode, the socket's port), never by its number
 * alone. The same start without passOnlyStdio is the control: it must find
 * the inheritable ones.
 */
#include "test_framework.h"

#include "common/ChildStdio.h"
#include "common/DetachedProcess.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QProcess>
#include <QThread>
#include <QUdpSocket>
#include <QUuid>

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#ifdef Q_OS_WIN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#else
#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "streaming/IceMuxHandles.h"
#endif

namespace {

// ── The second process ───────────────────────────────────────────────────────

#ifndef Q_OS_WIN
/// Its open descriptors, read before it does anything else.
std::vector<int> openDescriptors()
{
    std::vector<int> open;
#ifdef __linux__
    DIR* dir = opendir("/proc/self/fd");
    if (!dir) return open;
    const int own = dirfd(dir);
    while (const dirent* entry = readdir(dir)) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
        const int fd = std::atoi(entry->d_name);
        if (fd != own) open.push_back(fd);
    }
    closedir(dir);
#else
    for (int fd = 0; fd < 1024; ++fd)
        if (fcntl(fd, F_GETFD) != -1) open.push_back(fd);
#endif
    return open;
}

uint16_t portOf(int fd)
{
    sockaddr_storage bound{};
    socklen_t length = sizeof(bound);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &length) != 0) return 0;
    if (bound.ss_family == AF_INET) return ntohs(reinterpret_cast<sockaddr_in*>(&bound)->sin_port);
    if (bound.ss_family == AF_INET6)
        return ntohs(reinterpret_cast<sockaddr_in6*>(&bound)->sin6_port);
    return 0;
}
#endif

/// Whether this process holds the witness @p spec:
///   Windows  f:<handle>:<file name>   s:<handle>:<port>
///   Unix     f:<device>:<inode>       s:<port>   k:<port> (one it must hold)
bool holds(const std::string& spec, const std::vector<int>& open)
{
#ifdef Q_OS_WIN
    Q_UNUSED(open);
    const size_t second = spec.find(':', 2);
    if (spec.size() < 4 || second == std::string::npos) return false;
    const auto value = static_cast<uintptr_t>(std::strtoull(spec.c_str() + 2, nullptr, 10));
    const std::string rest = spec.substr(second + 1);
    if (spec[0] == 'f') {
        wchar_t path[1024];
        const DWORD n = GetFinalPathNameByHandleW(reinterpret_cast<HANDLE>(value), path, 1024,
                                                  FILE_NAME_NORMALIZED);
        if (n == 0 || n >= 1024) return false;
        const std::wstring got(path, n);
        const std::wstring name(rest.begin(), rest.end());
        return got.size() >= name.size() &&
               got.compare(got.size() - name.size(), name.size(), name) == 0;
    }
    if (spec[0] == 's') {
        sockaddr_storage bound{};
        int length = sizeof(bound);
        if (getsockname(static_cast<SOCKET>(value), reinterpret_cast<sockaddr*>(&bound), &length) !=
            0)
            return false;
        const auto port = static_cast<uint16_t>(std::strtoul(rest.c_str(), nullptr, 10));
        return bound.ss_family == AF_INET &&
               ntohs(reinterpret_cast<sockaddr_in*>(&bound)->sin_port) == port;
    }
    return false;
#else
    if (spec.size() < 3) return false;
    if (spec[0] == 'f') {
        const size_t second = spec.find(':', 2);
        if (second == std::string::npos) return false;
        const auto device = std::strtoull(spec.c_str() + 2, nullptr, 10);
        const auto inode = std::strtoull(spec.c_str() + second + 1, nullptr, 10);
        for (const int fd : open) {
            struct stat st{};
            if (fstat(fd, &st) == 0 && static_cast<unsigned long long>(st.st_dev) == device &&
                static_cast<unsigned long long>(st.st_ino) == inode)
                return true;
        }
        return false;
    }
    if (spec[0] == 's' || spec[0] == 'k') {
        const auto port = static_cast<uint16_t>(std::strtoul(spec.c_str() + 2, nullptr, 10));
        for (const int fd : open)
            if (portOf(fd) == port) return true;
        return false;
    }
    return false;
#endif
}

// ── This process ─────────────────────────────────────────────────────────────

/// A socket bound on the loopback that a child could inherit.
struct Witness
{
#ifdef Q_OS_WIN
    SOCKET socket = INVALID_SOCKET;
#else
    int socket = -1;
#endif
    uint16_t port = 0;

    explicit Witness(int type)
    {
#ifdef Q_OS_WIN
        socket = WSASocketW(AF_INET, type, type == SOCK_DGRAM ? IPPROTO_UDP : IPPROTO_TCP, nullptr,
                            0, WSA_FLAG_OVERLAPPED);
        if (socket == INVALID_SOCKET) return;
        SetHandleInformation(reinterpret_cast<HANDLE>(socket), HANDLE_FLAG_INHERIT,
                             HANDLE_FLAG_INHERIT);
        int length = sizeof(sockaddr_in);
#else
        socket = ::socket(AF_INET, type, 0);
        if (socket < 0) return;
        fcntl(socket, F_SETFD, 0);
        socklen_t length = sizeof(sockaddr_in);
#endif
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) return;
        if (type == SOCK_STREAM) listen(socket, 1);
        if (getsockname(socket, reinterpret_cast<sockaddr*>(&address), &length) == 0)
            port = ntohs(address.sin_port);
    }
    ~Witness()
    {
#ifdef Q_OS_WIN
        if (socket != INVALID_SOCKET) closesocket(socket);
#else
        if (socket >= 0) ::close(socket);
#endif
    }
    Witness(const Witness&) = delete;
    Witness& operator=(const Witness&) = delete;

    QString spec() const
    {
#ifdef Q_OS_WIN
        return QStringLiteral("s:%1:%2").arg(static_cast<quint64>(socket)).arg(port);
#else
        return QStringLiteral("s:%1").arg(port);
#endif
    }
};

/// A file, open, that a child could inherit.
struct FileWitness
{
    QString path;
#ifdef Q_OS_WIN
    HANDLE handle = INVALID_HANDLE_VALUE;
#else
    int fd = -1;
#endif

    FileWitness()
    {
        const QString name =
            QStringLiteral("mw-child-stdio-%1.tmp").arg(QUuid::createUuid().toString(QUuid::Id128));
        path = QDir::temp().filePath(name);
#ifdef Q_OS_WIN
        SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
        handle = CreateFileW(
            reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(path).utf16()),
            GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            &inherit, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
#else
        fd = ::open(QFile::encodeName(path).constData(), O_RDWR | O_CREAT | O_TRUNC, 0600);
#endif
    }
    ~FileWitness()
    {
#ifdef Q_OS_WIN
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
#else
        if (fd >= 0) ::close(fd);
        QFile::remove(path);
#endif
    }
    FileWitness(const FileWitness&) = delete;
    FileWitness& operator=(const FileWitness&) = delete;

    bool isOpen() const
    {
#ifdef Q_OS_WIN
        return handle != INVALID_HANDLE_VALUE;
#else
        return fd >= 0;
#endif
    }
    QString spec() const
    {
#ifdef Q_OS_WIN
        return QStringLiteral("f:%1:%2")
            .arg(reinterpret_cast<quintptr>(handle))
            .arg(QFileInfo(path).fileName());
#else
        struct stat st{};
        fstat(fd, &st);
        return QStringLiteral("f:%1:%2")
            .arg(static_cast<quint64>(st.st_dev))
            .arg(static_cast<quint64>(st.st_ino));
#endif
    }
};

/// What the second process said.
struct Answer
{
    bool started = false;
    bool pong = false;    // its standard channels work
    QStringList found;    // the witnesses it holds
    QStringList absent;   // those it does not
    std::vector<int> fds; // Unix: its open descriptors at its start
};

Answer parse(const QByteArray& out)
{
    Answer a;
    a.started = true;
    for (const QByteArray& raw : out.split('\n')) {
        const QString line = QString::fromUtf8(raw).trimmed();
        if (line == QLatin1String("pong ping"))
            a.pong = true;
        else if (line.startsWith(QLatin1String("found ")))
            a.found << line.mid(6);
        else if (line.startsWith(QLatin1String("absent ")))
            a.absent << line.mid(7);
        else if (line.startsWith(QLatin1String("fds")))
            for (const QString& n : line.mid(3).split(QLatin1Char(' '), Qt::SkipEmptyParts))
                a.fds.push_back(n.toInt());
    }
    std::sort(a.fds.begin(), a.fds.end());
    return a;
}

enum class Way
{
    Plain,
    OnlyStdio
};

/// The second process started with its three pipes, as the server starts one.
Answer probe(Way way, const QStringList& specs, const std::vector<int>& kept = {})
{
    QProcess process;
    process.setProgram(QCoreApplication::applicationFilePath());
    process.setArguments(QStringList{QStringLiteral("--child-stdio-probe"), QStringLiteral("-")} +
                         specs);
    if (way == Way::OnlyStdio) {
#ifdef Q_OS_WIN
        Q_UNUSED(kept);
        mw::passOnlyStdio(process);
#else
        mw::passOnlyStdio(process, kept, [kept]() {
            mw::icemuxhandles::leaveOpenAcrossExec(kept.data(), kept.size());
        });
#endif
    }
    process.start();
    if (!process.waitForStarted(10000)) return {};
    process.write("ping\n");
    process.closeWriteChannel();
    if (!process.waitForFinished(20000)) {
        process.kill();
        process.waitForFinished(2000);
        return {};
    }
    return parse(process.readAllStandardOutput());
}

QStringList witnessSpecs(const FileWitness& file, const Witness& udp, const Witness& tcp)
{
    return {file.spec(), udp.spec(), tcp.spec()};
}

void onlyItsChannels()
{
    SECTION("ChildStdio: a child started through passOnlyStdio gets its pipes and no witness");
    FileWitness file;
    Witness udp(SOCK_DGRAM);
    Witness tcp(SOCK_STREAM);
    QUdpSocket qtSocket;
    CHECK(qtSocket.bind(QHostAddress::LocalHost, 0));
    CHECK(file.isOpen());
    CHECK(udp.port != 0);
    CHECK(tcp.port != 0);
    QStringList specs = witnessSpecs(file, udp, tcp);
#ifdef Q_OS_WIN
    const QString qtSpec = QStringLiteral("s:%1:%2")
                               .arg(static_cast<quint64>(qtSocket.socketDescriptor()))
                               .arg(qtSocket.localPort());
#else
    const QString qtSpec = QStringLiteral("s:%1").arg(qtSocket.localPort());
#endif

    // The control: started as QProcess does by itself, the child holds them,
    // so the probe sees what it is asked to see
    const Answer control = probe(Way::Plain, specs);
    CHECK(control.started);
    CHECK(control.pong);
    CHECK(control.found.size() == 3);

    const Answer only = probe(Way::OnlyStdio, specs + QStringList{qtSpec});
    CHECK(only.started);
    CHECK(only.pong);
    CHECK(only.found.isEmpty());
    CHECK(only.absent.size() == 4);
    for (const QString& f : only.found)
        std::fprintf(stderr, "  the child holds %s\n", qPrintable(f));

    SECTION("ChildStdio: one QProcess started twice through it, both starts the same");
    {
        QProcess process;
        process.setProgram(QCoreApplication::applicationFilePath());
        process.setArguments(
            QStringList{QStringLiteral("--child-stdio-probe"), QStringLiteral("-")} + specs);
        mw::passOnlyStdio(process);
        mw::passOnlyStdio(process); // the second call changes nothing
        for (int round = 0; round < 2; ++round) {
            process.start();
            CHECK(process.waitForStarted(10000));
            process.write("ping\n");
            process.closeWriteChannel();
            CHECK(process.waitForFinished(20000));
            const Answer a = parse(process.readAllStandardOutput());
            CHECK(a.pong);
            CHECK(a.found.isEmpty());
        }
    }
}

#ifndef Q_OS_WIN
void theDescriptorsItIsMeantToGet()
{
    SECTION("ChildStdio: the descriptors left open for a child reach it, and nothing else");
    FileWitness file;
    Witness udp(SOCK_DGRAM);
    Witness tcp(SOCK_STREAM);
    // One it is meant to get, as a stream worker gets the shared ports: closed
    // at exec in this process, as every socket of the mux is
    Witness shared(SOCK_DGRAM);
    fcntl(shared.socket, F_SETFD, FD_CLOEXEC);
    const QStringList specs =
        witnessSpecs(file, udp, tcp) + QStringList{QStringLiteral("k:%1").arg(shared.port)};

    const Answer a = probe(Way::OnlyStdio, specs, {shared.socket});
    CHECK(a.started);
    CHECK(a.pong);
    CHECK(a.found == QStringList{QStringLiteral("k:%1").arg(shared.port)});
    // Still closed at exec here: only the child's copy was let through
    const int flags = fcntl(shared.socket, F_GETFD);
    CHECK(flags != -1 && (flags & FD_CLOEXEC));
#ifdef __linux__
    // Read by the child before anything else: its three channels and the one
    // it was meant to get, exactly
    const std::vector<int> expected{0, 1, 2, shared.socket};
    CHECK(a.fds == expected);
    if (a.fds != expected) {
        std::fprintf(stderr, "  the child had:");
        for (const int fd : a.fds)
            std::fprintf(stderr, " %d", fd);
        std::fprintf(stderr, "\n");
    }
#endif
}

void detachedChildren()
{
    SECTION("ChildStdio: a child started detached gets no witness either");
    FileWitness file;
    Witness udp(SOCK_DGRAM);
    Witness tcp(SOCK_STREAM);
    const QString out = QDir::temp().filePath(
        QStringLiteral("mw-child-stdio-%1.out").arg(QUuid::createUuid().toString(QUuid::Id128)));
    CHECK(mw::startDetached(QCoreApplication::applicationFilePath(),
                            QStringList{QStringLiteral("--child-stdio-probe"), out} +
                                witnessSpecs(file, udp, tcp)));
    QElapsedTimer waited;
    waited.start();
    QByteArray said;
    while (waited.elapsed() < 20000) {
        QFile f(out);
        if (f.open(QIODevice::ReadOnly)) said = f.readAll();
        if (said.contains("end\n")) break;
        QThread::msleep(50);
    }
    QFile::remove(out);
    const Answer a = parse(said);
    CHECK(said.contains("end\n"));
    CHECK(a.found.isEmpty());
    CHECK(a.absent.size() == 3);
}
#endif

#ifdef Q_OS_WIN
void refused()
{
    SECTION("ChildStdio: without its handle list, the child is not started");
    FileWitness file;
    const QString out = QDir::temp().filePath(
        QStringLiteral("mw-child-stdio-%1.out").arg(QUuid::createUuid().toString(QUuid::Id128)));
    QProcess process;
    process.setProgram(QCoreApplication::applicationFilePath());
    process.setArguments({QStringLiteral("--child-stdio-probe"), out, file.spec()});
    mw::passOnlyStdio(process);
    int startedSeen = 0;
    QObject::connect(&process, &QProcess::started, [&startedSeen]() { ++startedSeen; });
    mw::childstdio::refuseNextForTests();
    process.start();
    CHECK(!process.waitForStarted(5000));
    CHECK(process.error() == QProcess::FailedToStart);
    // CreateProcess itself refused it: no child ever existed
    CHECK(startedSeen == 0);
    QThread::msleep(1000);
    // It never ran: what it would have written is not there
    CHECK(!QFile::exists(out));

    // The next start of the same QProcess gets its list again
    process.setArguments({QStringLiteral("--child-stdio-probe"), QStringLiteral("-"), file.spec()});
    process.start();
    CHECK(process.waitForStarted(10000));
    CHECK(startedSeen == 1);
    process.write("ping\n");
    process.closeWriteChannel();
    CHECK(process.waitForFinished(20000));
    const Answer a = parse(process.readAllStandardOutput());
    CHECK(a.pong);
    CHECK(a.found.isEmpty());
    QFile::remove(out);
}

void throughTheShell()
{
    // HostAppLauncher's fallback and QDesktopServices::openUrl both have the
    // shell start the program from inside this process
    SECTION("ChildStdio: a program the shell starts from this process gets no witness");
    FileWitness file;
    Witness udp(SOCK_DGRAM);
    Witness tcp(SOCK_STREAM);
    const QString out = QDir::temp().filePath(
        QStringLiteral("mw-child-stdio-%1.out").arg(QUuid::createUuid().toString(QUuid::Id128)));
    QString parameters =
        QStringLiteral("--child-stdio-probe \"%1\"").arg(QDir::toNativeSeparators(out));
    for (const QString& s : witnessSpecs(file, udp, tcp))
        parameters += QLatin1Char(' ') + s;
    const std::wstring program =
        QDir::toNativeSeparators(QCoreApplication::applicationFilePath()).toStdWString();
    const std::wstring params = parameters.toStdWString();
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    // No console of its own: no window
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NO_CONSOLE | SEE_MASK_FLAG_NO_UI;
    info.lpFile = program.c_str();
    info.lpParameters = params.c_str();
    info.nShow = SW_HIDE;
    CHECK(ShellExecuteExW(&info));
    if (info.hProcess) {
        CHECK(WaitForSingleObject(info.hProcess, 20000) == WAIT_OBJECT_0);
        CloseHandle(info.hProcess);
    }
    QFile f(out);
    const QByteArray said = f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    f.close();
    QFile::remove(out);
    const Answer a = parse(said);
    CHECK(said.contains("end\n"));
    CHECK(a.found.isEmpty());
    for (const QString& w : a.found)
        std::fprintf(stderr, "  the program the shell started holds %s\n", qPrintable(w));
}
#endif

} // namespace

/// The second process: `--child-stdio-probe <out|-> <witness>...`. Says which
/// witnesses it holds, on stdout (then answers the line read on stdin) or in
/// the file @p out.
int run_child_stdio_probe(int argc, char** argv)
{
#ifdef Q_OS_WIN
    const std::vector<int> open;
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#else
    const std::vector<int> open = openDescriptors();
#endif
    if (argc < 3) return 2;
    std::string said;
#ifndef Q_OS_WIN
    said += "fds";
    for (const int fd : open)
        said += " " + std::to_string(fd);
    said += "\n";
#endif
    for (int i = 3; i < argc; ++i)
        said += std::string(holds(argv[i], open) ? "found " : "absent ") + argv[i] + "\n";
    const bool toStdout = std::strcmp(argv[2], "-") == 0;
    if (toStdout) {
        char line[64] = {};
        if (std::fgets(line, sizeof(line), stdin)) {
            std::string got(line);
            while (!got.empty() && (got.back() == '\n' || got.back() == '\r'))
                got.pop_back();
            said += "pong " + got + "\n";
        }
    }
    said += "end\n";
    if (toStdout) {
        std::fwrite(said.data(), 1, said.size(), stdout);
        std::fflush(stdout);
        return 0;
    }
    FILE* f = std::fopen(argv[2], "wb");
    if (!f) return 3;
    std::fwrite(said.data(), 1, said.size(), f);
    std::fclose(f);
    return 0;
}

void run_child_stdio_tests()
{
#ifdef Q_OS_WIN
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    onlyItsChannels();
#ifdef Q_OS_WIN
    refused();
    throughTheShell();
#else
    theDescriptorsItIsMeantToGet();
    detachedChildren();
#endif
}
