# Third-party notices

OpenAES67 itself is MIT-licensed (see [LICENSE](../LICENSE)). This file records
the third-party components that ship **inside** the release archives, because
the licence of a redistributed binary matters as much as the licence of the
source.

---

## In every archive

| Component | Why it is included | Licence |
|---|---|---|
| OpenAES67 (CLI, self-test, ASIO sound card) | the project itself | MIT |
| Microsoft Visual C++ runtime | MSVC links it dynamically; `windeployqt --compiler-runtime` copies `vcruntime140.dll` and `msvcp140.dll` | Microsoft EULA (redistributable, see below) |

The MSVC runtime is redistributed under the Visual Studio 2022 redistributable
terms. Its licence forbids using the name "Visual Studio" for the product and
requires that the Microsoft notice be included. The archive ships
`openaes67.exe` and its DLLs, neither of which uses the name.

## GUI archive only

| Component | Why it is included | Licence |
|---|---|---|
| Qt 6 (Core, Gui, Widgets, Network) | the optional GUI front end | LGPL v3 **or** GPL v3 **or** commercial |
| Qt platform plugin `platforms/qwindows.dll` | required for the GUI to start | LGPL v3, same terms as Qt |
| Qt transitive dependencies shipped by `windeployqt` (ANGLE, `d3dcompiler_47.dll`, `libEGL`/`libGLESv2`, `opengl32sw`, image format plugins, the `MSVC` runtime) | pulled in by the platform plugin | see Qt's `LICENSES/` directory in the kit |

### Why LGPL v3 is satisfied here

Qt is dual-licensed. This project uses the **LGPL v3** option, which is
permitted when:

1. **Dynamic linking only.** `CMakeLists.txt` links `Qt6::Widgets` and
   `Qt6::Network` as shared libraries; nothing links Qt statically. A user can
   substitute their own Qt build by dropping a different
   `Qt6Core.dll` / `Qt6Gui.dll` / `Qt6Widgets.dll` beside the executable.
2. **Relinkability is preserved.** No obfuscation, no patched build, no
   signature that would block substitution.
3. **The Qt licence notice travels with the archive.** The GUI archive contains
   a `LICENSES/` directory with the full text of LGPL-3.0-only, GPL-3.0-only
   and GPL-2.0-only, and a `QT-SBOM/` directory holding the kit's SPDX bill of
   materials, which records which licence applies to which Qt module.

Because the archive contains Qt as separate DLLs rather than object code inside
`openaes67_gui.exe`, LGPL section 4 (reverse engineering for debugging
modifications) and section 6 (the "Installation Information" requirement) do not
impose additional obligations on this project.

### Where the licence text comes from

The binary Qt kit that `aqtinstall` fetches does **not** contain a `LICENSES/`
directory -- only the Qt sources do. The CI job therefore takes the licence
text from the Qt sources of the exact version it built against:

    https://raw.githubusercontent.com/qt/qtbase/v6.8.3/LICENSES/LGPL-3.0-only.txt

The version is pinned to the one in `ci.yml`, so the text always matches the
binaries beside it. The SPDX bill of materials, which the kit does ship under
`sbom/`, is copied in as well: it names the licence of every module, so a
reader can check that the modules in this archive are the LGPL ones.

If the Qt version in `ci.yml` changes, the version in the download URL must
change with it. `verify-claims.ps1` asserts that both mention the same
version, so a stale URL cannot ship a mismatched licence.

---

## Deliberately NOT included

| Component | Why not |
|---|---|
| Steinberg ASIO SDK headers | not redistributable. This project declares the ASIO ABI itself in `asio_driver/asio_abi.h` and ships no Steinberg source. See `asio_driver/README.md`. |
| Any vendor SDK, DLL or firmware | zero vendor code is committed, and CI enforces it |
| A static Qt build | would require a commercial or GPL licence and is not produced |

---

## Verifying an archive

```powershell
Expand-Archive openaes67-<version>-gui-win64.zip -DestinationPath x
Get-ChildItem x -Recurse -Filter *.dll | Select-Object Name
Get-ChildItem x -Recurse -Include LICENSE*,COPYING*,NOTICE*
```

If the last command returns nothing, the archive is missing its third-party
notices and should not be published until `NOTICE.md` is added to it.

## Keeping this file accurate

Any change that adds a bundled component must update this file in the same
commit. `tools/release.ps1` copies `docs\*.md` into both archives, so this
file ships automatically.