# GABSERVE

A simple prototype for sharing a screen and system audio over UDP on Windows.

- the desktop image is scaled, encoded as JPEG, and split into UDP datagrams,
- audio played by the computer is captured using WASAPI loopback,
- the receiver reassembles frames, displays the image in a window, and plays audio,
- the default endpoint is `127.0.0.1:7777`.

## Requirements

- Windows 10 or 11,
- Visual Studio Build Tools with MSVC, or CMake with an MSVC/Clang compiler,
- Windows SDK.

## Building

In PowerShell:

```powershell
.\build.ps1
```

Alternatively, use CMake:

```powershell
cmake -S . -B build
cmake --build build --config Release
```

## Running on localhost

Start the receiver first:

```powershell
.\build\gabserve.exe receive
```

Start the sender in a second terminal:

```powershell
.\build\gabserve.exe send
```

Useful sender options:

```text
--fps 10          frame rate (1-60)
--width 1280      maximum width of the transmitted image
--quality 0.65    JPEG quality (0.1-1.0)
--no-audio        transmit video only
```

The address and port can be changed with `--host`/`--bind` and `--port`. At this
stage, the protocol assumes a trusted network: it does not provide encryption,
authentication, retransmission, or packet-loss recovery.

When the audio sender and receiver run on the same computer, WASAPI may capture
the audio played by the receiver again. Use `--no-audio` when testing video only;
full audio playback is best tested on two devices.

For a windowless test, run
`gabserve receive --headless --no-audio --seconds 5`. The sender can then be run
with `--test-pattern --seconds 3` to verify the complete pipeline without access
to an interactive desktop.

## MVP protocol

Each datagram has a 36-byte `GABS` header, an object number, and a fragment index.
The maximum data fragment is 1200 bytes to avoid IP fragmentation on a typical
Ethernet network. Video, PCM, and audio-format streams are reassembled independently.
