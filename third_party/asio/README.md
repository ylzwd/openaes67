# ASIO SDK goes here

This directory is intentionally empty in the repository: the Steinberg ASIO SDK is
not redistributable, so it is never committed or bundled.

To enable the ASIO backend, download the ASIO SDK from Steinberg (accepting their
licence) and copy these three files from its `common/` directory:

    iasiodrv.h
    asio.h
    asiosys.h

Then rebuild:

    .\build.bat asio
    # or: cmake -S . -B build -DOPENAES67_WITH_ASIO=ON

Both builds fail with an explicit message when the header is missing, instead of
quietly producing a binary without ASIO. See docs/AUDIO-DEVICES.md.