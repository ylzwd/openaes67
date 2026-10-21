# Icon set

Everything here is generated from [`logo.png`](../logo.png) by
[`tools/make-icon.ps1`](../tools/make-icon.ps1). Do not edit these files by hand:
change `logo.png` and regenerate.

```powershell
powershell -ExecutionPolicy Bypass -File tools\make-icon.ps1
powershell -ExecutionPolicy Bypass -File tools\check-icons.ps1
```

`check-icons.ps1` parses the generated files as bytes (ICO directory, each entry,
the DIB header, PNG signature) and verifies that the executables really carry the
icon and version resources. It is deliberately independent of the generator.

---

## What to use where

| File | Contents | Use it for |
|---|---|---|
| `openaes67.ico` | mark only, 10 sizes (16–256) | **The application icon.** Embedded in every `.exe` via `app.rc`; also the tray icon, the About box, and anything that shows "the app" |
| `openaes67-full.ico` | mark + word, 10 sizes | Places where the name should be readable without a label: installer header, website, docs |
| `png/openaes67-<n>.png` | mark only, transparent, 16–512 | Anywhere a single raster is needed: README, GUI splash, slides, favicons, installer art |
| `png/openaes67-full-<n>.png` | mark + word, transparent, 16–512 | Same, when the wordmark should appear |

Sizes inside the `.ico` files: **16, 20, 24, 32, 40, 48, 64, 96, 128, 256**.
20 and 40 exist because Windows asks for them at 125 % and 150 % display scaling.
PNG exports add **512** for stores and print.

### Front end (GUI)

`app.rc` is linked into `openaes67.exe`, `openaes67_gui.exe` and
`openaes67_selftest.exe`, so Windows picks the right size automatically for the
title bar, taskbar, Alt+Tab, desktop shortcuts and File Explorer. Qt inherits the
executable icon, so the window and taskbar show it without extra code.

If a future control needs an explicit image (About dialog, tray, splash), load
`assets/png/openaes67-<size>.png` rather than inventing a new size.

### Back end (services, driver, installer)

- The kernel driver has no icon resource; the driver package uses
  `openaes67-full.ico` for installer and Device Manager-adjacent material.
- A Windows service has no icon either — the tray/notification icon is the
  front end's job and uses `openaes67.ico`.
- Installers should take `48`/`64` for the wizard and `256` for
  Apps & Features.

---

## Why the application icon is the mark alone

The logo is a rounded-square mark with the word "OpenAES67" underneath. At 16 px
the word collapses into an illegible smudge — verified by rendering the sizes on
a dark background. So the application icon drops the word at every size and keeps
the mark, while `openaes67-full.ico` preserves the wordmark for large placements.

The split between mark and word is **detected**, not hard-coded: the first mostly
empty row in the lower half of the image is the gap between them. On the current
logo that lands at y=107, and the mark's tight box is 82×84 at (54, 23).

## Why the background is transparent

`logo.png` is an opaque white bitmap with no alpha. Keying *every* near-white
pixel to transparency also punches out the mark's inner white panel, leaving a
hole in the middle of the icon — visible as a dark blob on a dark taskbar. The
generator therefore floods from the border and only removes white that is
connected to the outside, which keeps the inner panel intact. `check-icons.ps1`
asserts both properties: corners transparent, centre opaque.

## Why 256 is PNG and the rest are DIB

Windows accepts PNG-compressed entries in `.ico` since Vista, and Explorer reads
them. But `System.Drawing` — and every .NET tool built on it — fails to extract a
specific size from a file whose entries are all PNG ("requested range extends
past the end of the array"). Sizes below 256 are therefore stored as
`BITMAPINFOHEADER` + bottom-up BGRA + 1bpp AND mask, and only 256 uses PNG. That
is also the layout Windows itself produces.

The AND mask is generated from alpha, so the icons render correctly on any
background.
