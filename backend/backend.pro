QT += core network websockets widgets
CONFIG += c++17 console
TEMPLATE = app

TARGET = mw-server

INCLUDEPATH += src

# Phase 2: qmdnsengine (mDNS discovery) — built as static lib via wrapper .pro
INCLUDEPATH += $$PWD/third_party/qmdnsengine/qmdnsengine/src/include
INCLUDEPATH += $$PWD/third_party/qmdnsengine

win32:CONFIG(release, debug|release): LIBS += -L$$OUT_PWD/third_party/qmdnsengine/release/ -lqmdnsengine
else:win32:CONFIG(debug, debug|release): LIBS += -L$$OUT_PWD/third_party/qmdnsengine/debug/ -lqmdnsengine
else:unix: LIBS += -L$$OUT_PWD/third_party/qmdnsengine/ -lqmdnsengine

# moonlight-common-c (control stream + input encryption)
INCLUDEPATH += $$PWD/third_party/moonlight-common-c/src
INCLUDEPATH += $$PWD/third_party/moonlight-common-c/enet/include
INCLUDEPATH += $$PWD/third_party/moonlight-common-c
INCLUDEPATH += $$PWD/third_party/moonlight-common-c/nanors/deps/obl

# libdatachannel (WebRTC DataChannel — built via CMake in build_libdatachannel.bat)
exists($$PWD/third_party/libdatachannel/install/include) {
    INCLUDEPATH += $$PWD/third_party/libdatachannel/install/include
    win32:CONFIG(release, debug|release): LIBS += -L$$PWD/third_party/libdatachannel/install/lib -ldatachannel -ljuice -lusrsctp -lsrtp2
    else:win32:CONFIG(debug, debug|release): LIBS += -L$$PWD/third_party/libdatachannel/install/debug/lib -ldatachannel -ljuice -lusrsctp -lsrtp2
    else:unix: LIBS += -L$$PWD/third_party/libdatachannel/install/lib -ldatachannel -ljuice -lusrsctp -lsrtp2
    DEFINES += RTC_STATIC RTC_ENABLE_WEBSOCKET=0 RTC_ENABLE_MEDIA=1
} else {
    # Submodule not yet initialized — headers missing, build will fail.
    # Instruct user: git submodule add https://github.com/paullouisageneau/libdatachannel.git backend/third_party/libdatachannel
    error("libdatachannel not found. Run: git submodule add https://github.com/paullouisageneau/libdatachannel.git backend/third_party/libdatachannel && cd backend/third_party/libdatachannel && cmake -B build && cmake --build build")
}

# miniupnpc (UPnP-IGD port mapping for WebRTC NAT traversal)
# Built from git submodule: backend/third_party/miniupnp
exists($$PWD/third_party/miniupnp/build/lib) {
    INCLUDEPATH += $$PWD/third_party/miniupnp/build/include
    win32:CONFIG(release, debug|release): LIBS += -L$$PWD/third_party/miniupnp/build/lib -lminiupnpc -lws2_32 -liphlpapi
    else:win32:CONFIG(debug, debug|release): LIBS += -L$$PWD/third_party/miniupnp/build/lib -lminiupnpc -lws2_32 -liphlpapi
    else:unix: LIBS += -lminiupnpc
    DEFINES += MW_HAVE_MINIUPNPC MINIUPNP_STATICLIB
} else {
    warning("miniupnpc not found — UPnP NAT traversal disabled. Run build_miniupnpc.bat to build from submodule.")
}

# OpenSSL
INCLUDEPATH += $$PWD/libs/windows/include/x64

SOURCES += \
    src/main.cpp \
    src/server/AppSettings.cpp \
    src/server/HttpServer.cpp \
    src/server/StaticFileHandler.cpp \
    src/server/RestRouter.cpp \
    src/common/Logger.cpp \
    src/network/UPNPClient.cpp \
    src/network/PdnsClient.cpp \
    src/network/StunClient.cpp \
    src/network/AcmeClient.cpp \
    src/network/InternetAccessManager.cpp \
    src/TrayManager.cpp \
    src/backend/NvHTTP.cpp \
    src/backend/NvComputer.cpp \
    src/backend/ComputerManager.cpp \
    src/backend/IdentityManager.cpp \
    src/backend/NvPairingManager.cpp \
    src/streaming/StreamConfig.cpp \
    src/streaming/Session.cpp \
    src/streaming/MoonlightShim.cpp \
    src/streaming/StreamRelay.cpp \
    # WebRTC DataChannel relay \
    src/streaming/DataChannelRelay.cpp \
    src/streaming/MediaTrackRelay.cpp \
    src/streaming/SignalingServer.cpp \
    # moonlight-common-c ENet \
    third_party/moonlight-common-c/enet/callbacks.c \
    third_party/moonlight-common-c/enet/compress.c \
    third_party/moonlight-common-c/enet/host.c \
    third_party/moonlight-common-c/enet/list.c \
    third_party/moonlight-common-c/enet/packet.c \
    third_party/moonlight-common-c/enet/peer.c \
    third_party/moonlight-common-c/enet/protocol.c \
    third_party/moonlight-common-c/enet/win32.c \
    # moonlight-common-c core (existing) \
    third_party/moonlight-common-c/src/Platform.c \
    third_party/moonlight-common-c/src/PlatformSockets.c \
    third_party/moonlight-common-c/src/PlatformCrypto.c \
    third_party/moonlight-common-c/src/ControlStream.c \
    third_party/moonlight-common-c/src/InputStream.c \
    third_party/moonlight-common-c/src/LinkedBlockingQueue.c \
    third_party/moonlight-common-c/src/Misc.c \
    third_party/moonlight-common-c/src/FakeCallbacks.c \
    third_party/moonlight-common-c/src/ByteBuffer.c \
    # moonlight-common-c nanors (Reed-Solomon FEC) \
    third_party/moonlight-common-c/nanors/rs.c \
    # moonlight-common-c RS wrapper \
    third_party/moonlight-common-c/src/rswrapper.c \
    # moonlight-common-c core (added for LiStartConnection) \
    third_party/moonlight-common-c/src/Connection.c \
    third_party/moonlight-common-c/src/VideoStream.c \
    third_party/moonlight-common-c/src/AudioStream.c \
    third_party/moonlight-common-c/src/RtspConnection.c \
    third_party/moonlight-common-c/src/RtspParser.c \
    third_party/moonlight-common-c/src/SdpGenerator.c \
    third_party/moonlight-common-c/src/VideoDepacketizer.c \
    third_party/moonlight-common-c/src/RtpVideoQueue.c \
    third_party/moonlight-common-c/src/RtpAudioQueue.c

HEADERS += \
    src/server/AppSettings.h \
    src/server/HttpServer.h \
    src/server/StaticFileHandler.h \
    src/server/RestRouter.h \
    src/common/Logger.h \
    src/common/Types.h \
    src/network/UPNPClient.h \
    src/network/PdnsClient.h \
    src/network/StunClient.h \
    src/network/AcmeClient.h \
    src/network/InternetAccessManager.h \
    src/TrayManager.h \
    src/backend/NvAddress.h \
    src/backend/NvApp.h \
    src/backend/NvHTTP.h \
    src/backend/NvComputer.h \
    src/backend/ComputerManager.h \
    src/backend/IdentityManager.h \
    src/backend/NvPairingManager.h \
    src/streaming/StreamConfig.h \
    src/streaming/Session.h \
    src/streaming/RelayBase.h \
    src/streaming/DataChannelRelay.h \
    src/streaming/MediaTrackRelay.h \
    src/streaming/SignalingServer.h \
    src/streaming/MoonlightShim.h \
    src/streaming/StreamRelay.h \
    # moonlight-common-c headers \
    third_party/moonlight-common-c/src/Platform.h \
    third_party/moonlight-common-c/src/PlatformSockets.h \
    third_party/moonlight-common-c/src/PlatformCrypto.h \
    third_party/moonlight-common-c/src/PlatformThreads.h \
    third_party/moonlight-common-c/src/Limelight.h \
    third_party/moonlight-common-c/src/Limelight-internal.h \
    third_party/moonlight-common-c/src/LinkedBlockingQueue.h \
    third_party/moonlight-common-c/src/Input.h \
    third_party/moonlight-common-c/src/Rtsp.h \
    third_party/moonlight-common-c/src/Video.h \
    third_party/moonlight-common-c/src/RtpAudioQueue.h \
    third_party/moonlight-common-c/src/RtpVideoQueue.h \
    third_party/moonlight-common-c/src/ByteBuffer.h

# Frontend directory (served as static files)
DEFINES += FRONTEND_DIR=\\\"$$PWD/../frontend/\\\"
DEFINES += CERT_DIR=\\\"$$PWD/cert/\\\"

win32 {
    LIBS += -lWS2_32 -lwinmm -lBcrypt -lCrypt32
    LIBS += -L$$PWD/libs/windows/lib/x64 -llibssl -llibcrypto
}

macx {
    LIBS += -framework Security -framework CoreFoundation
    QMAKE_INFO_PLIST = $$PWD/Info.plist
}

unix:!macx {
    LIBS += -lpthread -ldl
}
