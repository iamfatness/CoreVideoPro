// corevideo-browser-host (macOS) — one WKWebView render process per browser
// source, the twin of the Windows WebView2 host.
//
// WHY A SEPARATE PROCESS: page content must never run inside the media core. A
// hung script, a leaking canvas or a crashing plugin takes down this helper and
// the core keeps compositing — the same isolation rule the Zoom engine follows.
//
// It renders --url offscreen, captures frames, and publishes tightly packed
// BGRA into the POSIX shared segment the core maps (BrowserSourceShm.h). stdin
// is the control channel: EOF (the core dying or dropping the source) means
// exit, so a host can never outlive its parent.
//
// CAPTURE APPROACH, and its honest cost. WKWebView renders OUT OF PROCESS, so
// CALayer.render(in:) returns blank for it — the trick that works for ordinary
// NSViews does not work here. `takeSnapshotWithConfiguration:` is the supported
// path that actually returns page pixels. It is slower than the Windows host's
// WGC self-capture (which the core measures at ~28fps); this lands lower and is
// bounded by whatever the page and machine allow. That is an acceptable v1 for
// graphics overlays, which are typically low motion, and the fps actually
// achieved is reported on stderr rather than assumed. ScreenCaptureKit
// self-capture is the documented optimisation, mirroring WGC.

#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "modules/BrowserSourceShm.h"

namespace {

struct Options {
  std::string url;
  std::string shmName;
  int width = 1920;
  int height = 1080;
  int fps = 30;
};

Options parseOptions(int argc, const char** argv) {
  Options options;
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string flag = argv[i];
    const std::string value = argv[i + 1];
    if (flag == "--url") {
      options.url = value;
    } else if (flag == "--shm") {
      options.shmName = value;
    } else if (flag == "--width") {
      options.width = std::atoi(value.c_str());
    } else if (flag == "--height") {
      options.height = std::atoi(value.c_str());
    } else if (flag == "--fps") {
      options.fps = std::atoi(value.c_str());
    }
  }
  return options;
}

// Writer side of the seqlock segment. Created (not opened) here: the host owns
// the buffer, the core maps it read-only.
class FramePublisher {
 public:
  bool open(const std::string& name, int width, int height) {
    if (!corevideo::modules::browsershm::shmNameFitsPlatformLimit(name)) {
      std::fprintf(stderr, "[browser-host] shm name '%s' exceeds the platform limit\n",
                   name.c_str());
      return false;
    }
    bytes_ = corevideo::modules::browsershm::mappingBytes(width, height);
    // Unlink any stale segment first: a previous host that was SIGKILLed leaves
    // one behind, and re-opening it would inherit its size.
    ::shm_unlink(name.c_str());
    fd_ = ::shm_open(name.c_str(), O_CREAT | O_RDWR, 0600);
    if (fd_ < 0) {
      std::fprintf(stderr, "[browser-host] shm_open('%s') failed: %s\n", name.c_str(),
                   std::strerror(errno));
      return false;
    }
    if (::ftruncate(fd_, static_cast<off_t>(bytes_)) != 0) {
      std::fprintf(stderr, "[browser-host] ftruncate failed: %s\n", std::strerror(errno));
      return false;
    }
    void* view = ::mmap(nullptr, bytes_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (view == MAP_FAILED) {
      std::fprintf(stderr, "[browser-host] mmap failed: %s\n", std::strerror(errno));
      return false;
    }
    base_ = static_cast<uint8_t*>(view);
    name_ = name;
    std::memset(base_, 0, bytes_);
    return true;
  }

  void publish(const uint8_t* bgra, int width, int height) {
    if (base_ == nullptr) {
      return;
    }
    corevideo::modules::browsershm::writeFrame(base_, bytes_, bgra, width, height);
  }

  ~FramePublisher() {
    if (base_ != nullptr) {
      ::munmap(base_, bytes_);
    }
    if (fd_ >= 0) {
      ::close(fd_);
    }
    if (!name_.empty()) {
      ::shm_unlink(name_.c_str());
    }
  }

 private:
  int fd_ = -1;
  uint8_t* base_ = nullptr;
  std::size_t bytes_ = 0;
  std::string name_;
};

std::atomic<bool> g_running{true};

// stdin EOF is the parent's death signal. Watched on its own thread so a wedged
// page can never keep this process alive past the core.
void watchParent() {
  char scratch[64];
  while (g_running.load()) {
    const ssize_t read = ::read(STDIN_FILENO, scratch, sizeof(scratch));
    if (read <= 0) {
      g_running.store(false);
      dispatch_async(dispatch_get_main_queue(), ^{
        [NSApp terminate:nil];
      });
      return;
    }
  }
}

}  // namespace

@interface HostController : NSObject <WKNavigationDelegate>
@property(nonatomic, strong) WKWebView* webView;
@property(nonatomic, assign) FramePublisher* publisher;
@property(nonatomic, assign) int frameWidth;
@property(nonatomic, assign) int frameHeight;
@property(nonatomic, assign) int fps;
@property(nonatomic, assign) BOOL snapshotInFlight;
@property(nonatomic, assign) int64_t framesPublished;
@property(nonatomic, strong) NSDate* rateStamp;
@end

@implementation HostController

- (void)startCaptureLoop {
  self.rateStamp = [NSDate date];
  const double interval = 1.0 / MAX(1, self.fps);
  [NSTimer scheduledTimerWithTimeInterval:interval
                                  repeats:YES
                                    block:^(NSTimer* timer) {
    (void)timer;
    [self captureOnce];
  }];
}

- (void)captureOnce {
  // Never queue snapshots: takeSnapshot is async and slower than the timer, so
  // overlapping requests would pile up unboundedly and starve the run loop.
  if (self.snapshotInFlight) {
    return;
  }
  self.snapshotInFlight = YES;

  WKSnapshotConfiguration* config = [[WKSnapshotConfiguration alloc] init];
  config.rect = CGRectMake(0, 0, self.frameWidth, self.frameHeight);
  __weak HostController* weakSelf = self;
  [self.webView takeSnapshotWithConfiguration:config
                           completionHandler:^(NSImage* image, NSError* error) {
    HostController* strongSelf = weakSelf;
    if (strongSelf == nil) {
      return;
    }
    strongSelf.snapshotInFlight = NO;
    if (image == nil) {
      return;  // transient (page still loading); the next tick retries
    }
    [strongSelf publishImage:image];
  }];
}

- (void)publishImage:(NSImage*)image {
  const int width = self.frameWidth;
  const int height = self.frameHeight;
  const size_t stride = static_cast<size_t>(width) * 4;

  // Draw into a tightly packed BGRA buffer: the core's ingest expects exactly
  // width*height*4 with NO row padding, so the context is created over our own
  // buffer rather than letting CoreGraphics choose a stride.
  std::vector<uint8_t> buffer(stride * static_cast<size_t>(height), 0);
  CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGContextRef context = CGBitmapContextCreate(
      buffer.data(), width, height, 8, stride, space,
      kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
  CGColorSpaceRelease(space);
  if (context == nullptr) {
    return;
  }
  CGImageRef cgImage = [image CGImageForProposedRect:nullptr context:nil hints:nil];
  if (cgImage != nullptr) {
    CGContextDrawImage(context, CGRectMake(0, 0, width, height), cgImage);
  }
  CGContextRelease(context);

  self.publisher->publish(buffer.data(), width, height);
  ++self.framesPublished;

  // Report the fps ACTUALLY achieved. This path is snapshot-bound and slower
  // than the Windows WGC host, so the number must be observable rather than
  // assumed by anyone reading the source.
  const NSTimeInterval elapsed = -[self.rateStamp timeIntervalSinceNow];
  if (elapsed >= 5.0) {
    std::fprintf(stderr, "[browser-host] %.1f fps published (%lld frames)\n",
                 self.framesPublished / elapsed, (long long)self.framesPublished);
    self.framesPublished = 0;
    self.rateStamp = [NSDate date];
  }
}

- (void)webView:(WKWebView*)webView didFailNavigation:(WKNavigation*)navigation
      withError:(NSError*)error {
  (void)webView;
  (void)navigation;
  // LOUD: a page that never loads publishes nothing, and a silent black source
  // is the failure this project keeps having to diagnose after the fact.
  std::fprintf(stderr, "[browser-host] navigation FAILED: %s\n",
               error.localizedDescription.UTF8String);
}

- (void)webView:(WKWebView*)webView
    didFailProvisionalNavigation:(WKNavigation*)navigation
                       withError:(NSError*)error {
  (void)webView;
  (void)navigation;
  std::fprintf(stderr, "[browser-host] navigation FAILED (provisional): %s\n",
               error.localizedDescription.UTF8String);
}

@end

int main(int argc, const char** argv) {
  const Options options = parseOptions(argc, argv);
  if (options.url.empty() || options.shmName.empty()) {
    std::fprintf(stderr,
                 "usage: corevideo-browser-host --url <url> --shm <name> "
                 "[--width N --height N --fps N]\n");
    return 2;
  }

  static FramePublisher publisher;
  if (!publisher.open(options.shmName, options.width, options.height)) {
    return 3;
  }

  @autoreleasepool {
    [NSApplication sharedApplication];
    // Accessory: no Dock icon, no menu bar — this is a headless render helper,
    // not something the operator should ever see or be able to focus.
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];

    WKWebViewConfiguration* config = [[WKWebViewConfiguration alloc] init];
    const NSRect frame = NSMakeRect(0, 0, options.width, options.height);
    WKWebView* webView = [[WKWebView alloc] initWithFrame:frame configuration:config];
    // Transparent background so a page with alpha keys over program, matching
    // the Windows host (real alpha survives; graphics tools depend on it).
    [webView setValue:@NO forKey:@"drawsBackground"];

    // The view must live in a window to lay out and paint, but the window sits
    // OFF the visible desktop so it never appears on screen.
    NSWindow* window =
        [[NSWindow alloc] initWithContentRect:NSMakeRect(-32000, -32000, options.width,
                                                         options.height)
                                    styleMask:NSWindowStyleMaskBorderless
                                      backing:NSBackingStoreBuffered
                                        defer:NO];
    window.contentView = webView;
    window.opaque = NO;
    window.backgroundColor = NSColor.clearColor;
    [window orderBack:nil];

    HostController* controller = [[HostController alloc] init];
    controller.webView = webView;
    controller.publisher = &publisher;
    controller.frameWidth = options.width;
    controller.frameHeight = options.height;
    controller.fps = options.fps;
    webView.navigationDelegate = controller;

    NSURL* url = [NSURL URLWithString:@(options.url.c_str())];
    if (url == nil) {
      std::fprintf(stderr, "[browser-host] unusable url\n");
      return 4;
    }
    [webView loadRequest:[NSURLRequest requestWithURL:url]];
    [controller startCaptureLoop];

    std::thread(watchParent).detach();
    [NSApp run];
  }
  return 0;
}
