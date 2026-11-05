#include "WinUHid.h"
#include "WinUHid.tmh"

#include <intsafe.h>

#pragma comment(lib, "VhfUm.lib")

VOID
PostProcessReportSize(
    _Inout_ PULONG Size,
    _In_ BOOLEAN NumberedReports
)
{
    ULONG hidReportSizeBytes = *Size / 8;
    if (*Size % 8) {
        //
        // Round up to the next byte boundary
        //
        hidReportSizeBytes++;
    }
    if (NumberedReports) {
        //
        // Add a byte for the report ID prefix
        //
        hidReportSizeBytes++;
    }
    *Size = hidReportSizeBytes;
}

//
// Parses the HID report descriptor to determine report sizes and whether numbered reports are used
//
NTSTATUS
ParseReportDescriptor(
    _In_ PFILE_CONTEXT FileContext
)
{
    struct {
        UCHAR ReportId;
        ULONG ReportCount;
        ULONG ReportSize;
    } hidParserFrames[32]; // Arbitrary limit that should be enough for any device
    UCHAR currentFrame = 0;
    ULONG i = 0;

    //
    // Reinitialize parser state and parsed data
    //
    FileContext->NumberedReports = FALSE;
    RtlZeroMemory(FileContext->ReportSizes, sizeof(FileContext->ReportSizes));
    RtlZeroMemory(hidParserFrames, sizeof(hidParserFrames));

    while (i < FileContext->VhfConfig.ReportDescriptorLength) {
        UCHAR header = FileContext->VhfConfig.ReportDescriptor[i++];
        UCHAR tag = (header >> 4) & 0x0F;
        UCHAR type = (header >> 2) & 0x03;
        UCHAR size = header & 0x03;

        if (tag == 0xF) {
            //
            // Long items
            //
            if (i + 2 > FileContext->VhfConfig.ReportDescriptorLength) {
                TraceEvents(
                    TRACE_LEVEL_ERROR,
                    TRACE_PARSING,
                    "Out of bounds parsing long item header from report descriptor");
                return STATUS_INVALID_PARAMETER;
            }

            size = FileContext->VhfConfig.ReportDescriptor[i++];
            tag = FileContext->VhfConfig.ReportDescriptor[i++];
        }
        else {
            //
            // Short items
            //
            if (size == 3) {
                //
                // Size 3 actually means 4 bytes
                //
                size++;
            }
        }

        //
        // Check that the report descriptor contains enough bytes for the data value
        //
        if (i + size > FileContext->VhfConfig.ReportDescriptorLength) {
            TraceEvents(
                TRACE_LEVEL_ERROR,
                TRACE_PARSING,
                "Out of bounds reading %u bytes of data from report descriptor",
                size);
            return STATUS_INVALID_PARAMETER;
        }

        ULONG data;
        switch (size) {
        case 0:
            data = 0;
            break;
        case 1:
            data = FileContext->VhfConfig.ReportDescriptor[i++];
            break;
        case 2:
            data = FileContext->VhfConfig.ReportDescriptor[i++];
            data |= ((ULONG)FileContext->VhfConfig.ReportDescriptor[i++]) << 8;
            break;
        case 4:
            data = FileContext->VhfConfig.ReportDescriptor[i++];
            data |= ((ULONG)FileContext->VhfConfig.ReportDescriptor[i++]) << 8;
            data |= ((ULONG)FileContext->VhfConfig.ReportDescriptor[i++]) << 16;
            data |= ((ULONG)FileContext->VhfConfig.ReportDescriptor[i++]) << 24;
            break;
        default:
            //
            // Long items are not relevant for any tags we care about
            //
            i += size;
            continue;
        }

        if (type == 0x01) { // Global item
            switch (tag) {
            case 0x7: // Report size
                hidParserFrames[currentFrame].ReportSize = data;
                break;
            case 0x8: // Report ID
                if (ULongToUChar(data, &hidParserFrames[currentFrame].ReportId) != S_OK) {
                    TraceEvents(
                        TRACE_LEVEL_ERROR,
                        TRACE_PARSING,
                        "Invalid data size for report ID item: %u",
                        size);
                    return STATUS_INVALID_PARAMETER;
                }
                FileContext->NumberedReports = TRUE;
                break;
            case 0x9: // Report count
                hidParserFrames[currentFrame].ReportCount = data;
                break;
            case 0xA: // Push state frame
                if (currentFrame + 1 >= ARRAYSIZE(hidParserFrames)) {
                    TraceEvents(
                        TRACE_LEVEL_ERROR,
                        TRACE_PARSING,
                        "Attempting to push more state frames than supported");
                    return STATUS_NOT_IMPLEMENTED;
                }

                hidParserFrames[currentFrame + 1] = hidParserFrames[currentFrame];
                currentFrame++;
                break;
            case 0xB: // Pop state frame
                if (currentFrame == 0) {
                    TraceEvents(
                        TRACE_LEVEL_ERROR,
                        TRACE_PARSING,
                        "Underflow detected when popping state frame");
                    return STATUS_INVALID_PARAMETER;
                }

                currentFrame--;
                break;
            default:
                break;
            }
        }
        else if (type == 0x00) { // Main item
            ULONG reportDataBits;
            PHID_REPORT_SIZE hidReportSizeEntry = &FileContext->ReportSizes[hidParserFrames[currentFrame].ReportId];

            if (ULongMult(hidParserFrames[currentFrame].ReportSize,
                          hidParserFrames[currentFrame].ReportCount,
                          &reportDataBits) != S_OK) {
                TraceEvents(
                    TRACE_LEVEL_ERROR,
                    TRACE_PARSING,
                    "Integer overflow calculating report data bits");
                return STATUS_INTEGER_OVERFLOW;
            }

            switch (tag) {
            case 0x8: // Input
                if (ULongAdd(hidReportSizeEntry->InputReportSize, reportDataBits, &hidReportSizeEntry->InputReportSize) != S_OK) {
                    TraceEvents(
                        TRACE_LEVEL_ERROR,
                        TRACE_PARSING,
                        "Integer overflow calculating input report size");
                    return STATUS_INTEGER_OVERFLOW;
                }
                break;
            case 0x9: // Output
                if (ULongAdd(hidReportSizeEntry->OutputReportSize, reportDataBits, &hidReportSizeEntry->OutputReportSize) != S_OK) {
                    TraceEvents(
                        TRACE_LEVEL_ERROR,
                        TRACE_PARSING,
                        "Integer overflow calculating output report size");
                    return STATUS_INTEGER_OVERFLOW;
                }
                break;
            case 0xB: // Feature
                if (ULongAdd(hidReportSizeEntry->FeatureReportSize, reportDataBits, &hidReportSizeEntry->FeatureReportSize) != S_OK) {
                    TraceEvents(
                        TRACE_LEVEL_ERROR,
                        TRACE_PARSING,
                        "Integer overflow calculating feature report size");
                    return STATUS_INTEGER_OVERFLOW;
                }
                break;
            }
        }
    }

    //
    // Perform post processing to convert bits to bytes and add required padding
    //
    for (i = 0; i < ARRAYSIZE(FileContext->ReportSizes); i++) {
        PHID_REPORT_SIZE hidReportSizeEntry = &FileContext->ReportSizes[i];
        if (hidReportSizeEntry->InputReportSize) {
            PostProcessReportSize(&hidReportSizeEntry->InputReportSize, FileContext->NumberedReports);

            TraceEvents(
                TRACE_LEVEL_INFORMATION,
                TRACE_PARSING,
                "Input report %u is %u bytes",
                i, hidReportSizeEntry->InputReportSize);
        }
        if (hidReportSizeEntry->OutputReportSize) {
            PostProcessReportSize(&hidReportSizeEntry->OutputReportSize, FileContext->NumberedReports);

            TraceEvents(
                TRACE_LEVEL_INFORMATION,
                TRACE_PARSING,
                "Output report %u is %u bytes",
                i, hidReportSizeEntry->OutputReportSize);
        }
        if (hidReportSizeEntry->FeatureReportSize) {
            PostProcessReportSize(&hidReportSizeEntry->FeatureReportSize, FileContext->NumberedReports);

            TraceEvents(
                TRACE_LEVEL_INFORMATION,
                TRACE_PARSING,
                "Feature report %u is %u bytes",
                i, hidReportSizeEntry->FeatureReportSize);
        }
    }

    return STATUS_SUCCESS;
}

//
// Upon success, the caller must complete and free the returned request.
//
WDFOBJECT
TakeOutstandingRequestById(
    _In_ PFILE_CONTEXT FileContext,
    _In_ WINUHID_REQUEST_ID RequestId)
{
    ULONG i, count;

    WdfWaitLockAcquire(FileContext->RequestLock, NULL);

    //
    // Search for the request matching the ID
    //
    count = WdfCollectionGetCount(FileContext->OutstandingRequests);
    for (i = 0; i < count; i++) {
        WDFOBJECT op = WdfCollectionGetItem(FileContext->OutstandingRequests, i);

        if (OpGetContext(op)->RequestId == RequestId) {
            //
            // Dequeue and return the matching request
            //
            WdfCollectionRemoveItem(FileContext->OutstandingRequests, i);
            WdfWaitLockRelease(FileContext->RequestLock);
            return op;
        }
    }

    WdfWaitLockRelease(FileContext->RequestLock);

    return NULL;
}

//
// Fills an IOCTL_WINUHID_GET_NEXT_EVENT request with the first waiting operation, which
// moves to the outstanding requests. The caller holds RequestLock, and completes the
// request with the status returned and *BytesTransferred once it has released it.
//
// STATUS_PENDING: no operation is waiting, and the request is untouched. The caller parks
// it in GetNextEventIoctlQueue before releasing the lock, so that an operation queued
// after the lock is released finds it there (MoonlightWeb: parking it after the lock was
// released lost the wakeup of an operation queued in between, until a next operation came;
// a game whose synchronous write was that operation sent none, and froze).
//
NTSTATUS
FillNextEventLocked(
    _In_ PFILE_CONTEXT FileContext,
    _In_ WDFREQUEST Request,
    _Out_ PULONG_PTR BytesTransferred)
{
    NTSTATUS status;
    WDFOBJECT asyncOp;
    POPERATION_CONTEXT opContext;
    WDFMEMORY outputMem;
    WINUHID_EVENT eventHeader;
    ULONG_PTR bytesTransferred;

    bytesTransferred = 0;
    *BytesTransferred = 0;

    //
    // Get the most recent waiting operation to return (if any)
    //
    asyncOp = WdfCollectionGetFirstItem(FileContext->WaitingRequests);
    if (!asyncOp) {
        TraceEvents(
            TRACE_LEVEL_INFORMATION,
            TRACE_EVENT,
            "No requests ready for dispatching");
        return STATUS_PENDING;
    }
    opContext = OpGetContext(asyncOp);

    TraceEvents(
        TRACE_LEVEL_INFORMATION,
        TRACE_EVENT,
        "Dispatching waiting request %llx to user",
        opContext->RequestId);

    //
    // Get the caller's output buffer
    //
    status = WdfRequestRetrieveOutputMemory(Request, &outputMem);
    if (!NT_SUCCESS(status)) {
        TraceEvents(
            TRACE_LEVEL_ERROR,
            TRACE_EVENT,
            "Failed to get output buffer: %!STATUS!",
            status);
        return status;
    }

    //
    // Create the event we will return
    //
    eventHeader.Type = opContext->Type;
    eventHeader.RequestId = opContext->RequestId;
    eventHeader.ReportId = opContext->HidTransferPacket.reportId;
    if (WINUHID_EVENT_TYPE_IS_READ(opContext->Type)) {
        eventHeader.Read.DataLength = opContext->HidTransferPacket.reportBufferLen;
    }
    else {
        eventHeader.Write.DataLength = opContext->HidTransferPacket.reportBufferLen;
    }

    //
    // Copy the event header into it
    //
    status = WdfMemoryCopyFromBuffer(outputMem, 0, &eventHeader, sizeof(eventHeader));
    if (!NT_SUCCESS(status)) {
        TraceEvents(
            TRACE_LEVEL_ERROR,
            TRACE_EVENT,
            "Failed to copy event header to output buffer: %!STATUS!",
            status);
        return status;
    }
    bytesTransferred += sizeof(eventHeader);

    //
    // For writes, we also need to copy the data after the header
    //
    if (WINUHID_EVENT_TYPE_IS_WRITE(opContext->Type)) {
        status = WdfMemoryCopyFromBuffer(outputMem,
            FIELD_OFFSET(WINUHID_EVENT, Write.Data),
            opContext->HidTransferPacket.reportBuffer,
            opContext->HidTransferPacket.reportBufferLen);
        if (!NT_SUCCESS(status)) {
            TraceEvents(
                TRACE_LEVEL_ERROR,
                TRACE_EVENT,
                "Failed to copy event data to output buffer: %!STATUS!",
                status);
            return status;
        }

        bytesTransferred += opContext->HidTransferPacket.reportBufferLen;
    }

    //
    // Move the request into the outstanding requests collection
    //
    status = WdfCollectionAdd(FileContext->OutstandingRequests, asyncOp);
    if (!NT_SUCCESS(status)) {
        TraceEvents(
            TRACE_LEVEL_ERROR,
            TRACE_EVENT,
            "Failed to move event: %!STATUS!",
            status);
        return status;
    }
    WdfCollectionRemove(FileContext->WaitingRequests, asyncOp);

    *BytesTransferred = bytesTransferred;
    return status;
}

VOID
DispatchAsyncOperation(
    _In_ PVOID VhfClientContext,
    _In_opt_ VHFOPERATIONHANDLE VhfOperationHandle,
    _In_opt_ PHID_XFER_PACKET HidTransferPacket,
    _In_ WINUHID_EVENT_TYPE EventType)
{
    PFILE_CONTEXT fileContext = VhfClientContext;
    NTSTATUS status;
    WDF_OBJECT_ATTRIBUTES attribs;
    WDFOBJECT asyncOp;
    POPERATION_CONTEXT opContext;
    WDFREQUEST request;
    ULONG_PTR bytesTransferred;

    //
    // Create a WDF object to wrap this operation
    //
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attribs, OPERATION_CONTEXT);
    attribs.ParentObject = fileContext->FileObject;
    status = WdfObjectCreate(&attribs, &asyncOp);
    if (!NT_SUCCESS(status)) {
        TraceEvents(
            TRACE_LEVEL_ERROR,
            TRACE_EVENT,
            "Failed to create operation object: %!STATUS!",
            status);
        if (VhfOperationHandle) {
            status = VhfAsyncOperationComplete(VhfOperationHandle, STATUS_UNSUCCESSFUL);
            WDFVERIFY(NT_SUCCESS(status));
        }
        return;
    }

    opContext = OpGetContext(asyncOp);
    opContext->Type = EventType;

    if (!HidTransferPacket) {
        //
        // This is a ready-for-read pseudo-request with a fixed ID
        //
        opContext->RequestId = 0;
    }
    else {
        //
        // Always set the MSB for real requests to avoid matching a pseudo-request ID
        //
        opContext->RequestId = InterlockedIncrement(&fileContext->NextRequestId) | 0x80000000;
        opContext->Handle = VhfOperationHandle;
        opContext->HidTransferPacket = *HidTransferPacket;

        //
        // Windows 10's version of VHF has a bug where it can return a report buffer length
        // that is larger than the actual buffer it allocated. Fortunately it does actually
        // allocate a buffer of the maximum report size, so we can just clamp the length
        // value here as a workaround.
        //
        PHID_REPORT_SIZE reportSizeEntry = &fileContext->ReportSizes[opContext->HidTransferPacket.reportId];
        ULONG reportSize = 0;
        switch (EventType) {
        case WINUHID_EVENT_SET_FEATURE:
        case WINUHID_EVENT_GET_FEATURE:
            reportSize = reportSizeEntry->FeatureReportSize;
            break;
        case WINUHID_EVENT_WRITE_REPORT:
            reportSize = reportSizeEntry->OutputReportSize;
            break;
        case WINUHID_EVENT_READ_REPORT:
            reportSize = reportSizeEntry->InputReportSize;
            break;
        }

        //
        // We should know how long all reports should be. If not, something is seriously wrong.
        //
        if (reportSize == 0) {
            WDFVERIFY(reportSize != 0);
            TraceEvents(
                TRACE_LEVEL_ERROR,
                TRACE_EVENT,
                "Request has unknown report ID: %u",
                opContext->HidTransferPacket.reportId);
            if (VhfOperationHandle) {
                status = VhfAsyncOperationComplete(VhfOperationHandle, STATUS_UNSUCCESSFUL);
                WDFVERIFY(NT_SUCCESS(status));
            }
            WdfObjectDelete(asyncOp);
            return;
        }

        //
        // Clamp the length to the size of this report
        //
        if (reportSize < opContext->HidTransferPacket.reportBufferLen) {
            opContext->HidTransferPacket.reportBufferLen = reportSize;
        }
    }

    //
    // Lock the request collection for queuing
    //
    WdfWaitLockAcquire(fileContext->RequestLock, NULL);
    if (!fileContext->VhfHandle) {
        //
        // The device is being closed
        //
        TraceEvents(
            TRACE_LEVEL_WARNING,
            TRACE_EVENT,
            "Received operation callback during teardown");
        WdfWaitLockRelease(fileContext->RequestLock);
        if (VhfOperationHandle) {
            status = VhfAsyncOperationComplete(VhfOperationHandle, STATUS_TOO_LATE);
            WDFVERIFY(NT_SUCCESS(status));
        }
        WdfObjectDelete(asyncOp);
        return;
    }

    //
    // Queue this operation to the file's waiting requests collection
    //
    status = WdfCollectionAdd(fileContext->WaitingRequests, asyncOp);
    if (!NT_SUCCESS(status)) {
        WdfWaitLockRelease(fileContext->RequestLock);
        TraceEvents(
            TRACE_LEVEL_ERROR,
            TRACE_EVENT,
            "Failed to add operation to waiting requests: %!STATUS!",
            status);
        if (VhfOperationHandle) {
            status = VhfAsyncOperationComplete(VhfOperationHandle, status);
            WDFVERIFY(NT_SUCCESS(status));
        }
        WdfObjectDelete(asyncOp);
        return;
    }

    //
    // If an IOCTL_WINUHID_GET_NEXT_EVENT request is parked, service it now, still under the
    // lock: the IOCTL parks itself under it too, so either it found this operation waiting or
    // this finds it parked. A parked request means no operation was waiting before this one.
    //
    status = WdfIoQueueRetrieveRequestByFileObject(
        fileContext->DeviceContext->GetNextEventIoctlQueue, fileContext->FileObject, &request);
    if (status != STATUS_SUCCESS) {
        WdfWaitLockRelease(fileContext->RequestLock);
        return;
    }
    status = FillNextEventLocked(fileContext, request, &bytesTransferred);
    if (status == STATUS_PENDING) {
        //
        // Cannot happen with the operation just queued under the lock; parked again
        // rather than completed with a status a request may not complete with.
        //
        WDFVERIFY(status != STATUS_PENDING);
        status = WdfRequestRequeue(request);
        WdfWaitLockRelease(fileContext->RequestLock);
        if (!NT_SUCCESS(status)) {
            WdfRequestComplete(request, status);
        }
        return;
    }
    WdfWaitLockRelease(fileContext->RequestLock);
    WdfRequestCompleteWithInformation(request, status, NT_SUCCESS(status) ? bytesTransferred : 0);
}

_Function_class_(EVT_VHF_ASYNC_OPERATION)
VOID
WinUHidEvtVhfAsyncOperationGetFeature(
    _In_ PVOID VhfClientContext,
    _In_ VHFOPERATIONHANDLE VhfOperationHandle,
    _In_opt_ PVOID VhfOperationContext,
    _In_ PHID_XFER_PACKET HidTransferPacket
)
{
    UNREFERENCED_PARAMETER(VhfOperationContext);

    DispatchAsyncOperation(VhfClientContext, VhfOperationHandle, HidTransferPacket, WINUHID_EVENT_GET_FEATURE);
}

_Function_class_(EVT_VHF_ASYNC_OPERATION)
VOID
WinUHidEvtVhfAsyncOperationSetFeature(
    _In_ PVOID VhfClientContext,
    _In_ VHFOPERATIONHANDLE VhfOperationHandle,
    _In_opt_ PVOID VhfOperationContext,
    _In_ PHID_XFER_PACKET HidTransferPacket
)
{
    UNREFERENCED_PARAMETER(VhfOperationContext);

    DispatchAsyncOperation(VhfClientContext, VhfOperationHandle, HidTransferPacket, WINUHID_EVENT_SET_FEATURE);
}

_Function_class_(EVT_VHF_ASYNC_OPERATION)
VOID
WinUHidEvtVhfAsyncOperationWriteReport(
    _In_ PVOID VhfClientContext,
    _In_ VHFOPERATIONHANDLE VhfOperationHandle,
    _In_opt_ PVOID VhfOperationContext,
    _In_ PHID_XFER_PACKET HidTransferPacket
)
{
    UNREFERENCED_PARAMETER(VhfOperationContext);

    DispatchAsyncOperation(VhfClientContext, VhfOperationHandle, HidTransferPacket, WINUHID_EVENT_WRITE_REPORT);
}

_Function_class_(EVT_VHF_ASYNC_OPERATION)
VOID
WinUHidEvtVhfAsyncOperationGetInputReport(
    _In_ PVOID VhfClientContext,
    _In_ VHFOPERATIONHANDLE VhfOperationHandle,
    _In_opt_ PVOID VhfOperationContext,
    _In_ PHID_XFER_PACKET HidTransferPacket
)
{
    UNREFERENCED_PARAMETER(VhfOperationContext);

    DispatchAsyncOperation(VhfClientContext, VhfOperationHandle, HidTransferPacket, WINUHID_EVENT_READ_REPORT);
}

_Function_class_(EVT_VHF_READY_FOR_NEXT_READ_REPORT)
VOID
WinUHidEvtVhfReadyForNextReadReport(
    _In_ PVOID VhfClientContext
)
{
    //
    // This is a special case of WINUHID_EVENT_READ_REPORT which allows the caller to
    // return any input report they want. We turn it their completion IOCTL into a
    // call into VhfReadReportSubmit() to abstract away this API detail.
    //
    DispatchAsyncOperation(VhfClientContext, NULL, NULL, WINUHID_EVENT_READ_REPORT);
}

VOID
WinUHidEvtIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode
)
{
    WDFFILEOBJECT fileObject = WdfRequestGetFileObject(Request);
    PFILE_CONTEXT fileContext = FileGetContext(fileObject);
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Queue);
    UNREFERENCED_PARAMETER(OutputBufferLength);

    switch (IoControlCode)
    {
    case IOCTL_WINUHID_GET_INTERFACE_VERSION:
    {
        PULONG outputBuffer;

        //
        // Get the output buffer
        //
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*outputBuffer), &outputBuffer, NULL);
        if (!NT_SUCCESS(status)) {
            WdfRequestComplete(Request, status);
            break;
        }

        //
        // Write our compiled interface version back to the client
        //
        *outputBuffer = WINUHID_VERSION_LATEST;
        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(*outputBuffer));
        break;
    }

    case IOCTL_WINUHID_SET_DEVICE_INFO:
    {
        PWINUHID_DEVICE_INFO info;

        //
        // Device info can only be set once before device creation
        //
        if (fileContext->DeviceInfoSet || fileContext->VhfHandle) {
            WdfRequestComplete(Request, STATUS_INVALID_DEVICE_STATE);
            break;
        }

        //
        // Make sure the input buffer is large enough
        //
        status = WdfRequestRetrieveInputBuffer(Request, sizeof(*info), &info, NULL);
        if (!NT_SUCCESS(status)) {
            WdfRequestComplete(Request, status);
            break;
        }

        //
        // Make sure SupportedEvents only contains flags that we know about
        //
        if (info->SupportedEvents & ~(WINUHID_EVENT_GET_FEATURE |
                                      WINUHID_EVENT_SET_FEATURE |
                                      WINUHID_EVENT_WRITE_REPORT |
                                      WINUHID_EVENT_READ_REPORT)) {
            WdfRequestComplete(Request, STATUS_NOT_SUPPORTED);
            break;
        }

        fileContext->VhfConfig.VhfClientContext = fileContext;
        fileContext->VhfConfig.VendorID = info->VendorID;
        fileContext->VhfConfig.ProductID = info->ProductID;
        fileContext->VhfConfig.VersionNumber = info->VersionNumber;
        fileContext->VhfConfig.ContainerID = info->ContainerId;

        //
        // Register callbacks for any events the user wanted to handle
        //
        if (info->SupportedEvents & WINUHID_EVENT_GET_FEATURE) {
            fileContext->VhfConfig.EvtVhfAsyncOperationGetFeature = WinUHidEvtVhfAsyncOperationGetFeature;
        }
        if (info->SupportedEvents & WINUHID_EVENT_SET_FEATURE) {
            fileContext->VhfConfig.EvtVhfAsyncOperationSetFeature = WinUHidEvtVhfAsyncOperationSetFeature;
        }
        if (info->SupportedEvents & WINUHID_EVENT_WRITE_REPORT) {
            fileContext->VhfConfig.EvtVhfAsyncOperationWriteReport = WinUHidEvtVhfAsyncOperationWriteReport;
        }
        if (info->SupportedEvents & WINUHID_EVENT_READ_REPORT) {
            fileContext->VhfConfig.EvtVhfReadyForNextReadReport = WinUHidEvtVhfReadyForNextReadReport;
            fileContext->VhfConfig.EvtVhfAsyncOperationGetInputReport = WinUHidEvtVhfAsyncOperationGetInputReport;
        }

        fileContext->DeviceInfoSet = TRUE;
        WdfRequestComplete(Request, STATUS_SUCCESS);
        break;
    }

    case IOCTL_WINUHID_SET_REPORT_DESCRIPTOR:
    {
        PVOID inputBuffer;

        //
        // Report descriptor can only be set once before device creation
        //
        if (fileContext->VhfConfig.ReportDescriptor || fileContext->VhfHandle) {
            WdfRequestComplete(Request, STATUS_INVALID_DEVICE_STATE);
            break;
        }

        //
        // Get the input buffer
        //
        status = WdfRequestRetrieveInputBuffer(Request, 1, &inputBuffer, NULL);
        if (!NT_SUCCESS(status)) {
            WdfRequestComplete(Request, status);
            break;
        }
        else if (InputBufferLength > MAXUINT16) {
            //
            // The length must be small enough to fit in the USHORT in the VHF_CONFIG
            //
            WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
            break;
        }

        //
        // Copy the report descriptor into our file context
        //
        WDF_OBJECT_ATTRIBUTES attribs;
        WDF_OBJECT_ATTRIBUTES_INIT(&attribs);
        attribs.ParentObject = fileObject;
        status = WdfMemoryCreate(&attribs, PagedPool, REPORT_DESC_TAG, InputBufferLength, &fileContext->ReportDescriptorMem, NULL);
        if (!NT_SUCCESS(status)) {
            WdfRequestComplete(Request, status);
            break;
        }

        status = WdfMemoryCopyFromBuffer(fileContext->ReportDescriptorMem, 0, inputBuffer, InputBufferLength);
        if (!NT_SUCCESS(status)) {
            WdfObjectDelete(fileContext->ReportDescriptorMem);
            fileContext->ReportDescriptorMem = NULL;
            WdfRequestComplete(Request, status);
            break;
        }

        fileContext->VhfConfig.ReportDescriptor = WdfMemoryGetBuffer(fileContext->ReportDescriptorMem, NULL);
        fileContext->VhfConfig.ReportDescriptorLength = (USHORT)InputBufferLength; // Truncation checked above

        //
        // Parse the report descriptor to determine the size of each report and whether numbered reports are used
        //
        status = ParseReportDescriptor(fileContext);
#ifdef MOONLIGHTWEB_VHID
        //
        // MoonlightWeb: only game devices, by the engine's own validate(), and
        // the host's own mouse and keyboard, compared byte for byte
        //
        if (NT_SUCCESS(status) &&
            !MwHidDescriptorAllowed(fileContext->VhfConfig.ReportDescriptor, fileContext->VhfConfig.ReportDescriptorLength)) {
            status = STATUS_ACCESS_DENIED;
        }
#endif
        if (!NT_SUCCESS(status)) {
            fileContext->VhfConfig.ReportDescriptor = NULL;
            fileContext->VhfConfig.ReportDescriptorLength = 0;
            WdfObjectDelete(fileContext->ReportDescriptorMem);
            fileContext->ReportDescriptorMem = NULL;
            WdfRequestComplete(Request, status);
            break;
        }

        WdfRequestComplete(Request, status);
        break;
    }

    case IOCTL_WINUHID_SET_INSTANCE_ID:
    {
        PVOID inputBuffer;

        //
        // Instance ID can only be set once before device creation
        //
        if (fileContext->VhfConfig.InstanceID || fileContext->VhfHandle) {
            WdfRequestComplete(Request, STATUS_INVALID_DEVICE_STATE);
            break;
        }

        //
        // Get the input buffer
        //
        status = WdfRequestRetrieveInputBuffer(Request, sizeof(WCHAR), &inputBuffer, NULL);
        if (!NT_SUCCESS(status)) {
            WdfRequestComplete(Request, status);
            break;
        }
        else if (InputBufferLength > MAXUINT16) {
            //
            // The length must be small enough to fit in the USHORT in the VHF_CONFIG
            //
            WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
            break;
        }
        else if (InputBufferLength % sizeof(WCHAR) != 0) {
            //
            // The input buffer must be a multiple of a unicode character in length
            //
            WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
            break;
        }

        //
        // Copy the instance ID into our file context
        //
        WDF_OBJECT_ATTRIBUTES attribs;
        WDF_OBJECT_ATTRIBUTES_INIT(&attribs);
        attribs.ParentObject = fileObject;
        status = WdfMemoryCreate(&attribs, PagedPool, INSTANCE_ID_TAG, InputBufferLength, &fileContext->InstanceIdMem, NULL);
        if (!NT_SUCCESS(status)) {
            WdfRequestComplete(Request, status);
            break;
        }

        status = WdfMemoryCopyFromBuffer(fileContext->InstanceIdMem, 0, inputBuffer, InputBufferLength);
        if (!NT_SUCCESS(status)) {
            WdfObjectDelete(fileContext->InstanceIdMem);
            fileContext->InstanceIdMem = NULL;
            WdfRequestComplete(Request, status);
            break;
        }

        fileContext->VhfConfig.InstanceID = WdfMemoryGetBuffer(fileContext->InstanceIdMem, NULL);
        fileContext->VhfConfig.InstanceIDLength = (USHORT)InputBufferLength; // Truncation checked above
        WdfRequestComplete(Request, status);
        break;
    }

    case IOCTL_WINUHID_SET_HARDWARE_IDS:
    {
        PWCHAR inputBuffer;

        //
        // Hardware IDs can only be set once
        //
        if (fileContext->VhfConfig.HardwareIDs || fileContext->VhfHandle) {
            WdfRequestComplete(Request, STATUS_INVALID_DEVICE_STATE);
            break;
        }

        //
        // Get the input buffer of at least 2 characters (the final terminating null pair)
        //
        status = WdfRequestRetrieveInputBuffer(Request, 2*sizeof(WCHAR), &inputBuffer, NULL);
        if (!NT_SUCCESS(status)) {
            WdfRequestComplete(Request, status);
            break;
        }
        else if (InputBufferLength > MAXUINT16) {
            //
            // The length must be small enough to fit in the USHORT in the VHF_CONFIG
            //
            WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
            break;
        }
        else if (InputBufferLength % sizeof(WCHAR) != 0) {
            //
            // The input buffer must be a multiple of a unicode character in length
            //
            WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
            break;
        }
        else if (inputBuffer[InputBufferLength / sizeof(WCHAR) - 1] != 0 ||
                 inputBuffer[InputBufferLength / sizeof(WCHAR) - 2] != 0)
        {
            //
            // The input buffer must terminate with 2 null characters
            //
            WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
            break;
        }

        WDF_OBJECT_ATTRIBUTES attribs;
        WDF_OBJECT_ATTRIBUTES_INIT(&attribs);
        attribs.ParentObject = fileObject;
        status = WdfMemoryCreate(&attribs, PagedPool, HARDWARE_IDS_TAG, InputBufferLength, &fileContext->HardwareIdsMem, NULL);
        if (!NT_SUCCESS(status)) {
            WdfRequestComplete(Request, status);
            break;
        }

        status = WdfMemoryCopyFromBuffer(fileContext->HardwareIdsMem, 0, inputBuffer, InputBufferLength);
        if (!NT_SUCCESS(status)) {
            WdfObjectDelete(fileContext->HardwareIdsMem);
            fileContext->HardwareIdsMem = NULL;
            WdfRequestComplete(Request, status);
            break;
        }

        fileContext->VhfConfig.HardwareIDs = WdfMemoryGetBuffer(fileContext->HardwareIdsMem, NULL);
        fileContext->VhfConfig.HardwareIDsLength = (USHORT)InputBufferLength; // Truncation checked above
        WdfRequestComplete(Request, status);
        break;
    }

    case IOCTL_WINUHID_CREATE_DEVICE:
    {
        if (fileContext->VhfHandle || // Already created
            fileContext->VhfConfig.ReportDescriptorLength == 0 || // No report descriptor set
            !fileContext->DeviceInfoSet) { // No device info set
            WdfRequestComplete(Request, STATUS_INVALID_DEVICE_STATE);
            break;
        }

        status = VhfCreate(&fileContext->VhfConfig, &fileContext->VhfHandle);
        WdfRequestComplete(Request, status);
        break;
    }

    case IOCTL_WINUHID_START_DEVICE:
    {
        //
        // The device must have been created first
        //
        if (!fileContext->VhfHandle) {
            WdfRequestComplete(Request, STATUS_INVALID_DEVICE_STATE);
            break;
        }

        status = VhfStart(fileContext->VhfHandle);
        WdfRequestComplete(Request, status);
        break;
    }

    case IOCTL_WINUHID_GET_NEXT_EVENT:
    {
        ULONG_PTR bytesTransferred;

        //
        // The device must have been created first
        //
        if (!fileContext->VhfHandle) {
            WdfRequestComplete(Request, STATUS_INVALID_DEVICE_STATE);
            break;
        }

        //
        // Try to service a waiting request first
        //
        WdfWaitLockAcquire(fileContext->RequestLock, NULL);
        status = FillNextEventLocked(fileContext, Request, &bytesTransferred);
        if (status == STATUS_PENDING) {
            //
            // There wasn't a waiting request, so queue this for manual completion later,
            // before the lock goes: the next operation queued finds it (see FillNextEventLocked)
            //
            status = WdfRequestForwardToIoQueue(Request, fileContext->DeviceContext->GetNextEventIoctlQueue);
            WdfWaitLockRelease(fileContext->RequestLock);

            //
            // If requeuing failed, we have no option but to fail the request
            //
            if (!NT_SUCCESS(status)) {
                WDFVERIFY(NT_SUCCESS(status));
                WdfRequestComplete(Request, status);
            }
            break;
        }
        WdfWaitLockRelease(fileContext->RequestLock);
        WdfRequestCompleteWithInformation(Request, status, NT_SUCCESS(status) ? bytesTransferred : 0);
        break;
    }

#ifdef MOONLIGHTWEB_VHID
    case IOCTL_MOONLIGHTWEB_VHID_GET_FIXES:
    {
        PULONG outputBuffer;

        //
        // MoonlightWeb: the fixes this driver has, so the client knows what it may rely on
        //
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*outputBuffer), &outputBuffer, NULL);
        if (!NT_SUCCESS(status)) {
            WdfRequestComplete(Request, status);
            break;
        }
        *outputBuffer = MOONLIGHTWEB_VHID_FIXES;
        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(*outputBuffer));
        break;
    }
#endif

    case IOCTL_WINUHID_COMPLETE_WRITE_EVENT:
    {
        PWINUHID_WRITE_EVENT_COMPLETE writeComplete;
        WDFOBJECT op;
        POPERATION_CONTEXT opContext;

        //
        // The device must have been created first
        //
        if (!fileContext->VhfHandle) {
            WdfRequestComplete(Request, STATUS_INVALID_DEVICE_STATE);
            break;
        }

        //
        // Make sure the input buffer is large enough
        //
        status = WdfRequestRetrieveInputBuffer(Request, sizeof(*writeComplete), &writeComplete, NULL);
        if (!NT_SUCCESS(status)) {
            WdfRequestComplete(Request, status);
            break;
        }

        //
        // Find and dequeue the outstanding request being completed
        //
        op = TakeOutstandingRequestById(fileContext, writeComplete->RequestId);
        if (op == NULL) {
            WdfRequestComplete(Request, STATUS_NOT_FOUND);
            break;
        }
        opContext = OpGetContext(op);

        //
        // Complete and delete the request
        //
        status = VhfAsyncOperationComplete(opContext->Handle, writeComplete->Status);
        WDFVERIFY(NT_SUCCESS(status));
        WdfObjectDelete(op);
        WdfRequestComplete(Request, status);
        break;
    }

    case IOCTL_WINUHID_COMPLETE_READ_EVENT:
    {
        PWINUHID_READ_EVENT_COMPLETE readComplete;
        WDFOBJECT op;
        POPERATION_CONTEXT opContext;

        //
        // The device must have been created first
        //
        if (!fileContext->VhfHandle) {
            WdfRequestComplete(Request, STATUS_INVALID_DEVICE_STATE);
            break;
        }

        //
        // Make sure the input buffer is large enough
        //
        status = WdfRequestRetrieveInputBuffer(Request, sizeof(*readComplete), &readComplete, NULL);
        if (!NT_SUCCESS(status)) {
            WdfRequestComplete(Request, status);
            break;
        }
        else if (InputBufferLength < FIELD_OFFSET(WINUHID_READ_EVENT_COMPLETE, Data) + readComplete->DataLength) {
            WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL);
            break;
        }

        //
        // Find and dequeue the outstanding request being completed
        //
        op = TakeOutstandingRequestById(fileContext, readComplete->RequestId);
        if (op == NULL) {
            WdfRequestComplete(Request, STATUS_NOT_FOUND);
            break;
        }
        opContext = OpGetContext(op);

        //
        // NB: Past this point we must always call complete and delete op!
        //

        if (readComplete->RequestId == 0) {
            //
            // This is a pseudo-operation that just indicates that we should call
            // VhfReadReportSubmit() with whatever the caller provided.
            //
            if (NT_SUCCESS(readComplete->Status) && readComplete->DataLength != 0) {
                //
                // The user provided data for us to complete the request now
                //
                HID_XFER_PACKET xferPkt;
                xferPkt.reportId = fileContext->NumberedReports ? readComplete->Data[0] : 0;
                xferPkt.reportBuffer = readComplete->Data;
                xferPkt.reportBufferLen = readComplete->DataLength;
                status = VhfReadReportSubmit(fileContext->VhfHandle, &xferPkt);
                WDFVERIFY(NT_SUCCESS(status));
            }
            else if (!NT_SUCCESS(readComplete->Status)) {
                //
                // Issue a synthetic ready-for-read callback to enqueue another
                // event telling the user that we need another input report.
                //
                WinUHidEvtVhfReadyForNextReadReport(fileContext);
                status = STATUS_SUCCESS;
            }
            else {
                //
                // A zero byte read completion means the user acknowledges the
                // request for data and will complete it at a later time via
                // a write IRP.
                //
                status = STATUS_SUCCESS;
            }
            WdfObjectDelete(op);
            WdfRequestComplete(Request, status);
            break;
        }

        //
        // If the user failed the request, just complete it with an error
        //
        if (!NT_SUCCESS(readComplete->Status)) {
            status = VhfAsyncOperationComplete(opContext->Handle, readComplete->Status);
            WDFVERIFY(NT_SUCCESS(status));
            WdfObjectDelete(op);
            WdfRequestComplete(Request, status);
            break;
        }

        //
        // Copy the data into the transfer buffer
        //
        RtlCopyMemory(
            opContext->HidTransferPacket.reportBuffer,
            &readComplete->Data[0],
            min(readComplete->DataLength, opContext->HidTransferPacket.reportBufferLen));

        //
        // If the report is not fully populated, pad the remaining part with zeros
        //
        if (readComplete->DataLength < opContext->HidTransferPacket.reportBufferLen) {
            RtlZeroMemory(
                &opContext->HidTransferPacket.reportBuffer[readComplete->DataLength],
                opContext->HidTransferPacket.reportBufferLen - readComplete->DataLength);
        }

        //
        // Complete and delete the request
        //
        status = VhfAsyncOperationComplete(opContext->Handle, readComplete->Status);
        WDFVERIFY(NT_SUCCESS(status));
        WdfObjectDelete(op);
        WdfRequestComplete(Request, status);
        break;
    }

    default:
        WDFVERIFY(FALSE);
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
        break;
    }
}

void WinUHidEvtDeviceFileCreate(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ WDFFILEOBJECT FileObject
)
{
    PDEVICE_CONTEXT deviceContext = DeviceGetContext(Device);
    PFILE_CONTEXT fileContext = FileGetContext(FileObject);
    NTSTATUS status;
    WDF_OBJECT_ATTRIBUTES attribs;
    WDF_IO_TARGET_OPEN_PARAMS openParams;

    RtlZeroMemory(fileContext, sizeof(*fileContext));
    fileContext->FileObject = FileObject;
    fileContext->DeviceContext = deviceContext;

    //
    // Open the underlying VHF device
    //
    WDF_OBJECT_ATTRIBUTES_INIT(&attribs);
    attribs.ParentObject = FileObject;
    status = WdfIoTargetCreate(Device, &attribs, &fileContext->VhfIoTarget);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }

    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_FILE(&openParams, NULL);
    status = WdfIoTargetOpen(fileContext->VhfIoTarget, &openParams);
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(fileContext->VhfIoTarget);
        fileContext->VhfIoTarget = NULL;
        goto Exit;
    }

    //
    // Initialize the VHF config with our VHF I/O target handle
    //
    VHF_CONFIG_INIT(&fileContext->VhfConfig, WdfIoTargetWdmGetTargetFileHandle(fileContext->VhfIoTarget), 0, NULL);

    status = WdfWaitLockCreate(&attribs, &fileContext->RequestLock);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }

    status = WdfCollectionCreate(&attribs, &fileContext->WaitingRequests);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }

    status = WdfCollectionCreate(&attribs, &fileContext->OutstandingRequests);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }

Exit:
    WdfRequestComplete(Request, status);
}

void WinUHidEvtDeviceFileClose(
    _In_ WDFFILEOBJECT FileObject
)
{
    PFILE_CONTEXT fileContext = FileGetContext(FileObject);
    VHFHANDLE vhfHandle;
    WDFOBJECT op;

    //
    // Lock the queues and start rundown
    //
    WdfWaitLockAcquire(fileContext->RequestLock, NULL);

    //
    // Clear the VHF handle so no further queuing can occur
    //
    vhfHandle = fileContext->VhfHandle;
    fileContext->VhfHandle = NULL;

    //
    // Rundown both queues and complete all requests
    //

    while ((op = WdfCollectionGetFirstItem(fileContext->WaitingRequests))) {
        POPERATION_CONTEXT opContext = OpGetContext(op);

        if (opContext->RequestId != 0) {
            VhfAsyncOperationComplete(opContext->Handle, STATUS_CANCELLED);
        }

        WdfCollectionRemove(fileContext->WaitingRequests, op);
        WdfObjectDelete(op);
    }

    while ((op = WdfCollectionGetFirstItem(fileContext->OutstandingRequests))) {
        POPERATION_CONTEXT opContext = OpGetContext(op);

        if (opContext->RequestId != 0) {
            VhfAsyncOperationComplete(opContext->Handle, STATUS_CANCELLED);
        }

        WdfCollectionRemove(fileContext->OutstandingRequests, op);
        WdfObjectDelete(op);
    }

    WdfWaitLockRelease(fileContext->RequestLock);

    if (vhfHandle) {
        VhfDelete(vhfHandle, TRUE);
    }

    if (fileContext->VhfIoTarget) {
        WdfObjectDelete(fileContext->VhfIoTarget);
    }
}

void WinUHidEvtIoWrite(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t Length
)
{
    PFILE_CONTEXT fileContext = FileGetContext(WdfRequestGetFileObject(Request));
    NTSTATUS status;
    PUCHAR inputBuffer;
    HID_XFER_PACKET xferPkt;

    UNREFERENCED_PARAMETER(Queue);

    //
    // It's only valid to write if the device is created
    //
    if (!fileContext->VhfHandle) {
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_STATE);
        return;
    }

    //
    // Get the input buffer which must be at least 1 byte
    //
    status = WdfRequestRetrieveInputBuffer(Request, 1, &inputBuffer, NULL);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, status);
        return;
    }
    else if (Length > MAXUINT32) {
        //
        // Length must be small enough to fit in the HID_XFER_PACKET
        //
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return;
    }

    //
    // Submit the report to VHF
    //
    xferPkt.reportId = fileContext->NumberedReports ? inputBuffer[0] : 0;
    xferPkt.reportBufferLen = (ULONG)Length;
    xferPkt.reportBuffer = inputBuffer;
    status = VhfReadReportSubmit(fileContext->VhfHandle, &xferPkt);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, status);
        return;
    }

    WdfRequestCompleteWithInformation(Request, status, Length);
}

NTSTATUS
WinUHidEvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
)
{
    WDF_OBJECT_ATTRIBUTES objectAttributes;
    WDF_FILEOBJECT_CONFIG fileObjectConfig;
    PDEVICE_CONTEXT deviceContext;
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDFQUEUE defaultQueue;
    WDFDEVICE device;
    NTSTATUS status;
    DECLARE_CONST_UNICODE_STRING(ntDevicePath, WINUHID_NT_PATH);

    UNREFERENCED_PARAMETER(Driver);

    //
    // We are an upper filter for VHF (though we act more like a function driver)
    //
    WdfFdoInitSetFilter(DeviceInit);

    //
    // Register our file object callbacks
    //
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&objectAttributes, FILE_CONTEXT);
    objectAttributes.SynchronizationScope = WdfSynchronizationScopeNone;
    WDF_FILEOBJECT_CONFIG_INIT(&fileObjectConfig, WinUHidEvtDeviceFileCreate, WinUHidEvtDeviceFileClose, WDF_NO_EVENT_CALLBACK);
    fileObjectConfig.AutoForwardCleanupClose = WdfFalse;
    WdfDeviceInitSetFileObjectConfig(DeviceInit, &fileObjectConfig, &objectAttributes);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&objectAttributes, DEVICE_CONTEXT);
    status = WdfDeviceCreate(&DeviceInit, &objectAttributes, &device);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }

    deviceContext = DeviceGetContext(device);
    deviceContext->Device = device;

    //
    // Configure the default queue
    //
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.PowerManaged = WdfFalse;
    queueConfig.EvtIoDeviceControl = WinUHidEvtIoDeviceControl;
    queueConfig.EvtIoWrite = WinUHidEvtIoWrite;
    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &defaultQueue);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }

    //
    // Configure a manual dispatch queue for IOCTL_WINUHID_GET_NEXT_EVENT.
    // We will place requests in here if we cannot immediately satisfy them.
    //
    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);
    queueConfig.PowerManaged = WdfFalse;
    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &deviceContext->GetNextEventIoctlQueue);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }

    //
    // Create a symbolic link to the known path
    //
    status = WdfDeviceCreateSymbolicLink(device, &ntDevicePath);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }

Exit:
    return status;
}

VOID
WinUHidEvtDriverContextCleanup(
    _In_ WDFOBJECT DriverObject
)
{
    UNREFERENCED_PARAMETER(DriverObject);

    //
    // Stop WPP Tracing
    //
#if UMDF_VERSION_MAJOR == 2 && UMDF_VERSION_MINOR == 0
    WPP_CLEANUP();
#else
    WPP_CLEANUP(WdfDriverWdmGetDriverObject((WDFDRIVER)DriverObject));
#endif
}

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath
)
{
    WDF_DRIVER_CONFIG config;
    NTSTATUS status;
    WDF_OBJECT_ATTRIBUTES attributes;

    //
    // Initialize WPP Tracing
    //
#if UMDF_VERSION_MAJOR == 2 && UMDF_VERSION_MINOR == 0
    WPP_INIT_TRACING(MYDRIVER_TRACING_ID);
#else
    WPP_INIT_TRACING(DriverObject, RegistryPath);
#endif

    //
    // Register a cleanup callback so that we can call WPP_CLEANUP when
    // the framework driver object is deleted during driver unload.
    //
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.EvtCleanupCallback = WinUHidEvtDriverContextCleanup;

    WDF_DRIVER_CONFIG_INIT(&config, WinUHidEvtDeviceAdd);

    status = WdfDriverCreate(DriverObject,
        RegistryPath,
        &attributes,
        &config,
        WDF_NO_HANDLE
    );

    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER, "WdfDriverCreate failed %!STATUS!", status);
#if UMDF_VERSION_MAJOR == 2 && UMDF_VERSION_MINOR == 0
        WPP_CLEANUP();
#else
        WPP_CLEANUP(DriverObject);
#endif
        return status;
    }

    return status;
}