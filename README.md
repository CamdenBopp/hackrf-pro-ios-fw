# HackRF Pro firmware with an iOS USB gadget

This is the HackRF device firmware with one addition: a USB CDC-ECM (ethernet
gadget) personality so a HackRF Pro can be driven from an iPhone or iPad over
USB-C, with no host computer in the loop. It is the `firmware/` subtree of
[greatscottgadgets/hackrf](https://github.com/greatscottgadgets/hackrf),
extracted so the firmware builds on its own, plus the changes described below.

It is not affiliated with or endorsed by Great Scott Gadgets. "HackRF" is a
Great Scott Gadgets trademark; see [TRADEMARK](TRADEMARK).

## License

GPL v2 or later, the same license Great Scott Gadgets ships HackRF under. The
full text is in [COPYING](COPYING), and every source file keeps its original
copyright header. The new CDC-ECM files carry the same GPL header.

## What is different from upstream

The stock firmware exposes one USB configuration: the vendor-specific interface
that `libhackrf` and the desktop tools talk to. This build adds a second
personality next to it.

- A CDC-ECM interface (`firmware/hackrf_usb/usb_api_cdc.c`). A host that speaks
  CDC-ECM, which iOS and macOS do, binds the radio as a network device. The
  vendor interface is left in place, so the desktop tools still work.
- An on-device responder for ARP, ICMP echo, and a minimal DHCP server, enough
  to bring the link up and hand the host an address on a private `10.55.0.0/24`.
  The DHCP reply deliberately omits a gateway so the phone does not route its
  internet traffic through the radio.
- A UDP control and IQ transport on that link (protocol version 3). The app
  sends ASCII commands to UDP port 5000; RX IQ streams back from port 5001 as
  `[4-byte big-endian sequence][1024 bytes signed 8-bit IQ]`. The commands are
  `START`, `STOP`, `FREQ <hz>`, `RATE <hz>`, `BW <hz>`, `AMP <0|1>`,
  `LNA <db>`, `VGA <db>`, `ANT_BIAS <0|1>` (RF-port bias tee, off at idle),
  `CORR <ppb>` (signed reference-clock correction in parts per billion),
  `DECIM <n|AUTO>` (RX decimation: `AUTO` lets the firmware pick, a number sets
  a manual log2 ratio), `PING`, and `STATS`. `FREQ` applies mid-stream, which is
  what makes voice-follow retuning possible. On praline a low `RATE` engages the
  FPGA CIC decimator automatically, so the streamed rate is the requested rate.
  `STATS` replies with the effective rate, decimation, and frame counters.
- If the host stops draining the link mid-stream (unplug, USB suspend, interface
  down), the radio drops IQ frames rather than blocking, and streaming resumes on
  its own when the host returns. Dropped frames are counted in `STATS`.
- A vendor request to toggle the ethernet personality on or off, persisted in
  no-init RAM across a reset. A desktop that wants the plain vendor interface
  can turn it off; unplugging and powering down defaults it back on, so a phone
  always gets the ethernet gadget.

This is experimental. The code here is the whole story for the radio side; the
iOS app and the Go binding that consume the protocol live elsewhere.

## Building

The firmware targets the HackRF Pro (`praline`). You need the ARM GNU toolchain
(`arm-none-eabi-gcc`) and CMake, and the `libopencm3` submodule.

```
git submodule update --init
cd hackrf_usb
mkdir build && cd build
cmake .. -DBOARD=PRALINE
make
```

The image is `hackrf_usb/build/hackrf_usb.bin`. See [README](README) for the
other supported boards, DFU recovery, and toolchain notes. `libopencm3` is
pinned to the same commit upstream uses.

## Flashing

```
hackrf_spiflash -w hackrf_usb/build/hackrf_usb.bin
```

If the ethernet personality is enabled the device presents as a network gadget,
so before writing over USB you may need to toggle it off first (vendor request
64, `wValue` 0), flash, reset, then toggle it back on (`wValue` 1). Keep a known
good image on hand in case a write leaves the radio unresponsive; DFU recovery
is covered in the firmware README.
