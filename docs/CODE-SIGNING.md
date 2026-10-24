# Code signing

Status: **the released binaries are unsigned.** Downloading an `.exe` from this
project therefore shows "Windows protected your PC" (SmartScreen), and an
elevated `asio\register-asio-driver.bat` runs unsigned code from a path the user
chose. This document explains why that is the current state, what would change
it, and what the repository already contains so that signing is a matter of
supplying a certificate rather than writing plumbing.

## What SmartScreen actually reacts to

Two different mechanisms are often confused:

| Mechanism | What it checks | What removes the prompt |
|---|---|---|
| Authenticode signature | the file has a signature chained to a **publicly trusted** CA | a certificate from a trusted CA |
| SmartScreen reputation | Microsoft's telemetry about this publisher and file | reputation, which accrues to an **OV/EV certificate over time** |

A self-signed certificate satisfies neither for the general public: Windows
reports it as an unknown publisher unless the certificate was installed on that
machine. Signing with a real certificate removes the "unknown publisher" line but
a brand-new certificate still has no reputation, so SmartScreen may keep warning
until enough downloads accumulate. EV certificates historically skipped the
reputation wait; since the 2023 CA/Browser Forum rules, new OV and EV
certificates are both issued to hardware or cloud HSM, which is the part that
affects CI.

## Options, with the real costs

| Option | Cost | CI-friendly | Removes the prompt ? |
|---|---|---|---|
| **Do nothing** | 0 | — | No |
| **Self-signed** (`New-SelfSignedCertificate`) | 0 | Yes | No — only on machines that installed the certificate |
| **Azure Trusted Signing** (Microsoft) | ~$10/month, individual identity validation | Yes (API, no hardware) | Yes, after identity validation; reputation builds |
| **OV certificate** (Certum, SSL.com, Sectigo …) | ~$100–300/year | No — key lives on a token/HSM; signing from CI needs SignPath or a self-hosted runner with the token | Yes |
| **EV certificate** | ~$300–600/year | Same hardware constraint | Yes, and historically immediate reputation |
| **SignPath.io for OSS** | free for approved open-source projects | Yes (build integration) | Yes, once approved |

For a project at this stage the honest ranking is: **Azure Trusted Signing**, then
**SignPath OSS** (free, but approval is not guaranteed and does not happen in an
afternoon), then a paid OV certificate. A self-signed certificate is a pipeline
test, not a solution.

## What is already in the repository

`tools/sign-binaries.ps1` signs and verifies, and refuses to pretend:

```bat
:: sign with a certificate in the store (test or real)
powershell -File tools\sign-binaries.ps1 -Path build\openaes67.exe,build\selftest.exe ^
    -Thumbprint <sha1>

:: sign with a PFX; the password comes from an environment variable, never argv
set OPENAES67_PFX_PASSWORD=...
powershell -File tools\sign-binaries.ps1 -Path build\openaes67.exe -PfxPath cert.pfx

:: check what a package carries; exit code 1 if anything is unsigned
powershell -File tools\sign-binaries.ps1 -Path openaes67.exe,asio\openaes67_asio64.dll -VerifyOnly
```

Design decisions worth knowing:

- **Verification is separate from signing, and is mandatory in a release.** A
  signing step that silently does nothing produces a release whose notes claim a
  signature and whose users see a warning. The script asks
  `Get-AuthenticodeSignature` and prints the signer.
- **`UnknownError` is not "fine".** It is what Windows reports when the signature
  is intact but the chain is untrusted — the self-signed case. The script says so
  explicitly and fails, unless `-AllowUntrusted` is passed (pipeline tests only).
- **The PFX password is read from an environment variable**, because a password
  passed on the command line ends up in the process list, in shell history, and in
  CI logs.
- **RFC 3161 timestamping** (`/tr`) is on by default, so signatures stay valid
  after the certificate expires.

## How it would be wired in

Signing belongs **after** packaging and **before** compressing:

```
build -> package staging dir -> sign each .exe/.dll -> verify -> zip -> upload
```

An archive cannot be signed meaningfully after the fact (the signature covers the
file, not the zip), and re-zipping after signing would invalidate nothing but does
invite "I signed the wrong copy" mistakes. `tools/release.ps1` takes `-Sign`, which
inserts exactly that step and aborts the release if verification fails.

## What would have to change for releases to be signed

1. Obtain a certificate (Azure Trusted Signing or an OV certificate).
2. Store it where CI can reach it: a PFX in a repository secret for a cloud
   signing service, or a token on a self-hosted runner for an OV certificate.
3. Add the `-Sign` step to the release job with the secret injected as
   `OPENAES67_PFX_PASSWORD` (never as a command-line argument).
4. Re-run the verification step on the packaged files, and put the signer
   thumbprint in the release notes so users can check it themselves.

Until all four are done, the release notes say **unsigned**, and that is not going
to be quietly dropped from them.
