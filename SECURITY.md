# Security policy

## Supported versions

The project is pre-1.0; only the latest `main` is supported. Fixes land on `main`
and are not backported.

| Version | Supported |
|---|---|
| `main` (0.5.x) | ✅ |
| anything older | ❌ |

## Reporting a vulnerability

Please **do not open a public issue**. Use GitHub's private reporting:

1. Go to the [Security tab](https://github.com/ylzwd123/openaes67/security)
2. **Report a vulnerability**
3. Describe the impact and, if you can, the smallest reproduction

You will get an initial response within a few days. This is a spare-time project,
so please do not expect a paid-response SLA.

## What is in scope

OpenAES67 opens sockets, parses untrusted network input and writes files. The
interesting attack surface is therefore:

- **RTP / SDP / SAP parsers** — malformed packets, oversized fields, integer
  overflow in the length math, and stack or heap writes driven by packet content
- **SAP announcements** — a forged announcement that makes the receiver join an
  unexpected group, or a body size that the parser mishandles
- **WAV reader** — a crafted file that makes the reader allocate without bound or
  read past the chunk
- **ASIO driver** — a malicious or buggy host calling into the DLL, and the shared
  memory ring being written out of bounds by either side
- **Installer / registration scripts** — anything that would let a low-privileged
  user influence what gets registered under `HKLM`

## What is not a vulnerability

- **AES67 receivers do not appear in Dante Controller.** That is a property of
  Audinate's protocol, documented in
  [`docs/DANTE-VISIBILITY.md`](docs/DANTE-VISIBILITY.md), not a defect.
- **A stream is not encrypted or authenticated.** AES67 has no transport security;
  anyone on the L2 segment can inject audio. That is the standard's design.
- **PTP uses software timestamps**, so the clock accuracy is bounded by the OS.
- **The driver cannot be loaded with Secure Boot on** unless test signing is
  enabled. This is documented and expected.

## Handling

Security fixes are prepared in a private fork, released together with an advisory,
and credited to the reporter unless they ask otherwise. There is no bug bounty.
