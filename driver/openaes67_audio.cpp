// SPDX-License-Identifier: MIT
// Copyright (c) 2026 OpenAES67 contributors
//
// Every behaviour in this file is traceable to a published standard
// (AES67-2018, RFC 3550, RFC 4566, RFC 2974, IEEE 1588) or to a
// reproducible measurement recorded in docs/FINDINGS.md.

// OpenAES67 audio driver -- PortCls adapter skeleton.
//
// What this file is
// -----------------
// The first compilable step towards the M6 virtual sound card: a PortCls adapter
// driver plus a topology miniport. PortCls can bind it and register a subdevice.
//
// What this file is not
// ---------------------
// It is not verified to load or to play audio: this machine has Secure Boot
// enabled, so a test-signed kernel driver cannot start (docs/M6-FEASIBILITY.md).
// Everything here is compile-verified against portcls.h only. The WaveRT render
// stream (AllocateAudioBuffer / GetPosition / pumping the shared ring) is the
// next step.
//
// Build: driver\build-driver.bat (target openaes67_audio.sys)

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <ntddk.h>
// initguid.h must precede portcls.h so the interface and class GUIDs declared
// there are instantiated in this translation unit instead of staying extern.
#include <initguid.h>
#include <portcls.h>
#include <stdunk.h>
#include <ksmedia.h>

#include "shared_audio.h"

#define OPENAES67_POOL_TAG '6AeO'   /* 'OAe6' little endian */

// Kernel-mode operator new/delete. The C++ runtime is not linked into a driver,
// so these must be supplied here; they back the miniport allocations.
void *__cdecl operator new(size_t size)
{
    return ExAllocatePool2(POOL_FLAG_NON_PAGED, size, OPENAES67_POOL_TAG);
}

// operator delete is already provided by the WDK headers; only new was missing.

// Pin and node numbering for the (single) render path.
enum {
    kNodeTopologyRender = 0,
    kNodeTopologyBridge = 1,

    kPinTopologyRenderIn = 0,
    kPinTopologyRenderBridgeIn = 1,
    kPinTopologyRenderBridgeOut = 2,
    kPinTopologyRenderOut = 3
};

// ---------------------------------------------------------------- filter table
//
// PortCls will not create a subdevice without a descriptor. These tables describe
// one render path: input pin -> render node -> bridge node -> output pin.
//
// Field order matters and is not obvious; it was read from portcls.h:
//   PCNODE_DESCRIPTOR       { ULONG Flags; const PCAUTOMATION_TABLE*; const GUID* Type; const GUID* Name; }
//   PCPIN_DESCRIPTOR        { MaxGlobal; MaxFilter; MinFilter; const PCAUTOMATION_TABLE*; KSPIN_DESCRIPTOR }
//   PCCONNECTION_DESCRIPTOR { FromNode; FromNodePin; ToNode; ToNodePin; }
//
// PCNODE_DESCRIPTOR is four fields, not six: the first attempt used the older
// DDK layout and the compiler rejected it with "too many initializers".

// C++ will not convert an int literal to KSPIN_DATAFLOW / KSPIN_COMMUNICATION,
// so every pin spells out both enums.
static PCPIN_DESCRIPTOR g_TopologyPins[] = {
    // kPinTopologyRenderIn: the host renders into this pin
    { 1, 1, 1, NULL,
      { 0, NULL, 0, NULL, 0, NULL, KSPIN_DATAFLOW_IN, KSPIN_COMMUNICATION_SINK,
        NULL, NULL, { 0 } } },
    // kPinTopologyRenderBridgeIn
    { 1, 1, 1, NULL,
      { 0, NULL, 0, NULL, 0, NULL, KSPIN_DATAFLOW_OUT, KSPIN_COMMUNICATION_BRIDGE,
        NULL, NULL, { 0 } } },
    // kPinTopologyRenderBridgeOut
    { 1, 1, 1, NULL,
      { 0, NULL, 0, NULL, 0, NULL, KSPIN_DATAFLOW_IN, KSPIN_COMMUNICATION_BRIDGE,
        NULL, NULL, { 0 } } },
    // kPinTopologyRenderOut: goes out to the endpoint
    { 1, 1, 1, NULL,
      { 0, NULL, 0, NULL, 0, NULL, KSPIN_DATAFLOW_OUT, KSPIN_COMMUNICATION_SOURCE,
        NULL, NULL, { 0 } } }
};

static PCNODE_DESCRIPTOR g_TopologyNodes[] = {
    { 0, NULL, NULL, NULL },
    { 0, NULL, NULL, NULL }
};

static PCCONNECTION_DESCRIPTOR g_TopologyConnections[] = {
    { kNodeTopologyRender, kPinTopologyRenderIn,
      kNodeTopologyBridge, kPinTopologyRenderBridgeIn },
    { kNodeTopologyBridge, kPinTopologyRenderBridgeOut,
      kNodeTopologyRender, kPinTopologyRenderOut }
};

static PCFILTER_DESCRIPTOR g_TopologyFilter = {
    0,                                      // Version
    NULL,                                   // AutomationTable
    sizeof(PCPIN_DESCRIPTOR), SIZEOF_ARRAY(g_TopologyPins), g_TopologyPins,
    sizeof(PCNODE_DESCRIPTOR), SIZEOF_ARRAY(g_TopologyNodes), g_TopologyNodes,
    SIZEOF_ARRAY(g_TopologyConnections), g_TopologyConnections,
    0, NULL                                 // categories
};

// ------------------------------------------------------------ topology miniport
//
// IUnknown is implemented by hand rather than through the CUnknown helper: the
// helper's constructor signature differs between WDK releases and this class
// only needs three methods.

class CMiniportTopology : public IMiniportTopology
{
public:
    CMiniportTopology() : m_RefCount(1), m_Port(NULL) {}

    STDMETHODIMP_(NTSTATUS) QueryInterface(REFIID riid, PVOID *ppv) override
    {
        if (ppv == NULL) {
            return STATUS_INVALID_PARAMETER;
        }
        *ppv = NULL;
        if (IsEqualGUIDAligned(riid, IID_IUnknown) ||
            IsEqualGUIDAligned(riid, IID_IMiniport) ||
            IsEqualGUIDAligned(riid, IID_IMiniportTopology)) {
            *ppv = static_cast<IMiniportTopology *>(this);
            AddRef();
            return STATUS_SUCCESS;
        }
        return STATUS_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override
    {
        return static_cast<ULONG>(InterlockedIncrement(&m_RefCount));
    }

    STDMETHODIMP_(ULONG) Release() override
    {
        const LONG remaining = InterlockedDecrement(&m_RefCount);
        if (remaining == 0) {
            delete this;
        }
        return static_cast<ULONG>(remaining);
    }

    STDMETHODIMP_(NTSTATUS) Init(PUNKNOWN adapterUnknown,
                                 PRESOURCELIST resourceList,
                                 PPORTTOPOLOGY port) override
    {
        UNREFERENCED_PARAMETER(adapterUnknown);
        UNREFERENCED_PARAMETER(resourceList);
        m_Port = port;
        return STATUS_SUCCESS;
    }

    STDMETHODIMP_(NTSTATUS) GetDescription(PPCFILTER_DESCRIPTOR *description) override
    {
        if (description == NULL) {
            return STATUS_INVALID_PARAMETER;
        }
        *description = &g_TopologyFilter;
        return STATUS_SUCCESS;
    }

    STDMETHODIMP_(NTSTATUS) DataRangeIntersection(ULONG pinId,
                                                  PKSDATARANGE dataRange,
                                                  PKSDATARANGE matchingDataRange,
                                                  ULONG outputBufferLength,
                                                  PVOID resultantFormat,
                                                  PULONG resultantFormatLength) override
    {
        UNREFERENCED_PARAMETER(pinId);
        UNREFERENCED_PARAMETER(dataRange);
        UNREFERENCED_PARAMETER(matchingDataRange);
        UNREFERENCED_PARAMETER(outputBufferLength);
        UNREFERENCED_PARAMETER(resultantFormat);
        if (resultantFormatLength != NULL) {
            *resultantFormatLength = 0;
        }
        return STATUS_NOT_IMPLEMENTED;
    }

private:
    LONG           m_RefCount;
    PPORTTOPOLOGY  m_Port;
};

// Defined after the class (the adapter cannot allocate an incomplete type).
PUNKNOWN CreateWaveMiniport();

// -------------------------------------------------------------- adapter plumbing

static NTSTATUS OpenAes67StartDevice(PDEVICE_OBJECT deviceObject, PIRP irp,
                                     PRESOURCELIST resourceList);

static NTSTATUS OpenAes67AddDevice(PDRIVER_OBJECT driverObject,
                                   PDEVICE_OBJECT physicalDeviceObject)
{
    return PcAddAdapterDevice(driverObject, physicalDeviceObject,
                              OpenAes67StartDevice, 1, 0);
}

static NTSTATUS OpenAes67StartDevice(PDEVICE_OBJECT deviceObject, PIRP irp,
                                     PRESOURCELIST resourceList)
{
    NTSTATUS status;
    PPORTTOPOLOGY port = NULL;
    PUNKNOWN topologyUnknown = NULL;

    status = PcNewPort(reinterpret_cast<PPORT *>(&port), CLSID_PortTopology);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    CMiniportTopology *miniport = new CMiniportTopology();
    if (miniport == NULL) {
        port->Release();
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // IPortTopology::Init(DeviceObject, Irp, UnknownAdapter, Miniport, ResourceList)
    status = port->Init(deviceObject, irp, NULL, miniport, resourceList);
    miniport->Release();
    if (!NT_SUCCESS(status)) {
        port->Release();
        return status;
    }

    status = port->QueryInterface(IID_IUnknown,
                                  reinterpret_cast<PVOID *>(&topologyUnknown));
    if (!NT_SUCCESS(status)) {
        port->Release();
        return status;
    }

    status = PcRegisterSubdevice(deviceObject, L"Topology", topologyUnknown);
    topologyUnknown->Release();
    port->Release();
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // Wave subdevice: the actual render endpoint. PortCls needs a port object of
    // the matching class before it will register the miniport.
    PPORTWAVERT wavePort = NULL;
    PUNKNOWN waveUnknown = NULL;

    status = PcNewPort(reinterpret_cast<PPORT *>(&wavePort), CLSID_PortWaveRT);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    PUNKNOWN waveMiniport = CreateWaveMiniport();
    if (waveMiniport == NULL) {
        wavePort->Release();
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = wavePort->Init(deviceObject, irp, NULL, waveMiniport, resourceList);
    waveMiniport->Release();
    if (!NT_SUCCESS(status)) {
        wavePort->Release();
        return status;
    }

    status = wavePort->QueryInterface(IID_IUnknown,
                                      reinterpret_cast<PVOID *>(&waveUnknown));
    if (!NT_SUCCESS(status)) {
        wavePort->Release();
        return status;
    }

    status = PcRegisterSubdevice(deviceObject, L"Wave", waveUnknown);
    waveUnknown->Release();
    wavePort->Release();
    return status;
}

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driverObject,
                                PUNICODE_STRING registryPath)
{
    return PcInitializeAdapterDriver(driverObject, registryPath, OpenAes67AddDevice);
}

// ------------------------------------------------------------- wave data ranges
//
// One format: 2 channels, 16-bit PCM, 48 kHz -- exactly what AES67 L16 uses, so
// no conversion is needed between the ring and the endpoint.

static KSDATARANGE_AUDIO g_WaveRange = {
    { sizeof(KSDATARANGE_AUDIO), 0, 0, 0,
      STATIC_KSDATAFORMAT_SUBTYPE_WAVEFORMATEX,
      STATIC_KSDATAFORMAT_SUBTYPE_PCM },
    2,      // MaximumChannels
    16,     // MinimumBitsPerSample
    16,     // MaximumBitsPerSample
    48000,  // MinimumSampleFrequency
    48000   // MaximumSampleFrequency
};

static PCPIN_DESCRIPTOR g_WavePins[] = {
    // kPinWaveRenderIn: host renders into the wave filter
    { 1, 1, 1, NULL,
      { 1, NULL, 0, NULL, 1, NULL, KSPIN_DATAFLOW_IN, KSPIN_COMMUNICATION_SINK,
        NULL, NULL, { 0 } } }
};

static PCNODE_DESCRIPTOR g_WaveNodes[] = {
    { 0, NULL, NULL, NULL }
};

static PCCONNECTION_DESCRIPTOR g_WaveConnections[] = {
    { 0, 0, 0, 0 }
};

static PCFILTER_DESCRIPTOR g_WaveFilter = {
    0, NULL,
    sizeof(PCPIN_DESCRIPTOR), 1, g_WavePins,
    sizeof(PCNODE_DESCRIPTOR), 1, g_WaveNodes,
    0, NULL,
    0, NULL
};

// ------------------------------------------------------------------ wave stream

class CMiniportWaveRTStream : public IMiniportWaveRTStream
{
    // 2 channels * 16 bit. Kept beside the frame maths so the play offset and the
    // buffer sizing cannot drift apart.
    static constexpr ULONG kFrameBytes = 4;
public:
    CMiniportWaveRTStream() : m_RefCount(1), m_Buffer(NULL), m_Mdl(NULL),
                              m_BufferBytes(0), m_State(KSSTATE_STOP),
                              m_PlayedFrames(0), m_StartedAt(0)
    {
        KeInitializeTimerEx(&m_Timer, NotificationTimer);
        KeInitializeDpc(&m_Dpc, TimerDpc, this);
    }

    STDMETHODIMP_(NTSTATUS) QueryInterface(REFIID riid, PVOID *ppv) override
    {
        if (!ppv) return STATUS_INVALID_PARAMETER;
        *ppv = NULL;
        if (IsEqualGUIDAligned(riid, IID_IUnknown) ||
            IsEqualGUIDAligned(riid, IID_IMiniportWaveRTStream)) {
            *ppv = static_cast<IMiniportWaveRTStream *>(this);
            AddRef();
            return STATUS_SUCCESS;
        }
        return STATUS_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override
    {
        return static_cast<ULONG>(InterlockedIncrement(&m_RefCount));
    }
    STDMETHODIMP_(ULONG) Release() override
    {
        const LONG remaining = InterlockedDecrement(&m_RefCount);
        if (remaining == 0) delete this;
        return static_cast<ULONG>(remaining);
    }

    STDMETHODIMP_(NTSTATUS) SetFormat(PKSDATAFORMAT format) override
    {
        if (!format) return STATUS_INVALID_PARAMETER;
        const KSDATAFORMAT_WAVEFORMATEX *wfx =
            reinterpret_cast<const KSDATAFORMAT_WAVEFORMATEX *>(format);
        if (wfx->WaveFormatEx.nChannels != 2 ||
            wfx->WaveFormatEx.wBitsPerSample != 16 ||
            wfx->WaveFormatEx.nSamplesPerSec != 48000) {
            return STATUS_INVALID_PARAMETER;
        }
        return STATUS_SUCCESS;
    }

    STDMETHODIMP_(NTSTATUS) SetState(KSSTATE state) override
    {
        m_State = state;
        if (state == KSSTATE_RUN) {
            m_StartedAt = KeQueryInterruptTime();
            // 1 ms refill cadence: one packet, matching the AES67 ptime.
            // Relative due time is negative, in 100 ns units: -10000 = 1 ms.
            // The period argument is in milliseconds.
            LARGE_INTEGER due;
            due.QuadPart = -10000LL;
            KeSetTimerEx(&m_Timer, due, 1, &m_Dpc);
        } else {
            KeCancelTimer(&m_Timer);
            if (state == KSSTATE_STOP) {
                m_PlayedFrames = 0;
            }
        }
        return STATUS_SUCCESS;
    }

    STDMETHODIMP_(NTSTATUS) GetPosition(KSAUDIO_POSITION *position) override
    {
        if (!position) return STATUS_INVALID_PARAMETER;
        if (m_State == KSSTATE_RUN) {
            const ULONGLONG elapsed100ns = KeQueryInterruptTime() - m_StartedAt;
            const ULONGLONG frames = (elapsed100ns * 48000ULL) / 10000000ULL;
            m_PlayedFrames = frames;
        }
        position->PlayOffset = m_PlayedFrames * kFrameBytes;
        position->WriteOffset = position->PlayOffset;
        return STATUS_SUCCESS;
    }

    STDMETHODIMP_(NTSTATUS) AllocateAudioBuffer(ULONG requestedSize,
                                                PMDL *audioBufferMdl,
                                                ULONG *actualSize,
                                                ULONG *offsetFromFirstPage,
                                                MEMORY_CACHING_TYPE *cacheType) override
    {
        if (!audioBufferMdl || !actualSize || !offsetFromFirstPage || !cacheType) {
            return STATUS_INVALID_PARAMETER;
        }
        // PortCls asks for the minimum wave buffer; anything smaller than a single
        // frame cannot describe this format. Also keeps the parameter meaningful
        // instead of silently ignored.
        if (requestedSize < kFrameBytes) {
            return STATUS_INVALID_PARAMETER;
        }
        if (m_Buffer) {
            return STATUS_UNSUCCESSFUL;      // one buffer per stream
        }

        // 10 ms of audio, aligned to the AES67 packet size.
        const ULONG bytes = 48000 / 100 * 4;
        PVOID buffer = ExAllocatePool2(POOL_FLAG_NON_PAGED, bytes, OPENAES67_POOL_TAG);
        if (!buffer) return STATUS_INSUFFICIENT_RESOURCES;
        RtlZeroMemory(buffer, bytes);

        PMDL mdl = IoAllocateMdl(buffer, bytes, FALSE, FALSE, NULL);
        if (!mdl) {
            ExFreePoolWithTag(buffer, OPENAES67_POOL_TAG);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        MmBuildMdlForNonPagedPool(mdl);

        m_Buffer = buffer;
        m_Mdl = mdl;
        m_BufferBytes = bytes;

        *audioBufferMdl = mdl;
        *actualSize = bytes;
        *offsetFromFirstPage = 0;
        *cacheType = MmCached;
        return STATUS_SUCCESS;
    }

    STDMETHODIMP_(VOID) FreeAudioBuffer(PMDL audioBufferMdl, ULONG bufferSize) override
    {
        UNREFERENCED_PARAMETER(bufferSize);
        KeCancelTimer(&m_Timer);
        if (audioBufferMdl && audioBufferMdl == m_Mdl) {
            IoFreeMdl(m_Mdl);
            m_Mdl = NULL;
        }
        if (m_Buffer) {
            ExFreePoolWithTag(m_Buffer, OPENAES67_POOL_TAG);
            m_Buffer = NULL;
        }
        m_BufferBytes = 0;
        CloseRing();
    }

    STDMETHODIMP_(VOID) GetHWLatency(KSRTAUDIO_HWLATENCY *hwLatency) override
    {
        if (!hwLatency) return;
        hwLatency->FifoSize = 0;
        hwLatency->ChipsetDelay = 0;
        hwLatency->CodecDelay = 0;
    }

    STDMETHODIMP_(NTSTATUS) GetPositionRegister(KSRTAUDIO_HWREGISTER *reg) override
    {
        UNREFERENCED_PARAMETER(reg);
        return STATUS_NOT_IMPLEMENTED;      // time-based position, no register
    }

    STDMETHODIMP_(NTSTATUS) GetClockRegister(KSRTAUDIO_HWREGISTER *reg) override
    {
        UNREFERENCED_PARAMETER(reg);
        return STATUS_NOT_IMPLEMENTED;
    }

    // Opens the named section the user-mode pump created and maps it into
    // system space. Returns STATUS_NOT_FOUND when the pump is not running yet,
    // which is normal during driver start.
    NTSTATUS OpenRing(const WCHAR *sectionName)
    {
        if (sectionName == NULL) {
            return STATUS_INVALID_PARAMETER;
        }
        if (m_Ring != NULL) {
            return STATUS_SUCCESS;
        }

        UNICODE_STRING name;
        RtlInitUnicodeString(&name, sectionName);

        OBJECT_ATTRIBUTES attributes;
        InitializeObjectAttributes(&attributes, &name,
                                   OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                                   NULL, NULL);

        HANDLE section = NULL;
        NTSTATUS status = ZwOpenSection(&section,
                                        SECTION_MAP_READ | SECTION_MAP_WRITE,
                                        &attributes);
        if (!NT_SUCCESS(status)) {
            return status;
        }

        PVOID base = NULL;
        SIZE_T viewSize = 0;
        status = ZwMapViewOfSection(section, ZwCurrentProcess(), &base, 0, 0, NULL,
                                    &viewSize, ViewUnmap, 0, PAGE_READWRITE);
        if (!NT_SUCCESS(status)) {
            ZwClose(section);
            return status;
        }

        OpenAes67SharedAudio *header = static_cast<OpenAes67SharedAudio *>(base);
        if (header->magic != OPEN_AES67_SHM_MAGIC ||
            header->version != OPEN_AES67_SHM_VERSION ||
            header->frameBytes == 0 || header->capacityFrames == 0) {
            ZwUnmapViewOfSection(ZwCurrentProcess(), base);
            ZwClose(section);
            return STATUS_REVISION_MISMATCH;
        }

        m_SectionHandle = section;
        m_RingBase = base;
        m_Ring = header;
        return STATUS_SUCCESS;
    }

    void CloseRing()
    {
        if (m_RingBase != NULL) {
            ZwUnmapViewOfSection(ZwCurrentProcess(), m_RingBase);
            m_RingBase = NULL;
            m_Ring = NULL;
        }
        if (m_SectionHandle != NULL) {
            ZwClose(m_SectionHandle);
            m_SectionHandle = NULL;
        }
    }

private:
    static VOID TimerDpc(PKDPC, PVOID context, PVOID, PVOID)
    {
        static_cast<CMiniportWaveRTStream *>(context)->Refill();
    }

    // Pull whatever the user-mode pump produced into the cyclic buffer. Missing
    // frames stay silent and are counted, exactly like the user-mode path does.
    void Refill()
    {
        if (!m_Buffer || m_BufferBytes == 0) {
            return;
        }
        const uint32_t capacityFrames = m_BufferBytes / 4;
        uint32_t write = m_WriteIndex;
        uint32_t read = m_ReadIndex;

        if (m_Ring != NULL) {
            const uint32_t pending = openaes67_frames_pending(
                m_Ring->writeIndex, m_Ring->readIndex, m_Ring->capacityFrames);
            const uint32_t want = capacityFrames / 10;   // one packet per tick
            uint32_t taken = pending < want ? pending : want;

            int16_t *dst = static_cast<int16_t *>(m_Buffer);
            const int16_t *src = reinterpret_cast<const int16_t *>(
                reinterpret_cast<const uint8_t *>(m_Ring) + openaes67_render_offset());
            uint32_t srcPos = m_Ring->readIndex % m_Ring->capacityFrames;

            for (uint32_t i = 0; i < taken; ++i) {
                const uint32_t frame = (write + i) % capacityFrames;
                dst[frame * 2 + 0] = src[srcPos * 2 + 0];
                dst[frame * 2 + 1] = src[srcPos * 2 + 1];
                srcPos = (srcPos + 1) % m_Ring->capacityFrames;
            }
            if (taken < want) {
                for (uint32_t i = taken; i < want; ++i) {
                    const uint32_t frame = (write + i) % capacityFrames;
                    dst[frame * 2 + 0] = 0;
                    dst[frame * 2 + 1] = 0;
                }
                ++m_Ring->renderUnderruns;
            }
            m_Ring->readIndex = m_Ring->readIndex + taken;
            write += taken;
        } else {
            write += capacityFrames / 10;   // silence
        }
        m_WriteIndex = write % capacityFrames;
        m_ReadIndex = read % capacityFrames;
    }

    LONG                        m_RefCount;
    PVOID                       m_Buffer;
    PMDL                        m_Mdl;
    ULONG                       m_BufferBytes;
    KSSTATE                     m_State;
    ULONGLONG                   m_PlayedFrames;
    ULONGLONG                   m_StartedAt;
    KTIMER                      m_Timer;
    KDPC                        m_Dpc;
    OpenAes67SharedAudio       *m_Ring = NULL;
    PVOID                       m_RingBase = NULL;
    HANDLE                      m_SectionHandle = NULL;
    uint32_t                    m_WriteIndex = 0;
    uint32_t                    m_ReadIndex = 0;
};

// --------------------------------------------------------------- wave miniport

class CMiniportWaveRT : public IMiniportWaveRT
{
public:
    CMiniportWaveRT() : m_RefCount(1), m_Port(NULL) {}

    STDMETHODIMP_(NTSTATUS) QueryInterface(REFIID riid, PVOID *ppv) override
    {
        if (!ppv) return STATUS_INVALID_PARAMETER;
        *ppv = NULL;
        if (IsEqualGUIDAligned(riid, IID_IUnknown) ||
            IsEqualGUIDAligned(riid, IID_IMiniport) ||
            IsEqualGUIDAligned(riid, IID_IMiniportWaveRT)) {
            *ppv = static_cast<IMiniportWaveRT *>(this);
            AddRef();
            return STATUS_SUCCESS;
        }
        return STATUS_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override
    {
        return static_cast<ULONG>(InterlockedIncrement(&m_RefCount));
    }
    STDMETHODIMP_(ULONG) Release() override
    {
        const LONG remaining = InterlockedDecrement(&m_RefCount);
        if (remaining == 0) delete this;
        return static_cast<ULONG>(remaining);
    }

    STDMETHODIMP_(NTSTATUS) Init(PUNKNOWN adapterUnknown,
                                 PRESOURCELIST resourceList,
                                 PPORTWAVERT port) override
    {
        UNREFERENCED_PARAMETER(adapterUnknown);
        UNREFERENCED_PARAMETER(resourceList);
        m_Port = port;
        return STATUS_SUCCESS;
    }

    STDMETHODIMP_(NTSTATUS) GetDescription(PPCFILTER_DESCRIPTOR *description) override
    {
        if (!description) return STATUS_INVALID_PARAMETER;
        *description = &g_WaveFilter;
        return STATUS_SUCCESS;
    }

    STDMETHODIMP_(NTSTATUS) DataRangeIntersection(ULONG pinId,
                                                  PKSDATARANGE dataRange,
                                                  PKSDATARANGE matchingDataRange,
                                                  ULONG outputBufferLength,
                                                  PVOID resultantFormat,
                                                  PULONG resultantFormatLength) override
    {
        UNREFERENCED_PARAMETER(pinId);
        UNREFERENCED_PARAMETER(dataRange);
        UNREFERENCED_PARAMETER(matchingDataRange);

        const ULONG needed = sizeof(KSDATAFORMAT_WAVEFORMATEX);
        if (resultantFormatLength != NULL) {
            *resultantFormatLength = needed;
        }
        if (outputBufferLength < needed || resultantFormat == NULL) {
            return STATUS_BUFFER_TOO_SMALL;
        }

        KSDATAFORMAT_WAVEFORMATEX *out =
            static_cast<KSDATAFORMAT_WAVEFORMATEX *>(resultantFormat);
        RtlZeroMemory(out, needed);
        out->DataFormat.FormatSize = needed;
        out->DataFormat.MajorFormat = KSDATAFORMAT_SUBTYPE_WAVEFORMATEX;
        out->DataFormat.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
        out->DataFormat.SampleSize = 4;
        out->WaveFormatEx.wFormatTag = WAVE_FORMAT_PCM;
        out->WaveFormatEx.nChannels = 2;
        out->WaveFormatEx.nSamplesPerSec = 48000;
        out->WaveFormatEx.wBitsPerSample = 16;
        out->WaveFormatEx.nBlockAlign = 4;
        out->WaveFormatEx.nAvgBytesPerSec = 48000 * 4;
        return STATUS_SUCCESS;
    }

    STDMETHODIMP_(NTSTATUS) NewStream(PMINIPORTWAVERTSTREAM *stream,
                                      PPORTWAVERTSTREAM portStream,
                                      ULONG pin,
                                      BOOLEAN capture,
                                      PKSDATAFORMAT dataFormat) override
    {
        UNREFERENCED_PARAMETER(portStream);
        UNREFERENCED_PARAMETER(pin);
        if (!stream || !dataFormat) return STATUS_INVALID_PARAMETER;
        if (capture) return STATUS_NOT_SUPPORTED;   // render only, for now
        if (m_CaptureOnly) return STATUS_NOT_SUPPORTED;

        CMiniportWaveRTStream *created = new CMiniportWaveRTStream();
        if (!created) return STATUS_INSUFFICIENT_RESOURCES;
        const NTSTATUS status = created->SetFormat(dataFormat);
        if (!NT_SUCCESS(status)) {
            created->Release();
            return status;
        }
        // Best effort: the pump may not be running yet. A stream without the
        // ring still works, it just plays silence.
        created->OpenRing(L"\\BaseNamedObjects\\OpenAES67Audio");

        *stream = static_cast<PMINIPORTWAVERTSTREAM>(created);
        return STATUS_SUCCESS;
    }

    STDMETHODIMP_(NTSTATUS) GetDeviceDescription(PDEVICE_DESCRIPTION description) override
    {
        if (!description) return STATUS_INVALID_PARAMETER;
        RtlZeroMemory(description, sizeof(*description));
        description->Version = DEVICE_DESCRIPTION_VERSION;
        description->Master = TRUE;
        description->ScatterGather = TRUE;
        description->Dma32BitAddresses = FALSE;
        description->InterfaceType = PCIBus;
        description->MaximumLength = 128 * 1024;
        return STATUS_SUCCESS;
    }

private:
    LONG          m_RefCount;
    PPORTWAVERT   m_Port;
    bool          m_CaptureOnly = false;
};
// Factory placed after the class definition so the adapter can create it.
PUNKNOWN CreateWaveMiniport()
{
    CMiniportWaveRT *miniport = new CMiniportWaveRT();
    return static_cast<PUNKNOWN>(miniport);
}
