QT += core network websockets
CONFIG += c++17 console
TEMPLATE = app

TARGET = mw-server

INCLUDEPATH += src

# Phase 2: qmdnsengine (mDNS discovery) — built as static lib via wrapper .pro
INCLUDEPATH += $$PWD/third_party/qmdnsengine/qmdnsengine/src/include
INCLUDEPATH += $$PWD/third_party/qmdnsengine

# Phase 3: OpenSSL
INCLUDEPATH += $$PWD/libs/windows/include/x64

win32:CONFIG(release, debug|release): LIBS += -L$$OUT_PWD/third_party/qmdnsengine/release/ -lqmdnsengine
else:win32:CONFIG(debug, debug|release): LIBS += -L$$OUT_PWD/third_party/qmdnsengine/debug/ -lqmdnsengine
else:unix: LIBS += -L$$OUT_PWD/third_party/qmdnsengine/ -lqmdnsengine

SOURCES += \
    src/main.cpp \
    src/server/HttpServer.cpp \
    src/server/StaticFileHandler.cpp \
    src/server/RestRouter.cpp \
    src/common/Logger.cpp \
    src/backend/NvHTTP.cpp \
    src/backend/NvComputer.cpp \
    src/backend/ComputerManager.cpp \
    src/backend/IdentityManager.cpp \
    src/backend/NvPairingManager.cpp \
    src/streaming/StreamConfig.cpp \
    src/streaming/RtspClient.cpp \
    src/streaming/Session.cpp

HEADERS += \
    src/server/HttpServer.h \
    src/server/StaticFileHandler.h \
    src/server/RestRouter.h \
    src/common/Logger.h \
    src/common/Types.h \
    src/backend/NvAddress.h \
    src/backend/NvApp.h \
    src/backend/NvHTTP.h \
    src/backend/NvComputer.h \
    src/backend/ComputerManager.h \
    src/backend/IdentityManager.h \
    src/backend/NvPairingManager.h \
    src/streaming/StreamConfig.h \
    src/streaming/RtspClient.h \
    src/streaming/Session.h

# Frontend directory (served as static files)
DEFINES += FRONTEND_DIR=\\\"$$PWD/../frontend/\\\"

win32 {
    LIBS += -lWS2_32
    LIBS += -L$$PWD/libs/windows/lib/x64 -llibssl -llibcrypto
    CONFIG(release, debug|release) {
        QMAKE_LFLAGS += /SUBSYSTEM:WINDOWS
    }
}

macx {
    LIBS += -framework Security -framework CoreFoundation
    QMAKE_INFO_PLIST = $$PWD/Info.plist
}

unix:!macx {
    LIBS += -lpthread -ldl
}
