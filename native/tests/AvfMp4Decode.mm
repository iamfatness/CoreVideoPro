#include "AvfMp4Decode.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>

#include <cstring>

namespace corevideo::testing {

namespace {
// Synchronous wrapper over the async track loader (the sync accessor
// -tracksWithMediaType: is deprecated since macOS 15).
NSArray<AVAssetTrack*>* loadTracks(AVURLAsset* asset, AVMediaType type) {
  __block NSArray<AVAssetTrack*>* result = @[];
  dispatch_semaphore_t done = dispatch_semaphore_create(0);
  [asset loadTracksWithMediaType:type
               completionHandler:^(NSArray<AVAssetTrack*>* tracks, NSError*) {
                 if (tracks) result = tracks;
                 dispatch_semaphore_signal(done);
               }];
  dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC));
  return result;
}
}  // namespace

double mp4DurationSeconds(const std::string& path) {
  double longest = 0;
  @autoreleasepool {
    AVURLAsset* asset =
        [AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]]
                            options:nil];
    for (AVMediaType type in @[ AVMediaTypeVideo, AVMediaTypeAudio ]) {
      for (AVAssetTrack* track in loadTracks(asset, type)) {
        const double secs = CMTimeGetSeconds(CMTimeRangeGetEnd(track.timeRange));
        if (secs > longest) longest = secs;
      }
    }
  }
  return longest;
}

DecodedMp4Video decodeMp4VideoToBgra(const std::string& path) {
  DecodedMp4Video out;
  @autoreleasepool {
    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
    AVURLAsset* asset = [AVURLAsset URLAssetWithURL:url options:nil];
    NSArray<AVAssetTrack*>* tracks = loadTracks(asset, AVMediaTypeVideo);
    if (tracks.count == 0) {
      out.error = "no video track";
      return out;
    }
    AVAssetTrack* track = tracks[0];
    out.width = static_cast<int>(track.naturalSize.width);
    out.height = static_cast<int>(track.naturalSize.height);
    NSError* error = nil;
    AVAssetReader* reader = [AVAssetReader assetReaderWithAsset:asset error:&error];
    if (!reader) {
      out.error = "AVAssetReader init failed";
      return out;
    }
    AVAssetReaderTrackOutput* trackOutput = [AVAssetReaderTrackOutput
        assetReaderTrackOutputWithTrack:track
                         outputSettings:@{
                           (__bridge NSString*)kCVPixelBufferPixelFormatTypeKey :
                               @(kCVPixelFormatType_32BGRA)
                         }];
    [reader addOutput:trackOutput];
    if (![reader startReading]) {
      out.error = "startReading failed";
      return out;
    }
    while (CMSampleBufferRef sample = [trackOutput copyNextSampleBuffer]) {
      CVImageBufferRef image = CMSampleBufferGetImageBuffer(sample);
      if (image) {
        CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
        const size_t w = CVPixelBufferGetWidth(image);
        const size_t h = CVPixelBufferGetHeight(image);
        const size_t stride = CVPixelBufferGetBytesPerRow(image);
        const auto* base = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(image));
        std::vector<uint8_t> frame(w * h * 4u);
        for (size_t y = 0; y < h; ++y) std::memcpy(&frame[y * w * 4u], base + y * stride, w * 4u);
        CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
        out.frames.push_back(std::move(frame));
      }
      CFRelease(sample);
    }
  }
  return out;
}

}  // namespace corevideo::testing
