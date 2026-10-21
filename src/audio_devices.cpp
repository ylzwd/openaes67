#include "openaes67/audio_devices.h"

#include <algorithm>
#include <cstdio>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propsys.h>

namespace openaes67 {
namespace {

std::string narrow(const wchar_t *text) {
    if (!text) {
        return {};
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) {
        return {};
    }
    std::string out(static_cast<size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), needed - 1, nullptr, nullptr);
    return out;
}

// COM is required for IMMDeviceEnumerator. Callers may already have initialised
// the apartment (the receive loop does), in which case CoInitializeEx returns
// RPC_E_CHANGED_MODE and that is fine: we simply must not uninitialise.
class ComScope {
public:
    ComScope() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        owned_ = SUCCEEDED(hr);
    }
    ~ComScope() {
        if (owned_) {
            CoUninitialize();
        }
    }
    ComScope(const ComScope &) = delete;
    ComScope &operator=(const ComScope &) = delete;

private:
    bool owned_ = false;
};

std::vector<AudioDeviceInfo> listWasapiDevices(EDataFlow flow, const char *backend) {
    std::vector<AudioDeviceInfo> devices;
    ComScope com;

    IMMDeviceEnumerator *enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void **>(&enumerator));
    if (FAILED(hr) || !enumerator) {
        return devices;
    }

    std::wstring defaultId;
    IMMDevice *defaultDevice = nullptr;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(flow, eConsole, &defaultDevice)) &&
        defaultDevice) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(defaultDevice->GetId(&id)) && id) {
            defaultId = id;
            CoTaskMemFree(id);
        }
        defaultDevice->Release();
    }

    IMMDeviceCollection *collection = nullptr;
    hr = enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection);
    if (SUCCEEDED(hr) && collection) {
        UINT count = 0;
        collection->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            IMMDevice *device = nullptr;
            if (FAILED(collection->Item(i, &device)) || !device) {
                continue;
            }

            LPWSTR id = nullptr;
            std::wstring wideId;
            if (SUCCEEDED(device->GetId(&id)) && id) {
                wideId = id;
                CoTaskMemFree(id);
            }

            std::wstring friendly;
            IPropertyStore *store = nullptr;
            if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store)) && store) {
                PROPVARIANT value;
                PropVariantInit(&value);
                if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &value)) &&
                    value.vt == VT_LPWSTR && value.pwszVal) {
                    friendly = value.pwszVal;
                }
                PropVariantClear(&value);
                store->Release();
            }

            AudioDeviceInfo info;
            info.backend = backend;
            info.name = narrow(friendly.empty() ? wideId.c_str() : friendly.c_str());
            info.id = narrow(wideId.c_str());
            info.isDefault = (!wideId.empty() && wideId == defaultId);
            info.isAsio = false;
            devices.push_back(std::move(info));
            device->Release();
        }
        collection->Release();
    }
    enumerator->Release();

    // Default first, then alphabetical: the combo box is easier to scan that way.
    std::stable_sort(devices.begin(), devices.end(),
                     [](const AudioDeviceInfo &a, const AudioDeviceInfo &b) {
                         if (a.isDefault != b.isDefault) {
                             return a.isDefault;
                         }
                         return a.name < b.name;
                     });
    return devices;
}

// ASIO drivers register themselves as COM classes under HKLM\SOFTWARE\ASIO,
// one subkey per driver, with a CLSID and a Description value. Reading that is
// independent of the ASIO SDK, so the list is available even in builds without
// the backend.
void collectAsioDrivers(HKEY root, REGSAM extra, std::vector<AudioDeviceInfo> &out) {
    HKEY asio = nullptr;
    if (RegOpenKeyExW(root, L"SOFTWARE\\ASIO", 0, KEY_READ | extra, &asio) != ERROR_SUCCESS) {
        return;
    }

    wchar_t name[256];
    for (DWORD index = 0;; ++index) {
        DWORD nameLength = 256;
        const LSTATUS status = RegEnumKeyExW(asio, index, name, &nameLength, nullptr, nullptr,
                                             nullptr, nullptr);
        if (status != ERROR_SUCCESS) {
            break;
        }

        HKEY driver = nullptr;
        if (RegOpenKeyExW(asio, name, 0, KEY_READ | extra, &driver) != ERROR_SUCCESS) {
            continue;
        }

        wchar_t description[256] = {0};
        DWORD descriptionBytes = sizeof(description);
        DWORD type = 0;
        if (RegQueryValueExW(driver, L"Description", nullptr, &type,
                             reinterpret_cast<LPBYTE>(description), &descriptionBytes) !=
            ERROR_SUCCESS) {
            description[0] = L'\0';
        }
        RegCloseKey(driver);

        AudioDeviceInfo info;
        info.backend = "asio";
        info.name = narrow(description[0] ? description : name);
        info.id = narrow(name);
        info.isDefault = false;
        info.isAsio = true;

        const bool duplicate = std::any_of(out.begin(), out.end(),
                                           [&](const AudioDeviceInfo &existing) {
                                               return existing.isAsio && existing.id == info.id;
                                           });
        if (!duplicate) {
            out.push_back(std::move(info));
        }
    }
    RegCloseKey(asio);
}

} // namespace

std::vector<AudioDeviceInfo> listAudioOutputDevices() {
    std::vector<AudioDeviceInfo> devices = listWasapiDevices(eRender, "wasapi");

    // 64-bit view first, then the 32-bit view; duplicates are dropped so a driver
    // registered in both appears once.
    collectAsioDrivers(HKEY_LOCAL_MACHINE, 0, devices);
    collectAsioDrivers(HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY, devices);
    return devices;
}

std::vector<AudioDeviceInfo> listAudioInputDevices() {
    // WASAPI has no separate capture device per render endpoint, but loopback
    // capture reads the render endpoint, so the render list is the useful one.
    std::vector<AudioDeviceInfo> devices = listWasapiDevices(eRender, "wasapi-loopback");

    collectAsioDrivers(HKEY_LOCAL_MACHINE, 0, devices);
    collectAsioDrivers(HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY, devices);
    return devices;
}

std::vector<AudioDeviceInfo> listCaptureEndpoints() {
    // Real input endpoints: microphones, line inputs, and the capture side of a
    // virtual cable. This is the list a recording application sees, so it is the
    // one to check when someone asks "why can't Audition see this device?".
    return listWasapiDevices(eCapture, "wasapi-capture");
}

} // namespace openaes67
