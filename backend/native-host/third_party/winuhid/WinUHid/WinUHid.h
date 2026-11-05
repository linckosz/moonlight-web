#pragma once

#ifdef __cplusplus
#define WINUHID_EXTERN_C extern "C"
#else
#define WINUHID_EXTERN_C
#endif

#if defined(WINUHID_STATIC)
#define WINUHID_API WINUHID_EXTERN_C
#elif defined(WINUHID_EXPORTS)
#define WINUHID_API WINUHID_EXTERN_C __declspec(dllexport)
#else
#define WINUHID_API WINUHID_EXTERN_C __declspec(dllimport)
#endif

typedef enum _WINUHID_EVENT_TYPE {
	WINUHID_EVENT_NONE = 0x0,
	WINUHID_EVENT_GET_FEATURE = 0x1,
	WINUHID_EVENT_SET_FEATURE = 0x2,
	WINUHID_EVENT_WRITE_REPORT = 0x4,
	WINUHID_EVENT_READ_REPORT = 0x8,
} WINUHID_EVENT_TYPE, *PWINUHID_EVENT_TYPE;

#define WINUHID_EVENT_TYPE_IS_READ(x) ((x) == WINUHID_EVENT_GET_FEATURE || (x) == WINUHID_EVENT_READ_REPORT)
#define WINUHID_EVENT_TYPE_IS_WRITE(x) ((x) == WINUHID_EVENT_SET_FEATURE || (x) == WINUHID_EVENT_WRITE_REPORT)

#include <pshpack1.h>

typedef struct _WINUHID_DEVICE_CONFIG {
	//
	// Set all flags corresponding to operations you wish to support.
	// Unsupported operations will silently be failed by the OS.
	//
	WINUHID_EVENT_TYPE SupportedEvents;

	USHORT VendorID;
	USHORT ProductID;
	USHORT VersionNumber;

	//
	// Specifies the HID report descriptor
	//
	USHORT ReportDescriptorLength;
	LPCVOID ReportDescriptor;

	//
	// Optionally distinguishes the physical device collection
	//
	GUID ContainerId;

	//
	// Optionally distinguishes instances of the same device
	//
	PCWSTR InstanceID;

	//
	// Optionally specifies additional hardware IDs to provide for PnP enumeration
	//
	// Note: This value must be a REG_MULTI_SZ - that is, a set of null terminated strings terminated by a second consecutive null.
	//
	PCWSTR HardwareIDs;

	//
	// When WINUHID_EVENT_READ_REPORT is specified, this value throttles the rate
	// at which WINUHID_EVENT_READ_REPORT events are returned to the caller when
	// operating in callback mode. If set to 0, throttling is disabled.
	//
	// WARNING: Throttling read reports is very important to avoid causing applications
	// to enter a tight loop reading input from your device. Even if you don't use
	// WinUHid's throttling (or you use WinUHidPollEvent() directly), you must implement
	// some delay between input reports to avoid input devices being perpetually readable.
	//
	// The value is specified in microseconds.
	//
	UINT ReadReportPeriodUs;
} WINUHID_DEVICE_CONFIG, *PWINUHID_DEVICE_CONFIG;
typedef CONST WINUHID_DEVICE_CONFIG *PCWINUHID_DEVICE_CONFIG;

typedef ULONG WINUHID_REQUEST_ID;

typedef struct _WINUHID_EVENT {
	//
	// For output events (SET/WRITE), the raw data that should be sent to the
	// virtual device is included directly in the event. When the operation is
	// completed, you must invoke WinUHidCompleteWriteEvent(). If the device
	// uses numbered reports, the first byte of the data buffer will contain
	// the report ID.
	//
	// For input events (GET/READ), the caller must provide the read data by
	// calling WinUHidCompleteReadEvent(). For some READ events, the data
	// length and report ID will be 0. This indicates that you may complete
	// the request with any valid input report for the device.
	//
	WINUHID_EVENT_TYPE Type;

	//
	// This is an opaque handle that identifies the event. It must be not be modified.
	//
	WINUHID_REQUEST_ID RequestId;

	UCHAR ReportId;
	union {
		struct {
			ULONG DataLength;
			UCHAR Data[ANYSIZE_ARRAY];
		} Write;
		struct {
			ULONG DataLength;
		} Read;
	};
} WINUHID_EVENT, *PWINUHID_EVENT;
typedef CONST WINUHID_EVENT *PCWINUHID_EVENT;

#include <poppack.h>

typedef struct _WINUHID_DEVICE *PWINUHID_DEVICE;

//
// Queries the WinUHid driver for the maximum support driver interface version.
// This can be used to determine whether specific driver features are available,
// or whether the driver is installed/usable at all.
//
// NOTE: Checking interface version is completely optional. You can simply try
// using WinUHid features and handle failures (which you should do anyway!)
//
// On failure, this function returns 0. Call GetLastError() to the error code.
//
WINUHID_API DWORD WinUHidGetDriverInterfaceVersion(VOID);

//
// Creates a new device based upon the provided configuration. The configuration need
// only be valid for the duration of the function call and can be freed afterwards.
//
// To destroy the device, call WinUHidDestroyDevice().
//
// On failure, the function will return NULL. Call GetLastError() to the error code.
//
WINUHID_API PWINUHID_DEVICE WinUHidCreateDevice(PCWINUHID_DEVICE_CONFIG Config);

//
// Submits an input report to the device. This can be called before WinUHidStartDevice().
//
// Note: The provided report must be exactly the expected length based on the report descriptor.
// If the device uses numbered reports, the report ID byte must be prepended to the report data.
//
// If you have enabled WINUHID_EVENT_READ_REPORT, this function may return FALSE and GetLastError()
// will return ERROR_NOT_READY if the device is not yet ready to receive your input report yet.
// In that case, you should wait until you receive WINUHID_EVENT_READ_REPORT then submit this report
// to WinUHidCompleteReadEvent().
//
//
// On failure, the function will return FALSE. Call GetLastError() to the error code.
//
WINUHID_API BOOL WinUHidSubmitInputReport(PWINUHID_DEVICE Device, LPCVOID Report, DWORD ReportSize);

//
// The WinUHid event callback is invoked each time one of the registered SupportedEvents occurs.
//
// This callback runs in the context of the internal thread polling the device for events, so
// it should not perform any blocking operations.
//
// The callback is responsible for calling WinUHidCompleteWriteEvent() or WinUHidCompleteReadEvent()
// for each provided Event, though it may choose to wait to do this until after the callback returns.
//
// Note: It is possible for this callback to be invoked again with a new event even before
// WinUHidCompleteWriteEvent() or WinUHidCompleteReadEvent() is called for the previous event.
//
typedef VOID WINUHID_EVENT_CALLBACK(PVOID CallbackContext, PWINUHID_DEVICE Device, PCWINUHID_EVENT Event);
typedef WINUHID_EVENT_CALLBACK* PWINUHID_EVENT_CALLBACK;

//
// Starts a device and registers a callback to be called when a registered event occurs.
//
// Note: The caller may optionally choose not to provide EventCallback and instead use WinUHidPollEvent().
//
// On failure, the function will return FALSE. Call GetLastError() to the error code.
//
WINUHID_API BOOL WinUHidStartDevice(PWINUHID_DEVICE Device, PWINUHID_EVENT_CALLBACK EventCallback, PVOID CallbackContext);

//
// Polls the device for HID events, optionally waiting if none are currently available.
// The returned event must be passed to WinUHidCompleteWriteEvent() or WinUHidCompleteReadEvent().
//
// It is safe to concurrently invoke this function on multiple threads with the same device
// if a single thread is insufficient to satisfy the request load of the HID device. However,
// you must use WinUHidStopDevice() and wait for all calls to return before calling
// WinUHidDestroyDevice().
//
// If you specified WINUHID_EVENT_READ_REPORT when creating the device, it is strongly recommended
// that you use event callbacks which will allow you to throttle the rate that these events are
// delivered to your callback. If you don't, you must implement your own throttling to prevent
// input devices from being perpetually readable to avoid causing problems in other HID clients.
//
// This function must not be invoked if an event callback was registered in WinUHidStartDevice().
//
// If no event is available before the timeout expires, the function will return NULL.
//
WINUHID_API PCWINUHID_EVENT WinUHidPollEvent(PWINUHID_DEVICE Device, DWORD TimeoutMillis);

//
// Completes a write request and informs the the HID client whether the request was successful.
//
// NOTE: The Microsoft VHF.sys driver maintains an internal timeout for asynchronous requests
// and may silently cancel your pending requests if you take too long to complete them. To avoid
// that, keep a thread polling the device (if not in callback mode) and always try to complete a
// request within a couple seconds of receiving it.
//
// This function never fails as long as the provided arguments are valid.
//
WINUHID_API VOID WinUHidCompleteWriteEvent(PWINUHID_DEVICE Device, PCWINUHID_EVENT Event, BOOL Success);

//
// Completes a read request by providing the data requested by the HID client. If the request
// was unsuccessful for some reason, providing a NULL data pointer will fail the read.
//
// If DataLength is not equal to WINUHID_EVENT.Read.DataLength:
// - If DataLength < Read.DataLength, the completed buffer will be padded with zero bytes.
// - If DataLength > Read.DataLength, the completed buffer will be truncated to Read.DataLength bytes.
//
// If WINUHID_EVENT.Read.DataLength and ReportId was 0, you may provide any valid input report.
//
// If the device uses numbered reports, the first byte of the data buffer must be the report ID.
//
// NOTE: The Microsoft VHF.sys driver maintains an internal timeout for asynchronous requests
// and may silently cancel your pending requests if you take too long to complete them. To avoid
// that, keep a thread polling the device (if not in callback mode) and always try to complete a
// request within a couple seconds of receiving it.
//
// This function never fails as long as the provided arguments are valid.
//
WINUHID_API VOID WinUHidCompleteReadEvent(PWINUHID_DEVICE Device, PCWINUHID_EVENT Event, LPCVOID Data, DWORD DataLength);

//
// Stops event processing for this device and interrupts any pending poll calls.
//
// It is not necessary to call this function unless you are destroying a device
// while another thread may be inside WinUHidPollEvent(). In that case, you must
// call WinUHidStopDevice() then wait for all WinUHidPollEvent() calls to return
// before finally calling WinUHidDestroyDevice().
//
// This function never fails as long as the provided argument is valid.
//
WINUHID_API VOID WinUHidStopDevice(PWINUHID_DEVICE Device);

//
// Destroys the device.
//
// Note: You are responsible for completing any events you have received before calling this!
//
// This function never fails as long as the provided argument is valid.
//
WINUHID_API VOID WinUHidDestroyDevice(PWINUHID_DEVICE Device);

#ifdef MOONLIGHTWEB_VHID
//
// MoonlightWeb: a device that outlives the process that created it. The driver
// keeps a device for as long as any handle to its file object is open, so a
// process can hand a duplicate of the handle to another one, which carries on.
//

//
// The file handle behind a device, to duplicate into another process.
//
WINUHID_API HANDLE WinUHidGetHandle(PWINUHID_DEVICE Device);

//
// Wraps a handle another process created and started a device on (this
// process's own duplicate). Nothing is sent to the driver but a version query,
// which a handle to anything else fails. The device starts stopped: call
// WinUHidResumeDevice() to service its events. On success the device owns the
// handle; on failure (NULL) the caller still does.
//
WINUHID_API PWINUHID_DEVICE WinUHidAdoptDevice(HANDLE Handle);

//
// Services the events of a stopped device again (WinUHidStopDevice(), or
// WinUHidAdoptDevice()), without starting it in the driver a second time.
//
WINUHID_API BOOL WinUHidResumeDevice(PWINUHID_DEVICE Device, PWINUHID_EVENT_CALLBACK EventCallback, PVOID CallbackContext);

//
// TRUE while the device is started and its event thread still runs (it ends
// when polling fails): for a diagnostic, not a synchronisation.
//
WINUHID_API BOOL WinUHidIsServicingEvents(PWINUHID_DEVICE Device);

//
// The driver's IOCTL_WINUHID_GET_NEXT_EVENT parks itself under the lock
// operations are queued under, so an operation never waits for a next one to be
// handed out: events may be waited for without end (11/10/2026).
//
#define MOONLIGHTWEB_VHID_FIX_EVENT_WAKEUP 0x00000001

//
// IOCTL_WINUHID_SET_REPORT_DESCRIPTOR accepts the host's own mouse, one fixed
// descriptor compared byte for byte, beside the game devices it took already
// (19/10/2026). An older driver refuses it with ERROR_ACCESS_DENIED.
//
#define MOONLIGHTWEB_VHID_FIX_SYSTEM_MOUSE 0x00000002

//
// It accepts the host's own keyboard too, one fixed descriptor compared byte
// for byte (04/11/2026). An older driver refuses it with ERROR_ACCESS_DENIED.
//
#define MOONLIGHTWEB_VHID_FIX_SYSTEM_KEYBOARD 0x00000004

//
// The MOONLIGHTWEB_VHID_FIX_* flags of the driver loaded when the device was
// created or adopted; 0 for a driver older than them.
//
WINUHID_API ULONG WinUHidGetDriverFixes(PWINUHID_DEVICE Device);
#endif