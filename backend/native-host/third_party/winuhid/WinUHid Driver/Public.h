//
// Shared IOCTL interface between WinUHid.dll and WinUHidDriver.dll
//

#pragma once

#include "../WinUHid/WinUHid.h"

#include <winioctl.h>

#ifdef MOONLIGHTWEB_VHID
// MoonlightWeb: a name of our own, so a WinUHid installed by someone else never collides
#define WINUHID_DEVICE_NAME L"MoonlightWebVHid"
#else
#define WINUHID_DEVICE_NAME L"WinUHid"
#endif
#define WINUHID_WIN32_PATH (L"\\\\.\\" WINUHID_DEVICE_NAME)
#define WINUHID_NT_PATH (L"\\DosDevices\\Global\\" WINUHID_DEVICE_NAME)

#define WINUHID_CTL_CODE(x) CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800 + (x), METHOD_BUFFERED, FILE_WRITE_DATA)

#define WINUHID_NA_CTL_CODE(x) CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800 + (x), METHOD_BUFFERED, FILE_ANY_ACCESS)

#define WINUHID_VERSION_LATEST 1

typedef struct _WINUHID_DEVICE_INFO {
    //
    // Interface version of the client
    //
    ULONG InterfaceVersion;

    //
    // Set all flags corresponding to operations you wish to support.
    // Unsupported operations will be failed by VHF.
    //
    WINUHID_EVENT_TYPE SupportedEvents;

    USHORT VendorID;
    USHORT ProductID;
    USHORT VersionNumber;

    GUID ContainerId;
} WINUHID_DEVICE_INFO, *PWINUHID_DEVICE_INFO;

//
// Required before issuing IOCTL_WINUHID_CREATE_DEVICE. Expected input is a valid WINUHID_DEVICE_ID struct.
//
#define IOCTL_WINUHID_SET_DEVICE_INFO WINUHID_CTL_CODE(0)

//
// Required before issuing IOCTL_WINUHID_CREATE_DEVICE. Expected input is the raw HID report descriptor.
//
#define IOCTL_WINUHID_SET_REPORT_DESCRIPTOR WINUHID_CTL_CODE(1)

//
// Optional, but must be issued before IOCTL_WINUHID_CREATE_DEVICE. Expected input is a null terminated wide string.
//
#define IOCTL_WINUHID_SET_INSTANCE_ID WINUHID_CTL_CODE(2)

//
// Optional, but must be issued before IOCTL_WINUHID_CREATE_DEVICE. Expected input is a REG_MULTI_SZ wide string.
//
#define IOCTL_WINUHID_SET_HARDWARE_IDS WINUHID_CTL_CODE(3)

//
// Creates the device. No input or output data.
//
#define IOCTL_WINUHID_CREATE_DEVICE WINUHID_CTL_CODE(4)

//
// Starts the device. If any supported events were specified, issue IOCTL_WINUHID_GET_NEXT_EVENT soon after this IOCTL.
//
#define IOCTL_WINUHID_START_DEVICE WINUHID_CTL_CODE(5)

//
// Pends inside WinUHid until an event is ready, then outputs a WINUHID_EVENT struct.
// The caller *must* invoke IOCTL_WINUHID_COMPLETE_READ_EVENT or IOCTL_WINUHID_COMPLETE_WRITE_EVENT afterwards.
//
#define IOCTL_WINUHID_GET_NEXT_EVENT WINUHID_CTL_CODE(6)

#include <pshpack1.h>
typedef struct _WINUHID_WRITE_EVENT_COMPLETE {
    WINUHID_REQUEST_ID RequestId;
    NTSTATUS Status;
} WINUHID_WRITE_EVENT_COMPLETE, *PWINUHID_WRITE_EVENT_COMPLETE;
#include <poppack.h>

#define IOCTL_WINUHID_COMPLETE_WRITE_EVENT WINUHID_CTL_CODE(7)
#include <pshpack1.h>
typedef struct _WINUHID_READ_EVENT_COMPLETE {
    WINUHID_REQUEST_ID RequestId;
    NTSTATUS Status;
    ULONG DataLength;
    UCHAR Data[ANYSIZE_ARRAY];
} WINUHID_READ_EVENT_COMPLETE, *PWINUHID_READ_EVENT_COMPLETE;
#include <poppack.h>

#define IOCTL_WINUHID_COMPLETE_READ_EVENT WINUHID_CTL_CODE(8)

//
// Queries for the maximum supported interface version. Output is a ULONG.
//
// See WINUHID_VERSION_*.
//
#define IOCTL_WINUHID_GET_INTERFACE_VERSION WINUHID_NA_CTL_CODE(9)

#ifdef MOONLIGHTWEB_VHID
//
// MoonlightWeb: the fixes of ours this driver has. Output is a ULONG of
// MOONLIGHTWEB_VHID_FIX_* flags (WinUHid.h). A driver older than the IOCTL fails
// it as any IOCTL it does not know (ERROR_INVALID_FUNCTION): none.
//
#define IOCTL_MOONLIGHTWEB_VHID_GET_FIXES WINUHID_NA_CTL_CODE(0x40)

#define MOONLIGHTWEB_VHID_FIXES (MOONLIGHTWEB_VHID_FIX_EVENT_WAKEUP | MOONLIGHTWEB_VHID_FIX_SYSTEM_MOUSE | MOONLIGHTWEB_VHID_FIX_SYSTEM_KEYBOARD)
#endif