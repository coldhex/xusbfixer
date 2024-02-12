# xusbfixer

This is a bug fixing filter driver for Microsoft Windows xusb22.sys Xbox
gamepad driver (version 06/12/2019 10.0.19041.1).

## Description

Windows xusb22.sys driver interacting with dwm.exe (Desktop Window Manager) can
sometimes cause a very long (possibly infinite) loop where dwm.exe and the
driver are consuming 100% CPU. Especially when running Windows 10/11 in a
QEMU/KVM virtual machine and using the [ViGEm Bus driver](https://github.com/nefarius/ViGEmBus),
the system is rendered unusable as soon as a gamepad is connected.

The looping occurs in an xusb22.sys function called `ProcessWaitRequests`
(named according to the published PDB file) similar to the following
pseudocode:
```
while (TRUE) {
    WDFREQUEST request:
    WdfIoQueueRetrieveNextRequest(queue, &request);
    if (!request)
        break;

    // ... processing ...

    WdfRequestCompleteWithInformation(request, STATUS_SUCCESS, 29);
}
```

The `request` above originates from dwm.exe sending IOCTL 0x8000E3AC
(WaitForInput, to get gamepad stick and button state etc). Apparently, as soon
as the request is completed, dwm.exe sends a new IOCTL request to the driver
which inserts it into the same `queue` which is being processed by the loop
above. It can happen that the new request gets inserted into `queue` before
next `WdfIoQueueRetrieveNextRequest` call and this can repeat apparently
indefinitely.

## The (hopefully temporary) solution

The xusbfixer driver is intended to be a temporary solution until Microsoft
fixes xusb22. It is both an upper and lower filter driver where the driver
stack ends up looking like:

1. xusbfixer
2. xusb22
3. xusbfixer
4. ViGEmBus

The upper xusbfixer manages IOCTL 0x8000E3AC requests and doesn't let them pass
while the `ProcessWaitRequests` loop is running in xusb22. The lower xusbfixer
monitors USB interrupt transfer requests and their completion in an attempt to
detect when the `ProcessWaitRequests` loop is going to start and has finished.

## Installing

Unpack the release zip file containing the `inf` and `sys` files. In file
explorer, right click on the `inf` file and select `Install`.

### Alternative xusb21 mode

Another way to fix the infinite looping in xusb22 `ProcessWaitRequests` is to
install the previous version of Microsoft's driver, xusb21. That version does
not support the IOCTL 0x8000E3AC and doesn't have the `ProcessWaitRequests`
function. However, some applications won't be able to use gamepads anymore
(such as Windows start and settings menus.) The xusbfixer driver has an
alternative simple mode that mimics xusb21 and responds with invalid parameter
status to the IOCTL 0x8000E004 (GetCapabilities). To turn on this alternative
mode, simply remove xusbfixer from the lower filter list in Windows registry
(cf. step 5 below in the uninstalling section.) To restore the default
behaviour, add xusbfixer back.

## Uninstalling

Run the included PowerShell script `uninstall-xusbfixer.ps1` as an
administrator and reboot. Alternatively, you can uninstall manually:

1. In command prompt, run `pnputil /enum-drivers` and note the "Published
   name" (e.g. `oemXX.inf` filename) for xusbfixer.inf,
2. Run `pnputil /delete-driver oemXX.inf` where `oemXX.inf` is from the
   previous step,
3. Delete file `C:\Windows\System32\drivers\xusbfixer.sys`,
4. Run `regedit.exe` and delete key `HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\xusbfixer`,
5. Delete also `LowerFilters` and `UpperFilters` properties under the
   `HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\{d61ca365-5af4-4486-998b-9db4734c6ca3}`
   key,
6. Reboot.

## Caveat

This driver is only self-signed so you will have to use test mode or disable
Windows driver signature enforcement. Either

1. Disable Secure Boot in UEFI settings, enable test mode using the command
   `bcdedit /set testsigning on` and reboot, or
2. Reboot Windows by pressing Shift and clicking Restart from start menu,
   then select Troubleshoot / Advanced options / Start-up Settings and restart.
   When the boot menu shows up, select "Disable driver signature enforcement".
   You can make the boot menu show up on every boot by running the command
   `bcdedit /set advancedoptions on`.

## How to build

To build the driver from source code, install [Visual Studio 2022, Windows SDK
and Windows Driver Kit](https://learn.microsoft.com/en-us/windows-hardware/drivers/download-the-wdk).

(Tip: if VS2022 starts consuming 100% cpu and the UI freezes for a while
whenever you start editing a file (like it does for me with version 17.8.5),
try setting "Tools -> Options -> Text Editor -> C/C++ -> Advanced -> Disable Database Auto Updates" to `True`.)

## License

The project is licensed under the **MIT License**. See
[LICENSE.txt](LICENSE.txt) for details.

## Related software

- [ViGEm Bus driver](https://github.com/nefarius/ViGEmBus)
- Application software to accompany the bus driver:
  - [Xb2XInput](https://github.com/emoose/Xb2XInput)
  - [x360ce](https://www.x360ce.com/)
