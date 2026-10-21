// Minimal WDM driver used to prove the M6 toolchain end to end.
//
// This is NOT the audio driver. It exists to verify that this machine can
// compile a kernel-mode binary, stamp an INF, run Inf2Cat and produce a signed
// package -- the four things that must work before writing a PortCls miniport
// is worth the effort. It creates a device object and a symbolic link, and does
// nothing else.
//
// Build: driver\build-driver.bat

#include <ntddk.h>

#define OPEN_AES67_DEVICE_NAME L"\\Device\\OpenAes67Probe"
#define OPEN_AES67_SYMLINK_NAME L"\\DosDevices\\OpenAes67Probe"

static VOID OpenAes67ProbeUnload(PDRIVER_OBJECT DriverObject)
{
    UNICODE_STRING symlinkName;

    RtlInitUnicodeString(&symlinkName, OPEN_AES67_SYMLINK_NAME);
    IoDeleteSymbolicLink(&symlinkName);

    if (DriverObject->DeviceObject != NULL) {
        IoDeleteDevice(DriverObject->DeviceObject);
    }

    DbgPrint("OpenAES67 probe: unloaded\n");
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    UNICODE_STRING deviceName;
    UNICODE_STRING symlinkName;
    PDEVICE_OBJECT deviceObject = NULL;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(RegistryPath);

    DriverObject->DriverUnload = OpenAes67ProbeUnload;

    RtlInitUnicodeString(&deviceName, OPEN_AES67_DEVICE_NAME);
    RtlInitUnicodeString(&symlinkName, OPEN_AES67_SYMLINK_NAME);

    status = IoCreateDevice(DriverObject,
                            0,
                            &deviceName,
                            FILE_DEVICE_UNKNOWN,
                            0,
                            FALSE,
                            &deviceObject);
    if (!NT_SUCCESS(status)) {
        DbgPrint("OpenAES67 probe: IoCreateDevice failed 0x%08X\n", status);
        return status;
    }

    status = IoCreateSymbolicLink(&symlinkName, &deviceName);
    if (!NT_SUCCESS(status)) {
        DbgPrint("OpenAES67 probe: IoCreateSymbolicLink failed 0x%08X\n", status);
        IoDeleteDevice(deviceObject);
        return status;
    }

    DbgPrint("OpenAES67 probe: loaded, device %wZ\n", &deviceName);
    return STATUS_SUCCESS;
}
