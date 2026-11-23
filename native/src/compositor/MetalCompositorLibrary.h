#pragma once
// The Metal compositor's shader-library compile, extracted so a test can force
// the Tiles effect-compile fallback (MetalTilesPixels.ABrokenEffectStillCompilesACleanLibrary,
// the twin of TilesDecoration.EffectCompilationFailureRetainsCleanShader).
// Compiles to nothing when Metal is off.

#if COREVIDEO_WITH_METAL

#include <string>

#ifdef __OBJC__
#import <Metal/Metal.h>
// Compiles `source`; on failure retries with COREVIDEO_DISABLE_TILES_EFFECT and
// sets effectsAvailable=false. Returns nil only when the clean retry fails too
// (then `error` carries the diagnostics).
id<MTLLibrary> compileMetalCompositorLibrary(id<MTLDevice> device, const char* source,
                                             bool& effectsAvailable, std::string& error);
#endif

// Test hook for .cpp tests: compiles `source` on the system default device.
// Returns true when a library was produced. With no Metal device it returns
// false and leaves effectsAvailable untouched.
bool metalCompositorLibraryFallbackForTest(const char* source, bool& effectsAvailable);

#endif  // COREVIDEO_WITH_METAL
