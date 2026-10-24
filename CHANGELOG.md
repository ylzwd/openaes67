# Changelog

Notable changes per release. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
[Semantic Versioning](https://semver.org/spec/v2.0.0.html), with the usual
pre-1.0 caveat that minor versions may break compatibility.

Full engineering notes — measurements, dead ends, and why each decision was made
— live in [`docs/FINDINGS.md`](docs/FINDINGS.md).

## [Unreleased]

### Added

- `tools/sign-binaries.ps1`: Authenticode signing and verification, with the PFX
  password taken from the environment and an explicit failure when a signature is
  present but untrusted.
- `docs/CODE-SIGNING.md`: why the released binaries are unsigned, what SmartScreen
  actually checks, and what each signing option costs.
- `-Sign` on `tools/release.ps1`: signs the packaged binaries after packaging and
  before zipping, and aborts the release if verification fails.
- `docs/AES67-CONFORMANCE.md`: clause-by-clause analysis against AES67-2018 and
  ST 2110-30, naming what is not implemented and why.
- DSCP marking: audio at EF (46), PTP at CS6 (48). Verified by a `getsockopt`
  round trip in the self-test, not by trusting `setsockopt`.
- A startup note when a flow exceeds the AES67 8-channel limit.

## [0.1.1] — 2026-10-24

### Fixed

- A stream without `--group` failed with "group is not a multicast address"
  although the help text promised a default; the default now exists.
- The packet-size warning assumed L16, so L24 flows were reported at two thirds
  of their real size. It now follows the codec and prints what to change.
- The SDP parser accepted any positive channel count, so a forged SAP
  announcement could describe a stream this build cannot carry. Bounded to
  1–64 channels and 8–384 kHz.
- The WAV reader had the same hole: a 44-byte header could claim 2000 channels
  at 1 Hz.
- ASIO devices listed by `list-devices` could not be selected by name.
- SAP announcements with a unicast `c=` line were treated as usable.

### Added

- Deterministic mutation tests for the SDP, SAP, RTP, RTCP and PTP parsers, and
  truncation sweeps for the SAP and WAV readers (assertions 283 → 320).
- `verify-claims.ps1` runs the CLI and reads its output, so a documented default
  that drifts from the code fails the build.
- SPDX headers on every source file, a trademark notice, and third-party notices
  shipped inside both archives.

### Changed

- Repository moved to `ylzwd/openaes67`.

## [0.1.0] — 2026-10-21

First public release.

### Added

- AES67 / ST 2110-30 RTP (L16, L24), 1–64 channels, 8 kHz – 384 kHz
- SDP generation and parsing, SAP announcement and discovery
- PTPv2 slave (E2E), with the frequency estimate feeding the rate controller
- RTCP sender and receiver reports
- Jitter buffer with reordering and late/duplicate detection
- WAV, WASAPI (shared and exclusive) and a 64-in / 64-out ASIO sound card
- `openaes67` CLI with `doctor`, `probe`, `info`, `tx`, `rx`, `send`, `recv`,
  `bridge`, `capture`, `capture-in`, `play`, `sap-listen`, `ptp`, `shm-read`,
  `list-devices`, `bench`, `version`
- Qt 6 GUI with live statistics, tray icon and start-with-Windows
- 283 self-test assertions, a build gate, a performance gate and a secret scanner

[Unreleased]: https://github.com/ylzwd/openaes67/compare/v0.1.1...HEAD
[0.1.1]: https://github.com/ylzwd/openaes67/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/ylzwd/openaes67/releases/tag/v0.1.0
