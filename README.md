# GABSERVE

A simple prototype for sharing a screen and system audio over UDP on Windows.

- the desktop image is scaled, encoded as H.264, and split into UDP datagrams,
- the mouse cursor is composed into every captured frame,
- audio played by the computer is captured using WASAPI loopback,
- the receiver uses a back buffer to display frames without visible black clears,
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
--video-backend cpu|gpu  capture with GDI or DXGI Desktop Duplication
--video-codec h264|jpeg  Media Foundation H.264 (default) or WIC JPEG
--bitrate 4000           H.264 bitrate in kbps
--fps 10                 frame rate (1-60)
--width 1280             maximum width of the transmitted image
--quality 0.65           JPEG quality (0.1-1.0)
--no-audio               transmit video only
```

The default `cpu` backend captures through GDI. The `gpu` backend captures through
DXGI Desktop Duplication and D3D11:

```powershell
.\build\gabserve.exe send --video-backend gpu
```

H.264 uses a low-latency Media Foundation encoder with NV12 input and Baseline
profile. The sender periodically transmits SPS/PPS configuration and requests an
IDR frame so that a receiver can join an active stream. The receiver decodes H.264
through Media Foundation and converts NV12 to its BGRA display buffer.

With `--video-backend gpu`, the sender first tries a synchronous hardware H.264 MFT
and falls back to the built-in software MFT when the driver only exposes an
asynchronous encoder. Capture and scaling still pass through a CPU-visible buffer,
so this is not yet a completely zero-copy pipeline. JPEG remains available as a
compatibility fallback:

```powershell
.\build\gabserve.exe send --video-codec jpeg --quality 0.65
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
Ethernet network. H.264 access units, video configuration, JPEG fallback, PCM, and
audio-format streams are reassembled independently.
