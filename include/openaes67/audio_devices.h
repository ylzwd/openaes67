#pragma once
#include <string>
#include <vector>

namespace openaes67 {

// One selectable audio output. Until now every backend silently used the system
// default endpoint, so a machine with an onboard codec, a virtual cable and a USB
// interface had no way to choose between them.
struct AudioDeviceInfo {
    std::string backend;    // "wasapi" | "asio"
    std::string name;       // friendly name shown to the user
    std::string id;         // WASAPI endpoint id, or the ASIO driver key
    // Default-initialised: a caller that only fills some fields (the GUI's
    // "system default" placeholder, for instance) used to leave isAsio holding
    // stack garbage. It read as true and the device list grew a nameless
    // phantom ASIO entry.
    bool        isDefault = false;  // the system default render endpoint
    bool        isAsio = false;
};

// Enumerates *capture* endpoints (WASAPI) and registered ASIO drivers.
//
// This is the list the GUI shows for sending: OpenAES67 acts as a network sound
// card, so it takes audio FROM a device and puts it on the network. What it
// needs is an input, not an output. WASAPI capture of a render endpoint is
// loopback (the classic "what the PC is playing" tap).
std::vector<AudioDeviceInfo> listAudioInputDevices();

// Enumerates render endpoints (WASAPI) and registered ASIO drivers (registry).
// ASIO entries are listed even when the build has no ASIO backend: the caller can
// then explain why the device cannot be opened, instead of hiding it.
std::vector<AudioDeviceInfo> listAudioOutputDevices();

// Enumerates real input endpoints (WASAPI eCapture): microphones, line inputs and
// the capture side of a virtual cable. listAudioInputDevices() above returns
// *render* endpoints because this project captures them by loopback; this one
// returns what a recording application sees, which is a different list.
std::vector<AudioDeviceInfo> listCaptureEndpoints();

// True when this build was compiled with the ASIO backend (needs the Steinberg
// ASIO SDK headers; see docs/AUDIO-DEVICES.md).
bool asioBackendAvailable();

// Reason the ASIO backend is unavailable, or an empty string when it is.
std::string asioBackendUnavailableReason();

} // namespace openaes67
