QT -= gui
QT -= core

TARGET = enet
TEMPLATE = lib
CONFIG += staticlib c++17 warn_off

INCLUDEPATH += $$PWD/include

SOURCES += \
    callbacks.c \
    compress.c \
    host.c \
    list.c \
    packet.c \
    peer.c \
    protocol.c \
    win32.c

HEADERS += \
    include/enet/callbacks.h \
    include/enet/enet.h \
    include/enet/list.h \
    include/enet/protocol.h \
    include/enet/time.h \
    include/enet/types.h \
    include/enet/unix.h \
    include/enet/utility.h \
    include/enet/win32.h

win32: LIBS += -lWS2_32 -lWinmm
