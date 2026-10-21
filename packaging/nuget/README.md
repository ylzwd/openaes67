# Publishing the NuGet package

The package is a thin wrapper around the prebuilt Windows binaries: no code is
compiled, the `.csproj` only says which files go where. It exists so the binaries
can be resolved by a package manager, not because anything here is .NET.

## What goes in

```
tools\openaes67.exe                 CLI
tools\openaes67_selftest.exe        self-test
tools\asio\openaes67_asio64.dll     64-in / 64-out ASIO sound card
tools\asio\register-asio-driver.bat elevated, once
tools\asio\README.md                how the registry entry works
README.md, LICENSE
```

## Pack

The binaries must exist first:

```bat
build.bat
asio_driver\build-asio-driver.bat
```

then:

```bat
cd packaging\nuget
dotnet pack -c Release
```

The result is `packaging\nuget\out\OpenAES67.<version>.nupkg`. The version comes
from `OpenAES67.csproj` and must match `include/openaes67/version.h`; the build gate
checks the header and the binary, and this file is checked by eye at release time
(there is no cheap way to make MSBuild read a C++ header).

## Publish to GitHub Packages

Needs a token with `write:packages`, which is not part of the default `gh` login:

```bat
gh auth refresh -h github.com -s write:packages
```

```powershell
$token = gh auth token
dotnet nuget push packaging\nuget\out\OpenAES67.0.1.0.nupkg `
    --source https://nuget.pkg.github.com/ylzwd123/index.json `
    --api-key $token
```

`gh auth token` prints the token to the terminal; `dotnet nuget push --api-key`
consumes it as an argument. That is how the CLI is designed — do not paste the
token into a shell history you keep.

## Install

```bat
dotnet nuget add source https://nuget.pkg.github.com/ylzwd123/index.json ^
    --name openaes67 --username <github-user> --password <PAT-with-read:packages>
nuget install OpenAES67 -Source openaes67
```

**GitHub Packages always requires authentication, even for public packages.** That
is a GitHub restriction, not a choice here, and it is why the release zip remains
the recommended path for end users. This feed is for CI systems and for people who
want version pinning.

## Caveat

A NuGet package that only carries `tools\` is a distribution channel, not a library:
`dotnet add package` will not give you anything callable. Do not present it as an
SDK.
