/*++

Module Name:

    queue.c

Abstract:

    This file contains the queue entry points and callbacks.

Environment:

    Kernel-mode Driver Framework

--*/

#include <ntifs.h>
#include <usb.h>
#include <usbioctl.h>
#include <Ntstrsafe.h>
#include "driver.h"
#include "queue.tmh"

#ifdef ALLOC_PRAGMA
#pragma alloc_text (PAGE, xusbfixerQueueInitialize)
#endif

static EVT_WDF_REQUEST_COMPLETION_ROUTINE UpperCompletionRoutine;
static EVT_WDF_REQUEST_COMPLETION_ROUTINE LowerInternalCompletionRoutine;


#ifdef DBG

static
char*
printHex(
    _Out_ char* Dest,
    _In_ size_t DestNumBytes,
    _In_opt_ UINT8* Data,
    _In_ size_t DataNumBytes
)
{
    char* hex = Dest;
    size_t hexLen = DestNumBytes;
    char* rem = NULL;
    size_t remLen = 0;

    if (!Data || DataNumBytes == 0) {
        if (Dest && DestNumBytes >= sizeof(char)) {
            Dest[0] = '\0';
        }

        return Dest;
    }

    for (size_t i = 0; i < DataNumBytes; i++) {
        if (!NT_SUCCESS(RtlStringCbPrintfExA(hex, hexLen, &rem, &remLen, 0, "%02X ", Data[i]))) {
            break;
        }

        hex = rem;
        hexLen = remLen;
    }

    return Dest;
}

#endif // DBG

NTSTATUS
xusbfixerQueueInitialize(
    _In_ WDFDEVICE Device
)
/*++

Routine Description:

     The I/O dispatch callbacks for the frameworks device object
     are configured in this function.

     A default I/O Queue and possibly an input wait queue for
     IOCTL 0x8000E3AC requests are configured for parallel request
     processing.

Arguments:

    Device - Handle to a framework device object.

Return Value:

    VOID

--*/
{
    WDFQUEUE queue;
    NTSTATUS status;
    WDF_IO_QUEUE_CONFIG queueConfig;
    PDEVICE_CONTEXT deviceContext;

    PAGED_CODE();

    //
    // Configure a default queue so that requests that are not
    // configure-fowarded using WdfDeviceConfigureRequestDispatching to goto
    // other queues get dispatched here.
    //
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);

    deviceContext = DeviceGetContext(Device);

    if (deviceContext->Upper) {
        queueConfig.EvtIoDeviceControl = xusbfixerEvtIoDeviceControl;
    }
    else {
        queueConfig.EvtIoInternalDeviceControl = xusbfixerEvtIoInternalDeviceControl;
    }

    status = WdfIoQueueCreate(
        Device,
        &queueConfig,
        WDF_NO_OBJECT_ATTRIBUTES,
        &queue
    );

    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_QUEUE, "%!FUNC! WdfIoQueueCreate failed %!STATUS!", status);
        return status;
    }

    if (deviceContext->Upper && deviceContext->LowerDeviceObject) {
        WDF_IO_QUEUE_CONFIG inputWaitQueueConfig;
        WDF_IO_QUEUE_CONFIG_INIT(&inputWaitQueueConfig, WdfIoQueueDispatchParallel);
        inputWaitQueueConfig.EvtIoDeviceControl = xusbfixerDispatchWaitForInput;

        status = WdfIoQueueCreate(
            Device,
            &inputWaitQueueConfig,
            WDF_NO_OBJECT_ATTRIBUTES,
            &deviceContext->InputWaitQueue
        );

        if (NT_SUCCESS(status)) {
            PDEVICE_CONTEXT lowerContext = deviceContext->LowerDeviceObject->DeviceExtension;
            lowerContext->InputWaitQueue = deviceContext->InputWaitQueue;
            WdfObjectReference(lowerContext->InputWaitQueue);

            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_QUEUE, "%!FUNC! created input queue 0x%p",
                        deviceContext->InputWaitQueue);
        }
        else {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_QUEUE, "%!FUNC! input wait WdfIoQueueCreate failed %!STATUS!", status);
        }
    }

    return status;
}

VOID
xusbfixerDispatchPassThrough(
    _In_ WDFREQUEST Request,
    _In_ WDFIOTARGET Target
)
/*++
Routine Description:

    Passes a request on to the lower driver.

--*/
{
    WDF_REQUEST_SEND_OPTIONS options;
    BOOLEAN ret;
    NTSTATUS status = STATUS_SUCCESS;

    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET);

    ret = WdfRequestSend(Request, Target, &options);

    if (ret == FALSE) {
        status = WdfRequestGetStatus(Request);
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_QUEUE, "%!FUNC! WdfRequestSend failed %!STATUS!", status);
        WdfRequestComplete(Request, status);
    }

    return;
}

VOID
xusbfixerDispatchPassThroughAndComplete(
    _In_ WDFREQUEST Request,
    _In_ WDFIOTARGET Target,
    _In_ PFN_WDF_REQUEST_COMPLETION_ROUTINE CompletionRoutine
)
/*++
Routine Description:

    Passes a request on to the lower driver.

--*/
{
    BOOLEAN ret;
    NTSTATUS status = STATUS_SUCCESS;

    WdfRequestFormatRequestUsingCurrentType(Request);
    WdfRequestSetCompletionRoutine(Request, CompletionRoutine, NULL);

    ret = WdfRequestSend(Request, Target, WDF_NO_SEND_OPTIONS);

    if (ret == FALSE) {
        status = WdfRequestGetStatus(Request);
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_QUEUE, "%!FUNC! WdfRequestSend failed %!STATUS!", status);
        WdfRequestComplete(Request, status);
    }

    return;
}

VOID
xusbfixerEvtIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode
)
/*++

Routine Description:

    This event is invoked for upper filter IRP_MJ_DEVICE_CONTROL requests.

Arguments:

    Queue -  Handle to the framework queue object that is associated with the
             I/O request.

    Request - Handle to a framework request object.

    OutputBufferLength - Size of the output buffer in bytes

    InputBufferLength - Size of the input buffer in bytes

    IoControlCode - I/O control code.

Return Value:

    VOID

--*/
{
    WDFDEVICE hDevice;
    PDEVICE_CONTEXT deviceContext;

    TraceEvents(TRACE_LEVEL_INFORMATION,
                TRACE_QUEUE,
                "%!FUNC! Queue 0x%p, Request 0x%p OutputBufferLength %d InputBufferLength %d IoControlCode %lx",
                Queue, Request, (int)OutputBufferLength, (int)InputBufferLength, IoControlCode);

#ifdef DBG
    {
        UINT8* buffer;
        size_t len;
        NTSTATUS status = WdfRequestRetrieveInputBuffer(Request, 0, &buffer, &len);

        if (NT_SUCCESS(status)) {
            char hexBuffer[256];
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_QUEUE, "%!FUNC! INPUT = %s",
                        printHex(hexBuffer, sizeof(hexBuffer), buffer, len));
        }
        else {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_QUEUE,
                        "%!FUNC! WdfRequestRetrieveInputBuffer failed %!STATUS!", status);
        }
    }
#endif

    hDevice = WdfIoQueueGetDevice(Queue);
    deviceContext = DeviceGetContext(hDevice);

    if (deviceContext->InputWaitQueue) {
        if (IoControlCode == 0x8000E3AC) {
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_QUEUE, "%!FUNC! forwarding to input queue 0x%p",
                        deviceContext->InputWaitQueue);

            NTSTATUS status = WdfRequestForwardToIoQueue(Request, deviceContext->InputWaitQueue);

            if (!NT_SUCCESS(status)) {
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_QUEUE,
                            "%!FUNC! WdfRequestForwardToIoQueue failed %!STATUS!", status);
                WdfRequestComplete(Request, status);
            }

            return;
        }
    }
    else {
        // Alternative xusb21 mode

        if (IoControlCode == 0x8000E004) {
            UINT8* buffer;
            size_t len;
            NTSTATUS status = WdfRequestRetrieveInputBuffer(Request, 0, &buffer, &len);

            if (NT_SUCCESS(status) && len >= 2 && (buffer[0] > 1 || buffer[1] != 1)) {
                TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_QUEUE,
                            "%!FUNC! completing with invalid status (buffer %02X %02X)", buffer[0], buffer[1]);
                WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
                return;
            }
        }
    }

    xusbfixerDispatchPassThrough(Request, WdfDeviceGetIoTarget(hDevice));
    return;
}

static
VOID
UpperCompletionRoutine(
    _In_ WDFREQUEST Request,
    _In_ WDFIOTARGET Target,
    _In_ PWDF_REQUEST_COMPLETION_PARAMS Params,
    _In_ WDFCONTEXT Context
)
{
    TraceEvents(TRACE_LEVEL_INFORMATION,
                TRACE_QUEUE,
                "%!FUNC! Request 0x%p Target 0x%p Params ioctl %lx Outlength %lu Status %lx Information %lu Context 0x%p",
                Request, Target,
                Params->Parameters.Ioctl.IoControlCode,
                (ULONG)Params->Parameters.Ioctl.Output.Length,
                Params->IoStatus.Status,
                (ULONG)Params->IoStatus.Information,
                Context);

    WdfRequestCompleteWithInformation(Request, Params->IoStatus.Status, Params->IoStatus.Information);
}

VOID
xusbfixerDispatchWaitForInput(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode
)
/*++

Routine Description:

    This event is invoked for upper filter WaitForInput IOCTL requests.

Arguments:

    Queue -  Handle to the framework queue object that is associated with the
             I/O request.

    Request - Handle to a framework request object.

    OutputBufferLength - Size of the output buffer in bytes

    InputBufferLength - Size of the input buffer in bytes

    IoControlCode - I/O control code.

Return Value:

    VOID

--*/
{
    WDFDEVICE hDevice;

    TraceEvents(TRACE_LEVEL_INFORMATION,
                TRACE_QUEUE,
                "%!FUNC! Queue 0x%p, Request 0x%p OutputBufferLength %d InputBufferLength %d IoControlCode %lx",
                Queue, Request, (int)OutputBufferLength, (int)InputBufferLength, IoControlCode);

    if (IoControlCode != 0x8000E3AC) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_QUEUE, "%!FUNC! invalid IoControlCode %lx", IoControlCode);
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER_5);
        return;
    }

    hDevice = WdfIoQueueGetDevice(Queue);

#ifdef DBG
    xusbfixerDispatchPassThroughAndComplete(Request, WdfDeviceGetIoTarget(hDevice), UpperCompletionRoutine);
#else
    xusbfixerDispatchPassThrough(Request, WdfDeviceGetIoTarget(hDevice));
#endif

    return;
}

static
VOID
LowerInternalCompletionRoutine(
    _In_ WDFREQUEST Request,
    _In_ WDFIOTARGET Target,
    _In_ PWDF_REQUEST_COMPLETION_PARAMS Params,
    _In_ WDFCONTEXT Context
)
{
    WDFDEVICE hDevice;

    TraceEvents(TRACE_LEVEL_INFORMATION,
                TRACE_QUEUE,
                "%!FUNC! Request 0x%p Target 0x%p Params ioctl %lx Outlength %lu Status %lx Information %lu Context 0x%p",
                Request, Target,
                Params->Parameters.Ioctl.IoControlCode,
                (ULONG)Params->Parameters.Ioctl.Output.Length,
                Params->IoStatus.Status,
                (ULONG)Params->IoStatus.Information,
                Context);

    WDFQUEUE myQueue = WdfRequestGetIoQueue(Request);
    hDevice = WdfIoQueueGetDevice(myQueue);
    PDEVICE_CONTEXT lowerDeviceContext = DeviceGetContext(hDevice);

    if (NT_SUCCESS(Params->IoStatus.Status)) {
        PIRP irp = WdfRequestWdmGetIrp(Request);
        PURB urb = URB_FROM_IRP(irp);
        UINT8* buffer = urb->UrbBulkOrInterruptTransfer.TransferBuffer;
        ULONG bufferLength = urb->UrbBulkOrInterruptTransfer.TransferBufferLength;

#ifdef DBG
        {
            char hexBuffer[256];
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_QUEUE, "%!FUNC! TransferLength %lu OUTPUT = %s",
                        bufferLength, printHex(hexBuffer, sizeof(hexBuffer), buffer, bufferLength));
        }
#endif

        // Gamepad state report is 20 bytes (where the last 6 bytes are
        // unused), but ViGEmBus driver only sends 14 bytes.
        if (bufferLength >= 14 && buffer[0] == 0) {
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_QUEUE, "%!FUNC! stopping input queue 0x%p",
                        lowerDeviceContext->InputWaitQueue);

            if (lowerDeviceContext->InputWaitQueue) {
                WdfIoQueueStop(lowerDeviceContext->InputWaitQueue, NULL, NULL);
            }
        }
        else {
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_QUEUE,
                        "%!FUNC! unexpected transfer data, len %lu first byte %02X",
                        bufferLength, bufferLength > 0 ? buffer[0] : 0xFF);
        }
    }
    else {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_QUEUE, "%!FUNC! skipped stopping input queue 0x%p",
                    lowerDeviceContext->InputWaitQueue);
    }

    WdfRequestCompleteWithInformation(Request, Params->IoStatus.Status, Params->IoStatus.Information);
}

VOID
xusbfixerEvtIoInternalDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode
)
/*++

Routine Description:

    This event is invoked for lower filter IRP_MJ_INTERNAL_DEVICE_CONTROL requests.

Arguments:

    Queue -  Handle to the framework queue object that is associated with the
             I/O request.

    Request - Handle to a framework request object.

    OutputBufferLength - Size of the output buffer in bytes

    InputBufferLength - Size of the input buffer in bytes

    IoControlCode - I/O control code.

Return Value:

    VOID

--*/
{
    WDFDEVICE hDevice;

    TraceEvents(TRACE_LEVEL_INFORMATION,
                TRACE_QUEUE,
                "%!FUNC! Queue 0x%p, Request 0x%p OutputBufferLength %d InputBufferLength %d IoControlCode %lx",
                Queue, Request, (int)OutputBufferLength, (int)InputBufferLength, IoControlCode);

    hDevice = WdfIoQueueGetDevice(Queue);

    if (IoControlCode == IOCTL_INTERNAL_USB_SUBMIT_URB /* 0x220003 */) {
        PIRP irp = WdfRequestWdmGetIrp(Request);
        PURB urb = URB_FROM_IRP(irp);

        if (urb->UrbHeader.Function == URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER) {
            struct _URB_BULK_OR_INTERRUPT_TRANSFER* pTransfer = &urb->UrbBulkOrInterruptTransfer;

            if ((pTransfer->TransferFlags & USBD_TRANSFER_DIRECTION_IN) &&
                (pTransfer->PipeHandle == (USBD_PIPE_HANDLE)(0xFFFF0081))) {
                PDEVICE_CONTEXT deviceContext = DeviceGetContext(hDevice);

                TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_QUEUE, "%!FUNC! starting input queue 0x%p",
                            deviceContext->InputWaitQueue);

                if (deviceContext->InputWaitQueue) {
                    WdfIoQueueStart(deviceContext->InputWaitQueue);
                }

                xusbfixerDispatchPassThroughAndComplete(Request,
                                                        WdfDeviceGetIoTarget(hDevice),
                                                        LowerInternalCompletionRoutine);
                return;
            }
        }
    }

    xusbfixerDispatchPassThrough(Request, WdfDeviceGetIoTarget(hDevice));
    return;
}
