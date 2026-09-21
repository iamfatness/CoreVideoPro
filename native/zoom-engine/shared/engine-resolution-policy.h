#pragma once

#include <cstddef>
#include <cstdint>

// DOES A RESOLUTION REQUEST FOR A LIVE CAMERA RENDERER REBUILD IT? (#478 R4)
//
// A Zoom raw-data renderer's resolution is set BEFORE subscribe() and is never
// changed in place (setRawDataResolution on a live renderer is undocumented; see
// ZoomSubscriptionResolutionPolicy.h in the core). So a change of resolution is a
// destroy-then-create of the renderer.
//
// Raising it always rebuilt. LOWERING it used to be a no-op
// (`video_subscribe_noop_existing`): the renderer kept its higher resolution
// forever, so over a show every guest ever cued to a bus stayed at 1080P — the
// N x 1080P load that crashed the SDK subprocess (0xc000000d, commit bd3caf29).
// The core asks for 720P only when its 8-camera 1080P cap demotes a guest (since
// 2026-09-20 every in-show camera purpose is 1080P, so leaving a bus alone no
// longer lowers a request), and the engine honours the downgrade — but only when this source is the renderer's ONLY target: another
// target of the same participant renderer still wants the higher resolution.
//
// Pure, so it is testable without the SDK (native/tests/ZoomEngineRuntimeTest.cpp).
inline bool video_resolution_needs_rebuild(uint32_t requested, uint32_t active,
                                           size_t other_targets)
{
    if (requested > active) return true;
    if (requested < active) return other_targets == 0;
    return false;
}
