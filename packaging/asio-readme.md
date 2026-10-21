# OpenAES67 ASIO sound card

`openaes67_asio64.dll` is the 64-in / 64-out ASIO driver. It is a user-mode COM
DLL, so it needs no kernel signing and works with Secure Boot on.

## Register it

Double-click `register-asio-driver.bat`, or run it from an **administrator**
prompt. It writes:

```
HKLM\SOFTWARE\ASIO\OpenAES67 ASIO 64x64
HKLM\SOFTWARE\Classes\CLSID\{7C1F2E64-9A3B-4C5D-8E7F-0A1B2C3D4E5F}\InprocServer32
```

After that the DLL must stay where it is: the registry points at this path, so
copy the folder somewhere permanent (not `Downloads`) before registering.

Restart the application that should see the driver — most DAWs enumerate ASIO
drivers only at startup.

## Unregister

`unregister-asio-driver.bat` (also needs an administrator prompt) removes both
keys. Your audio settings in the DAW are not touched.

## What it does

- 64 inputs and 64 outputs at 48 kHz, 256-frame buffer
- 32-bit int LSB samples, the format ASIO hosts expect
- Talks to the `openaes67 bridge` process through a shared memory ring named
  `OpenAES67Audio`; start the bridge first, then open the driver

```
openaes67.exe bridge --iface <your NIC> --channels 64 --ptime 1 --shm OpenAES67Audio --sap
```

If no bridge is running the driver still opens and returns silence, so a DAW
will not hang waiting for it.

## Not sure it registered?

```
openaes67.exe doctor --iface <your NIC>
```

The `asio:` line reports the registry key and the DLL path it resolved.
