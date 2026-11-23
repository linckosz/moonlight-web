/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 */
#include "test_framework.h"
#include "common/DetachedProcess.h"

#include <QProcess>

// A child started detached gets none of the server's sockets: on Windows
// the handles are not passed (a relaunched server would otherwise hold the
// sockets of the one it replaces until it exits), on Unix Qt closes the
// descriptors. Pinned on the QProcess that mw::startDetached prepares.
void run_detached_process_tests()
{
    SECTION("DetachedProcess");

    QProcess process;
    mw::closeInheritedDescriptors(process);
#if defined(Q_OS_WIN)
    const QProcess::CreateProcessArgumentModifier modifier =
        process.createProcessArgumentsModifier();
    CHECK(static_cast<bool>(modifier));
    if (modifier) {
        QProcess::CreateProcessArguments arguments{};
        arguments.inheritHandles = true;
        modifier(&arguments);
        CHECK(!arguments.inheritHandles);
        // Nothing else is touched.
        CHECK(arguments.flags == 0);
        CHECK(arguments.startupInfo == nullptr);
    }
    // No standard channel is redirected by default.
    CHECK(process.processChannelMode() == QProcess::SeparateChannels);
#elif defined(Q_OS_UNIX) && QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    CHECK(process.unixProcessParameters().flags.testFlag(
        QProcess::UnixProcessFlag::CloseFileDescriptors));
#endif
}
