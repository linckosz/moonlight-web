# Moonlight-Web Unit Tests
QT += core network
CONFIG += c++17 console
CONFIG -= app_bundle
TEMPLATE = app

TARGET = run_tests

# Source paths relative to backend/
INCLUDEPATH += $$PWD/../
INCLUDEPATH += $$PWD/../src

# UPNPClient test
SOURCES += \
    test_upnpclient.cpp \
    ../src/network/UPNPClient.cpp

HEADERS += \
    ../src/network/UPNPClient.h

# Link OpenSSL (needed by UPNPClient on Windows for getLocalIP fallback)
win32 {
    LIBS += -lWS2_32 -lIphlpapi
    INCLUDEPATH += $$PWD/../../../moonlight-web-deepseek/backend/libs/windows/include/x64
}

# miniupnpc (if available) — for full E2E tests
exists($$PWD/../third_party/miniupnpc/lib) {
    INCLUDEPATH += $$PWD/../third_party/miniupnpc/include
    win32:CONFIG(release, debug|release): LIBS += -L$$PWD/../third_party/miniupnpc/lib -lminiupnpc
    else:win32:CONFIG(debug, debug|release): LIBS += -L$$PWD/../third_party/miniupnpc/lib -lminiupnpc
    else:unix: LIBS += -lminiupnpc
    DEFINES += MW_HAVE_MINIUPNPC
} else {
    message("NOTE: miniupnpc not found — UPnP E2E tests will be skipped")
}

DEFINES += QT_NO_DEBUG_OUTPUT
