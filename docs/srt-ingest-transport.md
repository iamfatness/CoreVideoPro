# SRT ingest and transport

CoreVideo Pro treats SRT as two related but separate media paths:

- **Program transport:** send the encoded program feed to an SRT listener/caller endpoint.
- **Contribution ingest:** receive an SRT contribution feed and expose it as a routable show input beside Zoom, Blackmagic, AJA, and UVC sources.

Haivision SRT is the intended native runtime. It is a C/C++ transport library for low-latency live media over unreliable networks, with configurable latency, ARQ recovery, encryption, and caller/listener/rendezvous connection modes. The dependency is dev-gated with `COREVIDEO_WITH_SRT_OUTPUT`; default stub builds remain green without libsrt installed.

## Current alpha contract

- WinUI streaming now requests `srt` as a program destination alongside `rtmp` and `ndi`.
- Native stub output diagnostics already isolate multiple destinations, including `srt`, so preflight can prove that SRT does not collapse into RTMP state.
- `native/CMakeLists.txt` exposes `COREVIDEO_WITH_SRT_OUTPUT` and probes for `SRT::srt` when a dev-machine build opts in.

## Next dev-machine work

1. Install/build Haivision SRT through vcpkg, Conan, or a local CMake install.
2. Configure native with `-DCOREVIDEO_ENABLE_DEV_ADAPTERS=ON -DCOREVIDEO_WITH_SRT_OUTPUT=ON`.
3. Replace `SrtOutputSenderAdapter.cpp` scaffold with a real sender implementing caller/listener modes, latency, passphrase/key length, and socket stats.
4. Add `SrtIngestSource` as a capture-source adapter and expose it in the Inputs screen as a routable network source.
5. Extend support bundles with SRT endpoint mode, connection state, RTT, packet loss, retransmit count, bitrate, and last socket error.

## Guardrails

- Keep SRT off by default until runtime packaging is stable.
- Do not mix SRT sender failure state with RTMP/NDI sender state.
- Treat SRT ingest as a source, not as a scene/filter feature.
