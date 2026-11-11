# ============================================================================
# mw-hid-poc — the HID passthrough's first proof (plan H2).
#
# hid-probe.html reads a game device through WebHID and sends its collections
# and input reports over a WebSocket; this tool rebuilds the report descriptor
# with the engine's own encoder (src/input/HidDescriptor.cpp), checks it with
# validate(), and creates the same device on this host: /dev/uhid on Linux. The
# Windows side waits for the driver of P1. `--selftest reading.json` encodes a
# saved reading without creating anything, on any OS.
#
# A lab instrument like the scaler bench: never installed, never linked into
# mw-native-host, built on demand with -DMW_BUILD_TOOLS=ON. It also configures
# on its own — `cmake -S backend/native-host/tools/hid-poc` — so a bench box
# needs Qt (Core, WebSockets) and nothing else of the backend.
# ============================================================================

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.21)
    project(mw-hid-poc LANGUAGES CXX)
endif()

find_package(Qt6 QUIET COMPONENTS Core WebSockets)
if(NOT Qt6WebSockets_FOUND)
    message(STATUS "mw-hid-poc skipped: needs Qt Core and WebSockets")
    return()
endif()

set(MW_HID_POC_SOURCES
    src/main.cpp
    # The page's collections, read as the relay reads them.
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../src/streaming/HidCollectionsJson.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/input/HidDescriptor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/input/HidPid.cpp
)
set(MW_WINUHID ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/winuhid)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND MW_HID_POC_SOURCES src/VirtualHid_linux.cpp)
elseif(WIN32)
    # The driver of P1 (drivers/vhid), through WinUHid's client library built in.
    list(APPEND MW_HID_POC_SOURCES src/VirtualHid_windows.cpp "${MW_WINUHID}/WinUHid/WinUHid.cpp")
else()
    list(APPEND MW_HID_POC_SOURCES src/VirtualHid_none.cpp)
endif()

add_executable(mw-hid-poc ${MW_HID_POC_SOURCES} src/VirtualHid.h)
set_target_properties(mw-hid-poc PROPERTIES AUTOMOC ON AUTOUIC OFF AUTORCC OFF)
target_compile_features(mw-hid-poc PRIVATE cxx_std_17)
target_include_directories(mw-hid-poc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src ${CMAKE_CURRENT_SOURCE_DIR}/../../include
                                              ${CMAKE_CURRENT_SOURCE_DIR}/../../../src)
target_link_libraries(mw-hid-poc PRIVATE Qt6::Core Qt6::WebSockets)
if(WIN32)
    target_include_directories(mw-hid-poc PRIVATE "${MW_WINUHID}/WinUHid")
    target_compile_definitions(mw-hid-poc PRIVATE WINUHID_STATIC MOONLIGHTWEB_VHID)
endif()
