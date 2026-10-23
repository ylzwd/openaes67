// SPDX-License-Identifier: MIT
// Copyright (c) 2026 OpenAES67 contributors
//
// Every behaviour in this file is traceable to a published standard
// (AES67-2018, RFC 3550, RFC 4566, RFC 2974, IEEE 1588) or to a
// reproducible measurement recorded in docs/FINDINGS.md.

/*
 * Shared-memory ABI between the OpenAES67 kernel audio driver and the
 * user-mode pump. C-compatible on purpose: the same header is included by the
 * driver (kernel) and by the user-mode service.
 *
 * Layout
 * ------
 *   [ OpenAes67SharedAudio header ]
 *   [ render  buffer: capacityFrames * frameBytes ]
 *   [ capture buffer: capacityFrames * frameBytes ]
 *
 * The two buffers are placed immediately after the header, render first.
 *
 * Indices
 * -------
 * All indices are frame counts, monotonically increasing, wrapping at 2^32.
 * Free-running counters avoid the "full vs empty" ambiguity of head==tail, and
 * unsigned subtraction gives the correct distance across a wrap.
 *
 *   writeIndex   written by user mode, read by the driver   (render direction)
 *   readIndex    written by the driver,    read by user mode
 *   captureWrite written by the driver,    read by user mode (capture direction)
 *   captureRead  written by user mode,     read by the driver
 *
 * Memory ordering
 * ---------------
 * Indices are published with release semantics after the data copy and read
 * with acquire semantics before it. In the kernel that is
 * KeMemoryBarrier()/InterlockedExchange; in user mode it is
 * std::atomic_ref / InterlockedExchange. The fields are declared volatile here
 * only so neither compiler caches them across the barrier.
 */
#ifndef OPENAES67_SHARED_AUDIO_H
#define OPENAES67_SHARED_AUDIO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OPEN_AES67_SHM_MAGIC 0x4F413637u /* "OA67" */
#define OPEN_AES67_SHM_VERSION 1u

/* Header flag bits. */
#define OPEN_AES67_FLAG_RUNNING 0x00000001u /* driver stream is running */
#define OPEN_AES67_FLAG_UNDERRUN 0x00000002u /* driver ran dry (render) */
#define OPEN_AES67_FLAG_OVERRUN 0x00000004u /* driver dropped input (capture) */

typedef struct OpenAes67SharedAudio {
    uint32_t magic;          /* OPEN_AES67_SHM_MAGIC                         */
    uint32_t version;        /* OPEN_AES67_SHM_VERSION                       */
    uint32_t frameBytes;     /* channels * sizeof(int16_t)                   */
    uint32_t channels;
    uint32_t sampleRate;
    uint32_t capacityFrames; /* per direction                               */
    uint32_t flags;

    volatile uint32_t writeIndex;
    volatile uint32_t readIndex;
    volatile uint32_t captureWrite;
    volatile uint32_t captureRead;

    /* Driver-side counters for diagnostics; user mode may read them. */
    volatile uint32_t renderUnderruns;
    volatile uint32_t captureOverruns;
    uint32_t reserved[5];
} OpenAes67SharedAudio;

/*
 * Compile-time ABI lock.
 *
 * This header is included by both the kernel driver and user mode. The offsets
 * below are the contract: the driver writes at sizeof(header) and user mode reads
 * there, so if either side ever changes packing or adds a field, the build must
 * fail instead of producing silent channel rotation at run time (a 8-byte slip
 * rotates an 8-channel stream by four channels).
 */
#if defined(__cplusplus)
static_assert(sizeof(OpenAes67SharedAudio) == 72,
              "OpenAes67SharedAudio changed size: the shared-memory ABI is broken");
static_assert(offsetof(OpenAes67SharedAudio, channels) == 12, "channels offset changed");
static_assert(offsetof(OpenAes67SharedAudio, capacityFrames) == 20,
              "capacityFrames offset changed");
static_assert(offsetof(OpenAes67SharedAudio, writeIndex) == 28, "writeIndex offset changed");
static_assert(offsetof(OpenAes67SharedAudio, readIndex) == 32, "readIndex offset changed");
#else
_Static_assert(sizeof(OpenAes67SharedAudio) == 72,
               "OpenAes67SharedAudio changed size: the shared-memory ABI is broken");
#endif

/* Byte offset of the render buffer from the start of the mapping. */
static inline uint32_t openaes67_render_offset(void)
{
    return (uint32_t)sizeof(OpenAes67SharedAudio);
}

/* Byte offset of the capture buffer from the start of the mapping. */
static inline uint32_t openaes67_capture_offset(uint32_t capacityFrames,
                                                uint32_t frameBytes)
{
    return (uint32_t)sizeof(OpenAes67SharedAudio) + capacityFrames * frameBytes;
}

/* Total mapping size for a given capacity. */
static inline uint32_t openaes67_mapping_size(uint32_t capacityFrames,
                                              uint32_t frameBytes)
{
    return (uint32_t)sizeof(OpenAes67SharedAudio) + 2u * capacityFrames * frameBytes;
}

/* Frames available to read/write, given two free-running counters. */
static inline uint32_t openaes67_frames_pending(uint32_t writeIndex,
                                                uint32_t readIndex,
                                                uint32_t capacityFrames)
{
    const uint32_t used = writeIndex - readIndex;
    return used > capacityFrames ? capacityFrames : used;
}

static inline uint32_t openaes67_frames_free(uint32_t writeIndex,
                                             uint32_t readIndex,
                                             uint32_t capacityFrames)
{
    return capacityFrames - openaes67_frames_pending(writeIndex, readIndex,
                                                     capacityFrames);
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPENAES67_SHARED_AUDIO_H */
