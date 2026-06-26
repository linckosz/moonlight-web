# Moonlight-Web — Backend TNR (non-regression) unit tests.
#
# Builds a console runner over the in-scope, deterministic logic units, linked
# against their REAL dependencies (no fragile stubs). Run under OpenCppCoverage
# via run_coverage.bat to enforce the 70% gate on these sources.
#
# Build standalone:
#   qmake tests.pro && nmake     (from an MSVC x64 environment)
#
# NOTE: the legacy test_upnpclient.cpp (separate main, needs a LAN gateway) is
# intentionally not part of this runner. See run_upnp_tests.bat to run it.

QT += core network
CONFIG += c++17 console
CONFIG -= app_bundle
TEMPLATE = app
TARGET = run_tests

# Resolve production includes the same way backend.pro does.
INCLUDEPATH += $$PWD/../src
INCLUDEPATH += $$PWD
# Limelight.h (header-only macros used by StreamConfig).
INCLUDEPATH += $$PWD/../third_party/moonlight-common-c/src
# OpenSSL headers (InputCrypto / AuthManager).
INCLUDEPATH += $$PWD/../libs/windows/include/x64

# ── Test sources ────────────────────────────────────────────────────────────
SOURCES += \
    main.cpp \
    test_input_encoder.cpp \
    test_stream_config.cpp \
    test_input_crypto.cpp \
    test_rest_router.cpp \
    test_app_settings.cpp \
    test_auth_manager.cpp

# ── In-scope production units (+ their real deps) ────────────────────────────
SOURCES += \
    ../src/streaming/InputEncoder.cpp \
    ../src/streaming/StreamConfig.cpp \
    ../src/streaming/InputCrypto.cpp \
    ../src/server/RestRouter.cpp \
    ../src/server/AppSettings.cpp \
    ../src/server/AuthManager.cpp \
    ../src/common/Logger.cpp

# Q_OBJECT classes — listed so qmake runs moc on them.
HEADERS += \
    ../src/server/RestRouter.h \
    ../src/server/AuthManager.h \
    ../src/server/AppSettings.h \
    ../src/streaming/InputEncoder.h \
    ../src/streaming/StreamConfig.h \
    ../src/streaming/InputCrypto.h \
    ../src/common/Logger.h \
    test_framework.h

# ── Platform libs ────────────────────────────────────────────────────────────
win32 {
    LIBS += -lWS2_32 -lwinmm -lBcrypt -lCrypt32
    LIBS += -L$$PWD/../libs/windows/lib/x64 -llibssl -llibcrypto
}
unix:!macx: LIBS += -lssl -lcrypto -lpthread -ldl
macx: LIBS += -lssl -lcrypto

# Silence production qDebug() noise; keep qInfo()/qCritical() (test output).
DEFINES += QT_NO_DEBUG_OUTPUT

# Emit PDB debug info even in release so OpenCppCoverage can map line coverage.
win32-msvc {
    QMAKE_CXXFLAGS += /Zi
    QMAKE_LFLAGS += /DEBUG
}
