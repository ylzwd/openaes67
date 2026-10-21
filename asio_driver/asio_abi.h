#pragma once
// Minimal declarations of the ASIO interface, written for this project.
//
// Why this file exists
// --------------------
// Steinberg's ASIO SDK is not redistributable, so it cannot live in this
// repository. An ASIO driver is, however, just a COM in-process server whose
// object implements one abstract class (IASIO) and whose vtable layout is a
// published ABI contract: hosts call through it by offset, not by header. This
// file declares exactly that contract so the driver can be built and tested
// without shipping anyone else's source.
//
// If you have the official SDK, put iasiodrv.h in third_party/asio and build
// with OPENAES67_USE_OFFICIAL_ASIO_SDK; then the official header is used
// instead and this file is skipped. Both paths must agree -- if they ever
// disagree, the official one wins.
//
// Only the parts a driver and a test host need are declared here.

#if defined(OPENAES67_USE_OFFICIAL_ASIO_SDK)
#include "iasiodrv.h"
#else

#include <cstdint>

// ---------------------------------------------------------------- basic types
typedef long ASIOBool;
typedef long ASIOError;
typedef double ASIOSampleRate;
typedef int64_t ASIOSamples;      // ASIO 2.x widened this from 32 bits
typedef int64_t ASIOTimeStamp;

enum { ASIOFalse = 0, ASIOTrue = 1 };

enum {
    ASE_OK = 0,
    ASE_SUCCESS = 0x3f4847a0,     // returned by future() when a call is supported
    ASE_NotPresent = -1000,
    ASE_HWMalfunction,
    ASE_InvalidParameter,
    ASE_InvalidMode,
    ASE_SPNotAdvancing,
    ASE_NoClock,
    ASE_NoMemory
};

enum {
    ASIOSTInt16LSB = 0,
    ASIOSTInt24LSB = 1,
    ASIOSTInt32LSB = 2,
    ASIOSTFloat32LSB = 3,
    ASIOSTFloat64LSB = 4
};

enum {
    kAsioSelectorSupported = 1,
    kAsioEngineVersion,
    kAsioResetRequest,
    kAsioBufferSizeChange,
    kAsioResyncRequest,
    kAsioLatenciesChanged,
    kAsioSupportsTimeInfo,
    kAsioSupportsTimeCode,
    kAsioMMCCommand,
    kAsioSupportsInputMonitor,
    kAsioSupportsInputGain,
    kAsioSupportsInputMeter,
    kAsioSupportsOutputGain,
    kAsioSupportsOutputMeter,
    kAsioOverload
};

// ------------------------------------------------------------------ structures
struct ASIOClockSource {
    long     index;
    long     associatedChannel;
    long     associatedGroup;
    ASIOBool isCurrentSource;
    char     name[32];
};

struct ASIOChannelInfo {
    long     channel;
    ASIOBool isInput;
    ASIOBool isActive;
    long     channelGroup;
    long     type;              // ASIOSampleType, e.g. ASIOSTInt32LSB
    char     name[32];
};

struct ASIOBufferInfo {
    ASIOBool isInput;
    long     channelNum;
    void    *buffers[2];        // double buffering: buffers[0] and buffers[1]
};

struct ASIOTimeInfo {
    double        speed;
    ASIOTimeStamp systemTime;
    ASIOSamples   samplePosition;
    ASIOSampleRate sampleRate;
    unsigned long flags;
    char          reserved[12];
};

struct ASIOTime {
    long          reserved[4];
    ASIOTimeInfo  timeInfo;
    long          future[4];
};

typedef void        (*buffer_switch_t)(long index, ASIOBool processNow);
typedef ASIOTime   *(*buffer_switch_time_info_t)(long index, ASIOTime *timeInfo,
                                                 ASIOBool processNow);
typedef long        (*asio_message_t)(long selector, long value, void *message, double *opt);

struct ASIOCallbacks {
    buffer_switch_t          bufferSwitch;
    asio_message_t           asioMessage;
    buffer_switch_time_info_t bufferSwitchTimeInfo;
};

// ------------------------------------------------------------------- IASIO
// The method order below is the ABI. IASIO derives from IUnknown, so the first
// three vtable slots are QueryInterface/AddRef/Release -- hosts rely on that.
class IASIO : public IUnknown {
public:
    virtual ASIOBool    init(void *sysHandle) = 0;
    virtual void        getDriverName(char *name) = 0;
    virtual long        getDriverVersion() = 0;
    virtual void        getErrorMessage(char *string) = 0;
    virtual ASIOError   start() = 0;
    virtual ASIOError   stop() = 0;
    virtual ASIOError   getChannels(long *numInputChannels, long *numOutputChannels) = 0;
    virtual ASIOError   getLatencies(long *inputLatency, long *outputLatency) = 0;
    virtual ASIOError   getBufferSize(long *minSize, long *maxSize, long *preferredSize,
                                      long *granularity) = 0;
    virtual ASIOError   canSampleRate(ASIOSampleRate sampleRate) = 0;
    virtual ASIOError   getSampleRate(ASIOSampleRate *sampleRate) = 0;
    virtual ASIOError   setSampleRate(ASIOSampleRate sampleRate) = 0;
    virtual ASIOError   getClockSources(ASIOClockSource *clocks, long *numSources) = 0;
    virtual ASIOError   setClockSource(long reference) = 0;
    virtual ASIOError   getSamplePosition(ASIOSamples *sPos, ASIOTimeStamp *tStamp) = 0;
    virtual ASIOError   getChannelInfo(ASIOChannelInfo *info) = 0;
    virtual ASIOError   createBuffers(ASIOBufferInfo *bufferInfos, long numChannels,
                                      long bufferSize, ASIOCallbacks *callbacks) = 0;
    virtual ASIOError   disposeBuffers() = 0;
    virtual ASIOError   controlPanel() = 0;
    virtual ASIOError   future(long selector, void *opt) = 0;
    virtual ASIOError   outputReady() = 0;
};

#endif // OPENAES67_USE_OFFICIAL_ASIO_SDK
