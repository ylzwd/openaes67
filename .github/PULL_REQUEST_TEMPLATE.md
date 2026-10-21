## What this changes

<!-- One paragraph. Link the issue if there is one: "Fixes #12". -->

## Why

<!-- The problem, not the patch. If this is a behaviour change, say what breaks. -->

## Checklist

- [ ] Behaviour is traceable to a public standard, or I have explained why not
- [ ] Numbers in the description come from a reproducible measurement
- [ ] `build.bat` passes and prints `ALL CLAIMS HOLD`
- [ ] `build\selftest.exe` prints `ALL PASS` (and I updated the assertion count in
      the two READMEs and `CONTRIBUTING.md` if I added assertions)
- [ ] `tools\scan-secrets.ps1` exits 0
- [ ] I ran `tools\verify-perf.ps1` if this touches the receive loop, the shared
      ring or the ASIO pump
- [ ] New CLI options appear in `usage()` **and** in the README's CLI reference
      (the build fails if they do not)
- [ ] No vendor names, licence checks or telemetry were added

## Measured effect

<!-- For a performance or accuracy change: before -> after, with the command used. -->
