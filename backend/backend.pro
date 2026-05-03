QT += core network websockets
CONFIG += c++17 console
TEMPLATE = app

TARGET = mw-server

SOURCES += \
    src/main.cpp \
    src/server/HttpServer.cpp \
    src/server/StaticFileHandler.cpp \
    src/server/RestRouter.cpp \
    src/common/Logger.cpp

HEADERS += \
    src/server/HttpServer.h \
    src/server/StaticFileHandler.h \
    src/server/RestRouter.h \
    src/common/Logger.h

# Frontend directory (served as static files)
DEFINES += FRONTEND_DIR=\\\"$$PWD/../frontend/\\\"

win32 {
    LIBS += -lWS2_32
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
