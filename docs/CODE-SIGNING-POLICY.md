# Code signing policy

**Free code signing provided by [SignPath.io](https://about.signpath.io), certificate by [SignPath Foundation](https://signpath.org).**

This page states which binaries are signed, who is allowed to approve a signing
request, and what the software does with the user's system and data. It exists
because a signature is only as meaningful as the policy behind it.

> Status: the project has applied for the SignPath Foundation programme. Until a
> signature is present, every release note says **unsigned**, and the reason is in
> [`CODE-SIGNING.md`](CODE-SIGNING.md).

## What gets signed

| Artifact | Signed | Notes |
|---|---|---|
| `openaes67.exe` | yes | the CLI |
| `openaes67_selftest.exe` | yes | the test binary shipped with it |
| `openaes67_gui.exe` | yes | Qt 6 GUI |
| `openaes67_asio64.dll` | yes | the 64-in/64-out ASIO sound card |
| Qt runtime DLLs (`Qt6*.dll`, plugins) | no | upstream LGPL binaries, redistributed unmodified; see [`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md) |
| `driver\*` (WDM/PortCls) | no | needs WDK + test signing, not part of a normal release |

Builds are produced by the GitHub Actions workflow in
[`.github/workflows/ci.yml`](../.github/workflows/ci.yml) from the tagged commit;
the archives in a release are assembled from those CI artifacts, and each archive
ships a `sha256.txt`.

## Team roles

This is a single-maintainer project; the roles are named anyway so that a signing
request has an explicit approver rather than an implied one.

| Role | Who | Responsibility |
|---|---|---|
| Committers | [@ylzwd](https://github.com/ylzwd) | may push to `main`; every change from someone else arrives as a reviewed pull request |
| Reviewers | [@ylzwd](https://github.com/ylzwd) | reviews pull requests before merge; branch protection requires the `build + self-test (windows)` and `policy checks` jobs to pass |
| Approvers | [@ylzwd](https://github.com/ylzwd) | approves each signing request for a specific release built from a specific commit |

Multi-factor authentication is enabled on the repository account.

## What is signed, and when

- A signing request is created **after** CI has passed on the exact commit being
  released, and the artifacts come from that run — not from a developer machine.
- Each request needs manual approval; there is no automatic "sign whatever was
  pushed" path.
- The signed files are verified after signing (`tools/sign-binaries.ps1`), and the
  release notes record the signer so users can check a download themselves.

## Privacy

**This program will not transfer any information to other networked systems
unless specifically requested by the user or the person installing or operating
it.**

Concretely:

- No telemetry, no analytics, no update check, no licence server, no account.
- The program sends AES67 media, SAP announcements, PTP and (optionally) RTCP to
  the addresses the user configures — that is the function of the program, not
  data collection. Nothing is sent to a server belonging to this project.
- `doctor` performs local checks; it opens no connections other than the ones the
  user's configuration implies.
- The GUI stores its settings in the user's profile; nothing leaves the machine.
- Third-party components: Qt 6 (LGPL-3.0) is used by the GUI. See
  [`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md) for the full list and their
  licences. None of them adds telemetry to this program.

## System changes and uninstallation

- The CLI and GUI change nothing outside the user's profile unless asked.
- `asio\register-asio-driver.bat` **writes to `HKLM`** (the ASIO driver registry
  key and its COM class) and therefore requires an elevated prompt. It says so
  before doing anything.
- `asio\unregister-asio-driver.bat` removes exactly those keys; it needs the same
  elevation. Both scripts are in the archive, so uninstallation never depends on
  an installer.
- The WDM driver, when built by the user, is installed and removed with the
  scripts in `driver\`, which also state that test signing is required.

## What is not in this project

- No malware, adware, or "potentially unwanted" behaviour.
- No security-scanner or exploit tooling: OpenAES67 moves audio over RTP and
  synchronises clocks; it does not probe or attack other systems.
- No proprietary component: all source is MIT, the ASIO interface is implemented
  from the published ABI (`asio_driver/asio_abi.h`) rather than Steinberg's
  non-redistributable SDK, and third-party runtime binaries are upstream OSS.

## Reporting a problem with a signed binary

If a file signed for this project does not match the source in this repository,
open an issue or use the private reporting described in
[`SECURITY.md`](../SECURITY.md). Include the signer name and the SHA-256 of the
file.
