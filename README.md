# WDL/SWELL with Linux accessibility support

This repository publishes a Linux build of SWELL with accessibility support
enabled through AccessKit and AT-SPI. It is intended for testing and using
REAPER-style SWELL applications with Linux screen readers and other assistive
technology.

The latest release build of `libSwell.so` is built automatically from `main`:

https://github.com/RDMurray/WDL/releases/latest/download/libSwell.so

## Building locally

This checkout expects a sibling AccessKit checkout at `../accesskit`, matching
the Rust shim path dependencies in `WDL/swell/rust/accesskit_shim/Cargo.toml`.

To build a release `libSwell.so` locally:

```bash
make -C WDL/swell -j2
```

For debugging accessibility changes, build with symbols:

```bash
make -C WDL/swell DEBUG=1 -j2
```
