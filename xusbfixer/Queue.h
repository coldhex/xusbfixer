/*++

Module Name:

    queue.h

Abstract:

    This file contains the queue definitions.

Environment:

    Kernel-mode Driver Framework

--*/

EXTERN_C_START

NTSTATUS
xusbfixerQueueInitialize(
    _In_ WDFDEVICE Device
);

//
// Events from the IoQueue object
//
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL xusbfixerEvtIoDeviceControl;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL xusbfixerDispatchWaitForInput;
EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL xusbfixerEvtIoInternalDeviceControl;

EXTERN_C_END
