#import "X1BoxNativeBridge.h"

#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>

#include <TargetConditionals.h>
#include <dlfcn.h>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

struct Error;
struct strList;

namespace {

NSString *const X1BoxNativeBridgeErrorDomain = @"X1Box.NativeBridge";

std::mutex gEmbeddedCoreLoaderMutex;
void *gEmbeddedCoreDynamicHandle = nullptr;
bool gEmbeddedCoreLoadAttempted = false;
std::string gEmbeddedCoreDynamicPath;
std::string gEmbeddedCoreDynamicLoadError;

using EmbeddedBootFn = bool (*)(const char *, const char **);
using EmbeddedPumpFrameFn = void (*)(void);
using EmbeddedRequestShutdownFn = void (*)(void);
using EmbeddedIsActiveFn = bool (*)(void);
using EmbeddedGetLastErrorFn = const char *(*)(void);
using QemuInitFn = void (*)(int, char **);
using QemuMainFn = int (*)(void);
using XemuSettingsSetPathFn = void (*)(const char *);
using XemuSettingsLoadFn = bool (*)(void);
using XemuSettingsGetErrorMessageFn = const char *(*)(void);
using QemuSystemPowerdownRequestFn = void (*)(void);
using SaveSnapshotFn = bool (*)(const char *, bool, const char *, bool, strList *, Error **);
using LoadSnapshotFn = bool (*)(const char *, const char *, bool, strList *, Error **);
using DeleteSnapshotFn = bool (*)(const char *, bool, strList *, Error **);
using ErrorGetPrettyFn = const char *(*)(const Error *);
using ErrorFreeFn = void (*)(Error *);

template <typename T>
static T ResolveOptionalSymbol(const char *name)
{
  std::lock_guard<std::mutex> lock(gEmbeddedCoreLoaderMutex);

  if (!gEmbeddedCoreLoadAttempted && gEmbeddedCoreDynamicHandle == nullptr) {
    gEmbeddedCoreLoadAttempted = true;

    NSMutableArray<NSString *> *candidates = [NSMutableArray array];
    NSBundle *mainBundle = [NSBundle mainBundle];
    NSBundle *bridgeBundle = [NSBundle bundleForClass:[X1BoxNativeBridge class]];
    NSArray<NSString *> *frameworkRoots = @[
      mainBundle.privateFrameworksPath ?: @"",
      bridgeBundle.privateFrameworksPath ?: @"",
      [bridgeBundle.bundlePath stringByDeletingLastPathComponent]
    ];

#if TARGET_OS_SIMULATOR
    NSArray<NSString *> *relativeCandidates = @[
      @"X1BoxEmbeddedCore.xcframework/ios-arm64_x86_64-simulator/X1BoxEmbeddedCore.framework/X1BoxEmbeddedCore",
      @"X1BoxEmbeddedCore.xcframework/ios-arm64-simulator/X1BoxEmbeddedCore.framework/X1BoxEmbeddedCore",
      @"X1BoxEmbeddedCore.xcframework/ios-x86_64-simulator/X1BoxEmbeddedCore.framework/X1BoxEmbeddedCore",
      @"X1BoxEmbeddedCore.xcframework/ios-arm64_x86_64-simulator/libxemu-ios-core.dylib",
      @"X1BoxEmbeddedCore.xcframework/ios-arm64-simulator/libxemu-ios-core.dylib",
      @"X1BoxEmbeddedCore.xcframework/ios-x86_64-simulator/libxemu-ios-core.dylib",
      @"X1BoxEmbeddedCore.framework/X1BoxEmbeddedCore",
      @"libxemu-ios-core.dylib"
    ];
#else
    NSArray<NSString *> *relativeCandidates = @[
      @"X1BoxEmbeddedCore.xcframework/ios-arm64/X1BoxEmbeddedCore.framework/X1BoxEmbeddedCore",
      @"X1BoxEmbeddedCore.xcframework/ios-arm64/libxemu-ios-core.dylib",
      @"X1BoxEmbeddedCore.framework/X1BoxEmbeddedCore",
      @"libxemu-ios-core.dylib"
    ];
#endif

    for (NSString *root in frameworkRoots) {
      if (root.length == 0) {
        continue;
      }
      for (NSString *relativePath in relativeCandidates) {
        [candidates addObject:[root stringByAppendingPathComponent:relativePath]];
      }
    }

    NSArray<NSString *> *appSupportRoots = NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES);
    for (NSString *appSupportRoot in appSupportRoots) {
      NSString *coreRoot = [appSupportRoot stringByAppendingPathComponent:@"X1Box/EmbeddedCore"];
      for (NSString *relativePath in relativeCandidates) {
        [candidates addObject:[coreRoot stringByAppendingPathComponent:relativePath]];
      }
    }

    NSFileManager *fileManager = [NSFileManager defaultManager];
    for (NSString *candidate in candidates) {
      if (![fileManager fileExistsAtPath:candidate]) {
        continue;
      }

      void *handle = dlopen(candidate.fileSystemRepresentation, RTLD_NOW | RTLD_GLOBAL);
      if (handle != nullptr) {
        gEmbeddedCoreDynamicHandle = handle;
        gEmbeddedCoreDynamicPath = std::string(candidate.UTF8String ? candidate.UTF8String : "");
        gEmbeddedCoreDynamicLoadError.clear();
        break;
      }

      const char *dlError = dlerror();
      gEmbeddedCoreDynamicLoadError = dlError != nullptr
        ? std::string(dlError)
        : std::string("dlopen failed while loading the embedded core image.");
    }

    if (gEmbeddedCoreDynamicHandle == nullptr && gEmbeddedCoreDynamicLoadError.empty()) {
      gEmbeddedCoreDynamicLoadError =
        "No bundled or staged X1BoxEmbeddedCore image was found. Embed a signed "
        "X1BoxEmbeddedCore.framework into the app bundle to enable real iOS core startup.";
    }
  }

  if (gEmbeddedCoreDynamicHandle != nullptr) {
    T dynamicSymbol = reinterpret_cast<T>(dlsym(gEmbeddedCoreDynamicHandle, name));
    if (dynamicSymbol != nullptr) {
      return dynamicSymbol;
    }
  }

  return reinterpret_cast<T>(dlsym(RTLD_DEFAULT, name));
}

static EmbeddedBootFn EmbeddedBootSymbol(void)
{
  return ResolveOptionalSymbol<EmbeddedBootFn>("xemu_embedded_boot");
}

static EmbeddedPumpFrameFn EmbeddedPumpFrameSymbol(void)
{
  return ResolveOptionalSymbol<EmbeddedPumpFrameFn>("xemu_embedded_pump_frame");
}

static EmbeddedRequestShutdownFn EmbeddedRequestShutdownSymbol(void)
{
  return ResolveOptionalSymbol<EmbeddedRequestShutdownFn>("xemu_embedded_request_shutdown");
}

static EmbeddedIsActiveFn EmbeddedIsActiveSymbol(void)
{
  return ResolveOptionalSymbol<EmbeddedIsActiveFn>("xemu_embedded_is_active");
}

static EmbeddedGetLastErrorFn EmbeddedGetLastErrorSymbol(void)
{
  return ResolveOptionalSymbol<EmbeddedGetLastErrorFn>("xemu_embedded_get_last_error");
}

static QemuInitFn QemuInitSymbol(void)
{
  return ResolveOptionalSymbol<QemuInitFn>("qemu_init");
}

static QemuMainFn QemuMainSymbol(void)
{
  return ResolveOptionalSymbol<QemuMainFn>("qemu_main");
}

static XemuSettingsSetPathFn XemuSettingsSetPathSymbol(void)
{
  return ResolveOptionalSymbol<XemuSettingsSetPathFn>("xemu_settings_set_path");
}

static XemuSettingsLoadFn XemuSettingsLoadSymbol(void)
{
  return ResolveOptionalSymbol<XemuSettingsLoadFn>("xemu_settings_load");
}

static XemuSettingsGetErrorMessageFn XemuSettingsGetErrorMessageSymbol(void)
{
  return ResolveOptionalSymbol<XemuSettingsGetErrorMessageFn>("xemu_settings_get_error_message");
}

static QemuSystemPowerdownRequestFn QemuSystemPowerdownRequestSymbol(void)
{
  return ResolveOptionalSymbol<QemuSystemPowerdownRequestFn>("qemu_system_powerdown_request");
}

static SaveSnapshotFn SaveSnapshotSymbol(void)
{
  return ResolveOptionalSymbol<SaveSnapshotFn>("save_snapshot");
}

static LoadSnapshotFn LoadSnapshotSymbol(void)
{
  return ResolveOptionalSymbol<LoadSnapshotFn>("load_snapshot");
}

static DeleteSnapshotFn DeleteSnapshotSymbol(void)
{
  return ResolveOptionalSymbol<DeleteSnapshotFn>("delete_snapshot");
}

static ErrorGetPrettyFn ErrorGetPrettySymbol(void)
{
  return ResolveOptionalSymbol<ErrorGetPrettyFn>("error_get_pretty");
}

static ErrorFreeFn ErrorFreeSymbol(void)
{
  return ResolveOptionalSymbol<ErrorFreeFn>("error_free");
}

struct VirtualAxisState {
  float x = 0.0f;
  float y = 0.0f;
};

struct SessionRuntimeState {
  bool running = false;
  bool embeddedCoreLinked = false;
  bool embeddedHostAPILinked = false;
  bool bootThreadActive = false;
  bool coreStartedOnce = false;
  std::string configPath;
  std::string statusLine;
  std::map<std::string, bool> pressedButtons;
  std::map<std::string, VirtualAxisState> axes;
};

static bool EmbeddedHostAPIIsLinked(void)
{
  return EmbeddedBootSymbol() != nullptr &&
         EmbeddedPumpFrameSymbol() != nullptr &&
         EmbeddedRequestShutdownSymbol() != nullptr;
}

static bool EmbeddedCoreIsLinked(void)
{
  return EmbeddedHostAPIIsLinked() || (QemuInitSymbol() != nullptr && QemuMainSymbol() != nullptr);
}

static bool NativeSnapshotAPIIsLinked(void)
{
  return SaveSnapshotSymbol() != nullptr &&
         LoadSnapshotSymbol() != nullptr &&
         DeleteSnapshotSymbol() != nullptr &&
         ErrorGetPrettySymbol() != nullptr &&
         ErrorFreeSymbol() != nullptr;
}

static void ResetEmbeddedCoreLoadState(void)
{
  std::lock_guard<std::mutex> lock(gEmbeddedCoreLoaderMutex);
  if (gEmbeddedCoreDynamicHandle == nullptr) {
    gEmbeddedCoreDynamicPath.clear();
    gEmbeddedCoreDynamicLoadError.clear();
    gEmbeddedCoreLoadAttempted = false;
  }
}

static NSString *EmbeddedCoreDynamicPathString(void)
{
  std::lock_guard<std::mutex> lock(gEmbeddedCoreLoaderMutex);
  return gEmbeddedCoreDynamicPath.empty()
    ? nil
    : [NSString stringWithUTF8String:gEmbeddedCoreDynamicPath.c_str()];
}

static NSString *EmbeddedCoreLoaderStatusString(void)
{
  ResolveOptionalSymbol<void *>("xemu_embedded_boot");

  std::lock_guard<std::mutex> lock(gEmbeddedCoreLoaderMutex);
  std::ostringstream stream;

  if (!gEmbeddedCoreDynamicPath.empty()) {
    stream << "Dynamic embedded core image loaded.\n";
    stream << "Path: " << gEmbeddedCoreDynamicPath << "\n";
    stream << "A bundled signed framework is the preferred path for real iPhone/iPad startup.";
  } else if (!gEmbeddedCoreDynamicLoadError.empty()) {
    stream << gEmbeddedCoreDynamicLoadError;
  } else {
    stream << "Embedded core loader has not attempted to resolve a dynamic image yet.";
  }

  return [NSString stringWithUTF8String:stream.str().c_str()];
}

static NSString *NSStringFromStd(const std::string &value)
{
  if (value.empty()) {
    return @"";
  }
  return [NSString stringWithUTF8String:value.c_str()];
}

static std::string StdStringFromNSString(NSString *value)
{
  if (value == nil) {
    return std::string();
  }
  return std::string(value.UTF8String ? value.UTF8String : "");
}

static NSError *SnapshotNSError(NSInteger code, const char *fallbackMessage, Error *snapshotError)
{
  ErrorGetPrettyFn errorGetPretty = ErrorGetPrettySymbol();
  ErrorFreeFn errorFree = ErrorFreeSymbol();
  NSString *message = fallbackMessage ? [NSString stringWithUTF8String:fallbackMessage] : @"Native snapshot operation failed.";
  if (snapshotError != nullptr && errorGetPretty != nullptr) {
    const char *pretty = errorGetPretty(snapshotError);
    if (pretty != nullptr && pretty[0] != '\0') {
      message = [NSString stringWithUTF8String:pretty];
    }
  }
  if (snapshotError != nullptr && errorFree != nullptr) {
    errorFree(snapshotError);
  }
  return [NSError errorWithDomain:X1BoxNativeBridgeErrorDomain
                             code:code
                         userInfo:@{NSLocalizedDescriptionKey: message}];
}

static NSString *SummaryFromState(const SessionRuntimeState &state)
{
  NSString *dynamicCorePath = EmbeddedCoreDynamicPathString();
  NSString *loaderStatus = EmbeddedCoreLoaderStatusString();
  std::ostringstream stream;

  if (!state.running) {
    stream << "Ready.\n";
    if (state.embeddedHostAPILinked) {
      stream << "Embedded iOS host API detected from ui/xemu.c.\n";
      if (state.coreStartedOnce) {
        stream << "This process already initialized the embedded host once. Restart the app to launch again.\n";
      } else {
        stream << "A launch will use the embedded xemu boot path and frame pump.\n";
      }
    } else if (state.embeddedCoreLinked) {
      stream << "Raw xemu symbols detected, but the iOS host API is not linked yet.\n";
      stream << "The bridge can fall back to qemu_init/qemu_main, but the preferred route is the embedded API.\n";
    } else {
      stream << "Embedded xemu symbols are not linked yet.\n";
      stream << "The iOS shell stays usable while the framework waits for the real core link step.\n";
    }
    if (dynamicCorePath.length > 0) {
      stream << "Dynamic core image: " << (dynamicCorePath.UTF8String ? dynamicCorePath.UTF8String : "") << "\n";
    } else if (loaderStatus.length > 0) {
      stream << loaderStatus.UTF8String << "\n";
    }
    return [NSString stringWithUTF8String:stream.str().c_str()];
  }

  stream << "Mode: ";
  if (state.embeddedHostAPILinked) {
    stream << "embedded iOS host";
  } else if (state.embeddedCoreLinked) {
    stream << "raw embedded core";
  } else {
    stream << "shell fallback";
  }
  stream << "\n";

  if (!state.configPath.empty()) {
    stream << "Config: " << state.configPath << "\n";
  }

  if (!state.statusLine.empty()) {
    stream << state.statusLine << "\n";
  }

  if (dynamicCorePath.length > 0) {
    stream << "Dynamic core image: " << (dynamicCorePath.UTF8String ? dynamicCorePath.UTF8String : "") << "\n";
  }

  stream << "Buttons tracked: " << state.pressedButtons.size();
  if (!state.axes.empty()) {
    stream << " | Axes tracked: " << state.axes.size();
  }

  if (state.embeddedHostAPILinked) {
    stream << "\nRendering is pumped through the embedded xemu iOS host API.";
  } else if (state.embeddedCoreLinked) {
    stream << "\nOfficial xemu entry points resolved from the linked core.";
  } else {
    stream << "\nLink the embedded iOS host symbols into X1BoxNativeCore to replace the fallback host.";
  }

  return [NSString stringWithUTF8String:stream.str().c_str()];
}

}  // namespace

@interface X1BoxNativeEmulatorViewController : UIViewController

- (void)applyRunning:(BOOL)running detail:(NSString *)detail;
- (void)setFramePumpEnabled:(BOOL)enabled;

@end

@implementation X1BoxNativeEmulatorViewController {
  CAGradientLayer *_backgroundLayer;
  UIView *_panelView;
  UILabel *_titleLabel;
  UILabel *_detailLabel;
  CADisplayLink *_displayLink;
  BOOL _framePumpEnabled;
}

- (void)viewDidLoad {
  [super viewDidLoad];

  self.view.backgroundColor = [UIColor blackColor];

  _backgroundLayer = [CAGradientLayer layer];
  _backgroundLayer.colors = @[
    (id)[UIColor colorWithRed:0.02 green:0.05 blue:0.02 alpha:1.0].CGColor,
    (id)[UIColor colorWithRed:0.07 green:0.15 blue:0.08 alpha:1.0].CGColor,
    (id)[UIColor colorWithRed:0.01 green:0.03 blue:0.02 alpha:1.0].CGColor
  ];
  _backgroundLayer.startPoint = CGPointMake(0.0, 0.0);
  _backgroundLayer.endPoint = CGPointMake(1.0, 1.0);
  [self.view.layer addSublayer:_backgroundLayer];

  _panelView = [[UIView alloc] initWithFrame:CGRectZero];
  _panelView.translatesAutoresizingMaskIntoConstraints = NO;
  _panelView.backgroundColor = [UIColor colorWithRed:0.05 green:0.11 blue:0.06 alpha:0.94];
  _panelView.layer.cornerRadius = 28.0;
  _panelView.layer.borderWidth = 1.0;
  _panelView.layer.borderColor = [UIColor colorWithRed:0.54 green:0.92 blue:0.39 alpha:0.55].CGColor;
  _panelView.layer.shadowColor = [UIColor colorWithRed:0.72 green:1.0 blue:0.48 alpha:1.0].CGColor;
  _panelView.layer.shadowOpacity = 0.2;
  _panelView.layer.shadowRadius = 18.0;
  _panelView.layer.shadowOffset = CGSizeMake(0.0, 10.0);
  [self.view addSubview:_panelView];

  _titleLabel = [[UILabel alloc] initWithFrame:CGRectZero];
  _titleLabel.translatesAutoresizingMaskIntoConstraints = NO;
  _titleLabel.font = [UIFont systemFontOfSize:28.0 weight:UIFontWeightHeavy];
  _titleLabel.textColor = [UIColor colorWithRed:0.72 green:1.0 blue:0.48 alpha:1.0];
  _titleLabel.numberOfLines = 0;
  _titleLabel.text = @"X1 BOX iOS";

  _detailLabel = [[UILabel alloc] initWithFrame:CGRectZero];
  _detailLabel.translatesAutoresizingMaskIntoConstraints = NO;
  _detailLabel.font = [UIFont monospacedSystemFontOfSize:14.0 weight:UIFontWeightMedium];
  _detailLabel.textColor = [UIColor colorWithRed:0.84 green:0.93 blue:0.80 alpha:1.0];
  _detailLabel.numberOfLines = 0;
  _detailLabel.text = @"Preparing native bridge...";

  [_panelView addSubview:_titleLabel];
  [_panelView addSubview:_detailLabel];

  [NSLayoutConstraint activateConstraints:@[
    [_panelView.leadingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.leadingAnchor constant:24.0],
    [_panelView.trailingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.trailingAnchor constant:-24.0],
    [_panelView.centerYAnchor constraintEqualToAnchor:self.view.centerYAnchor],

    [_titleLabel.topAnchor constraintEqualToAnchor:_panelView.topAnchor constant:24.0],
    [_titleLabel.leadingAnchor constraintEqualToAnchor:_panelView.leadingAnchor constant:24.0],
    [_titleLabel.trailingAnchor constraintEqualToAnchor:_panelView.trailingAnchor constant:-24.0],

    [_detailLabel.topAnchor constraintEqualToAnchor:_titleLabel.bottomAnchor constant:16.0],
    [_detailLabel.leadingAnchor constraintEqualToAnchor:_panelView.leadingAnchor constant:24.0],
    [_detailLabel.trailingAnchor constraintEqualToAnchor:_panelView.trailingAnchor constant:-24.0],
    [_detailLabel.bottomAnchor constraintEqualToAnchor:_panelView.bottomAnchor constant:-24.0]
  ]];
}

- (void)dealloc {
  [_displayLink invalidate];
  [_displayLink release];
  [_detailLabel release];
  [_titleLabel release];
  [_panelView release];
  [super dealloc];
}

- (void)viewDidLayoutSubviews {
  [super viewDidLayoutSubviews];
  _backgroundLayer.frame = self.view.bounds;
}

- (void)applyRunning:(BOOL)running detail:(NSString *)detail {
  _titleLabel.text = running ? @"Session Shell Active" : @"X1 BOX iOS";
  _titleLabel.textColor = running
    ? [UIColor colorWithRed:0.72 green:1.0 blue:0.48 alpha:1.0]
    : [UIColor colorWithRed:0.62 green:0.88 blue:0.42 alpha:1.0];
  _detailLabel.text = detail;
}

- (void)setFramePumpEnabled:(BOOL)enabled {
  _framePumpEnabled = enabled;

  if (!enabled) {
    _displayLink.paused = YES;
    return;
  }

  if (_displayLink == nil) {
    _displayLink = [CADisplayLink displayLinkWithTarget:self selector:@selector(displayLinkDidTick:)];
    _displayLink.preferredFramesPerSecond = 60;
    [_displayLink addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
  }
  _displayLink.paused = NO;
}

- (void)displayLinkDidTick:(CADisplayLink *)sender {
  (void)sender;
  if (!_framePumpEnabled) {
    return;
  }

  EmbeddedPumpFrameFn pumpFrame = EmbeddedPumpFrameSymbol();
  EmbeddedIsActiveFn isActive = EmbeddedIsActiveSymbol();
  if (pumpFrame != nullptr) {
    pumpFrame();
    if (isActive != nullptr && !isActive()) {
      [self setFramePumpEnabled:NO];
    }
  }
}

@end

@interface X1BoxNativeBridge ()

@property(nonatomic, strong) X1BoxNativeEmulatorViewController *controller;

- (void)launchRawEmbeddedCoreWithConfigPath:(NSString *)configPath;
- (void)syncController;

@end

@implementation X1BoxNativeBridge {
  SessionRuntimeState _state;
}

+ (instancetype)shared {
  static X1BoxNativeBridge *sharedBridge = nil;
  static dispatch_once_t onceToken;
  dispatch_once(&onceToken, ^{
    sharedBridge = [[self alloc] initPrivate];
  });
  return sharedBridge;
}

- (instancetype)init {
  return [X1BoxNativeBridge shared];
}

- (instancetype)initPrivate {
  self = [super init];
  if (self) {
    _controller = [[X1BoxNativeEmulatorViewController alloc] init];
    _state.embeddedCoreLinked = EmbeddedCoreIsLinked();
    _state.embeddedHostAPILinked = EmbeddedHostAPIIsLinked();
  }
  return self;
}

- (UIViewController *)makeEmulatorViewController {
  _state.embeddedCoreLinked = EmbeddedCoreIsLinked();
  _state.embeddedHostAPILinked = EmbeddedHostAPIIsLinked();
  [self syncController];
  return self.controller;
}

- (void)refreshEmbeddedCoreAvailability {
  ResetEmbeddedCoreLoadState();
  _state.embeddedCoreLinked = EmbeddedCoreIsLinked();
  _state.embeddedHostAPILinked = EmbeddedHostAPIIsLinked();
  [self syncController];
}

- (NSString *)embeddedCoreStatusSummary {
  return EmbeddedCoreLoaderStatusString();
}

- (NSString * _Nullable)resolvedEmbeddedCorePath {
  return EmbeddedCoreDynamicPathString();
}

- (BOOL)startSessionWithConfigPath:(NSString *)configPath error:(NSError * _Nullable __autoreleasing *)error {
  if (configPath.length == 0) {
    if (error != nil) {
      *error = [NSError errorWithDomain:X1BoxNativeBridgeErrorDomain
                                   code:10
                               userInfo:@{NSLocalizedDescriptionKey: @"The config path is empty."}];
    }
    return NO;
  }

  if (![[NSFileManager defaultManager] fileExistsAtPath:configPath]) {
    if (error != nil) {
      *error = [NSError errorWithDomain:X1BoxNativeBridgeErrorDomain
                                   code:11
                               userInfo:@{NSLocalizedDescriptionKey: @"The generated xemu config file could not be found."}];
    }
    return NO;
  }

  if (_state.running) {
    if (error != nil) {
      *error = [NSError errorWithDomain:X1BoxNativeBridgeErrorDomain
                                   code:12
                               userInfo:@{NSLocalizedDescriptionKey: @"An iOS xemu session is already active in this process."}];
    }
    return NO;
  }

  NSError *readError = nil;
  NSString *config = [NSString stringWithContentsOfFile:configPath
                                               encoding:NSUTF8StringEncoding
                                                  error:&readError];
  if (config == nil) {
    if (error != nil) {
      *error = readError ?: [NSError errorWithDomain:X1BoxNativeBridgeErrorDomain
                                                code:13
                                            userInfo:@{NSLocalizedDescriptionKey: @"The config file could not be read."}];
    }
    return NO;
  }

  if ([config rangeOfString:@"[sys.files]"].location == NSNotFound) {
    if (error != nil) {
      *error = [NSError errorWithDomain:X1BoxNativeBridgeErrorDomain
                                   code:14
                               userInfo:@{NSLocalizedDescriptionKey: @"The config file is missing required [sys.files] entries."}];
    }
    return NO;
  }

  _state.embeddedCoreLinked = EmbeddedCoreIsLinked();
  _state.embeddedHostAPILinked = EmbeddedHostAPIIsLinked();
  if (_state.embeddedCoreLinked && _state.coreStartedOnce) {
    if (error != nil) {
      *error = [NSError errorWithDomain:X1BoxNativeBridgeErrorDomain
                                   code:15
                               userInfo:@{NSLocalizedDescriptionKey: @"The embedded xemu core was already initialized in this process. Relaunch the app to boot it again."}];
    }
    return NO;
  }

  _state.running = true;
  _state.bootThreadActive = false;
  _state.configPath = StdStringFromNSString(configPath);

  EmbeddedBootFn embeddedBoot = EmbeddedBootSymbol();
  if (_state.embeddedHostAPILinked && embeddedBoot != nullptr) {
    const char *bootError = NULL;
    if (!embeddedBoot(configPath.UTF8String, &bootError)) {
      _state.running = false;
      _state.statusLine = bootError ? std::string(bootError) : std::string("The embedded iOS host failed to start.");
      [self.controller setFramePumpEnabled:NO];
      [self syncController];
      if (error != nil) {
        *error = [NSError errorWithDomain:X1BoxNativeBridgeErrorDomain
                                     code:16
                                 userInfo:@{NSLocalizedDescriptionKey: NSStringFromStd(_state.statusLine)}];
      }
      return NO;
    }

    _state.coreStartedOnce = true;
    _state.statusLine = "Embedded iOS host API started from ui/xemu.c. The controller now pumps frames through CADisplayLink.";
    [self.controller setFramePumpEnabled:YES];
    [self syncController];
    return YES;
  }

  _state.statusLine = _state.embeddedCoreLinked
    ? "Raw core symbols are linked, but the embedded iOS host API is missing. Falling back to qemu_init/qemu_main bootstrap."
    : "Embedded core symbols are not linked yet. Running the iOS shell fallback.";
  [self.controller setFramePumpEnabled:NO];
  [self syncController];

  if (!_state.embeddedCoreLinked) {
    return YES;
  }

  XemuSettingsSetPathFn setSettingsPath = XemuSettingsSetPathSymbol();
  if (setSettingsPath != nullptr) {
    setSettingsPath(configPath.UTF8String);
  }
  XemuSettingsLoadFn loadSettings = XemuSettingsLoadSymbol();
  XemuSettingsGetErrorMessageFn settingsErrorMessage = XemuSettingsGetErrorMessageSymbol();
  if (loadSettings != nullptr && !loadSettings()) {
    const char *message = settingsErrorMessage != nullptr
      ? settingsErrorMessage()
      : "Failed to load the xemu settings file.";
    _state.running = false;
    _state.statusLine = message ? std::string(message) : std::string("Failed to load the xemu settings file.");
    [self syncController];
    if (error != nil) {
      *error = [NSError errorWithDomain:X1BoxNativeBridgeErrorDomain
                                   code:17
                               userInfo:@{NSLocalizedDescriptionKey: NSStringFromStd(_state.statusLine)}];
    }
    return NO;
  }

  _state.bootThreadActive = true;
  _state.coreStartedOnce = true;
  _state.statusLine =
    "Raw embedded core thread started. This path keeps compatibility, but the preferred iOS route is the embedded host API from ui/xemu.c.";
  [self syncController];
  [self launchRawEmbeddedCoreWithConfigPath:configPath];
  return YES;
}

- (void)stopSession {
  [self.controller setFramePumpEnabled:NO];

  EmbeddedRequestShutdownFn embeddedShutdown = EmbeddedRequestShutdownSymbol();
  QemuSystemPowerdownRequestFn powerdownRequest = QemuSystemPowerdownRequestSymbol();
  if (_state.embeddedHostAPILinked && embeddedShutdown != nullptr) {
    embeddedShutdown();
    _state.statusLine =
      "Embedded iOS guest shutdown requested. Restart the app before a new embedded launch.";
  } else if (_state.embeddedCoreLinked && powerdownRequest != nullptr) {
    powerdownRequest();
    _state.statusLine =
      "Guest powerdown requested. Because qemu_init cannot be safely re-run in the same process, restart the app before a new embedded launch.";
  } else {
    _state.statusLine = "Session cleared in shell mode.";
  }

  _state.running = false;
  _state.bootThreadActive = false;
  _state.pressedButtons.clear();
  _state.axes.clear();
  [self syncController];
}

- (BOOL)canUseNativeSnapshots {
  return NativeSnapshotAPIIsLinked() && _state.running;
}

- (BOOL)saveNativeSnapshotNamed:(NSString *)name error:(NSError * _Nullable __autoreleasing *)error {
  if (name.length == 0) {
    if (error != nil) {
      *error = [NSError errorWithDomain:X1BoxNativeBridgeErrorDomain
                                   code:30
                               userInfo:@{NSLocalizedDescriptionKey: @"Snapshot name is empty."}];
    }
    return NO;
  }

  if (!NativeSnapshotAPIIsLinked()) {
    if (error != nil) {
      *error = [NSError errorWithDomain:X1BoxNativeBridgeErrorDomain
                                   code:31
                               userInfo:@{NSLocalizedDescriptionKey: @"Native snapshot APIs are not linked into the current core build yet."}];
    }
    return NO;
  }

  Error *snapshotError = nullptr;
  SaveSnapshotFn saveSnapshot = SaveSnapshotSymbol();
  if (saveSnapshot == nullptr || !saveSnapshot(name.UTF8String, true, nullptr, false, nullptr, &snapshotError)) {
    _state.statusLine = "Native snapshot save failed.";
    [self syncController];
    if (error != nil) {
      *error = SnapshotNSError(32, "Failed to save native snapshot.", snapshotError);
    }
    return NO;
  }

  _state.statusLine = "Native snapshot saved through the core snapshot API.";
  [self syncController];
  return YES;
}

- (BOOL)loadNativeSnapshotNamed:(NSString *)name error:(NSError * _Nullable __autoreleasing *)error {
  if (name.length == 0) {
    if (error != nil) {
      *error = [NSError errorWithDomain:X1BoxNativeBridgeErrorDomain
                                   code:33
                               userInfo:@{NSLocalizedDescriptionKey: @"Snapshot name is empty."}];
    }
    return NO;
  }

  if (!NativeSnapshotAPIIsLinked()) {
    if (error != nil) {
      *error = [NSError errorWithDomain:X1BoxNativeBridgeErrorDomain
                                   code:34
                               userInfo:@{NSLocalizedDescriptionKey: @"Native snapshot APIs are not linked into the current core build yet."}];
    }
    return NO;
  }

  Error *snapshotError = nullptr;
  LoadSnapshotFn loadSnapshot = LoadSnapshotSymbol();
  if (loadSnapshot == nullptr || !loadSnapshot(name.UTF8String, nullptr, false, nullptr, &snapshotError)) {
    _state.statusLine = "Native snapshot load failed.";
    [self syncController];
    if (error != nil) {
      *error = SnapshotNSError(35, "Failed to load native snapshot.", snapshotError);
    }
    return NO;
  }

  _state.statusLine = "Native snapshot restored through the core snapshot API.";
  [self syncController];
  return YES;
}

- (BOOL)deleteNativeSnapshotNamed:(NSString *)name error:(NSError * _Nullable __autoreleasing *)error {
  if (name.length == 0) {
    if (error != nil) {
      *error = [NSError errorWithDomain:X1BoxNativeBridgeErrorDomain
                                   code:36
                               userInfo:@{NSLocalizedDescriptionKey: @"Snapshot name is empty."}];
    }
    return NO;
  }

  if (!NativeSnapshotAPIIsLinked()) {
    if (error != nil) {
      *error = [NSError errorWithDomain:X1BoxNativeBridgeErrorDomain
                                   code:37
                               userInfo:@{NSLocalizedDescriptionKey: @"Native snapshot APIs are not linked into the current core build yet."}];
    }
    return NO;
  }

  Error *snapshotError = nullptr;
  DeleteSnapshotFn deleteSnapshot = DeleteSnapshotSymbol();
  if (deleteSnapshot == nullptr || !deleteSnapshot(name.UTF8String, false, nullptr, &snapshotError)) {
    _state.statusLine = "Native snapshot delete failed.";
    [self syncController];
    if (error != nil) {
      *error = SnapshotNSError(38, "Failed to delete native snapshot.", snapshotError);
    }
    return NO;
  }

  _state.statusLine = "Native snapshot removed through the core snapshot API.";
  [self syncController];
  return YES;
}

- (void)updateVirtualButton:(NSString *)name pressed:(BOOL)pressed {
  _state.pressedButtons[std::string(name.UTF8String)] = pressed;
}

- (void)updateVirtualAxis:(NSString *)name x:(float)x y:(float)y {
  _state.axes[std::string(name.UTF8String)] = VirtualAxisState{x, y};
}

- (void)launchRawEmbeddedCoreWithConfigPath:(NSString *)configPath {
  NSString *pathCopy = [configPath copy];

  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    @autoreleasepool {
      int exitCode = 1;
      QemuInitFn qemuInit = QemuInitSymbol();
      QemuMainFn qemuMain = QemuMainSymbol();
      if (qemuInit != nullptr && qemuMain != nullptr) {
        std::vector<std::string> argStorage;
        argStorage.emplace_back("xemu");
        argStorage.emplace_back("-config_path");
        argStorage.emplace_back(StdStringFromNSString(pathCopy));

        std::vector<char *> argv;
        argv.reserve(argStorage.size() + 1);
        for (std::string &arg : argStorage) {
          argv.push_back(arg.data());
        }
        argv.push_back(nullptr);

        qemuInit((int)argStorage.size(), argv.data());
        exitCode = qemuMain();
      }

      [pathCopy release];

      dispatch_async(dispatch_get_main_queue(), ^{
        self->_state.bootThreadActive = false;
        self->_state.running = false;
        self->_state.statusLine =
          exitCode == 0
            ? "Raw embedded xemu core exited cleanly."
            : "Raw embedded xemu core returned control to iOS. Inspect the linked backend if video or SDL host setup is still incomplete.";
        [self syncController];
      });
    }
  });
}

- (void)syncController {
  _state.embeddedCoreLinked = EmbeddedCoreIsLinked();
  _state.embeddedHostAPILinked = EmbeddedHostAPIIsLinked();

  EmbeddedGetLastErrorFn lastErrorSymbol = EmbeddedGetLastErrorSymbol();
  if (_state.embeddedHostAPILinked && lastErrorSymbol != nullptr) {
    const char *lastError = lastErrorSymbol();
    if (lastError != nullptr && lastError[0] != '\0' && !_state.running) {
      _state.statusLine = lastError;
    }
  }

  NSString *detail = SummaryFromState(_state);
  dispatch_async(dispatch_get_main_queue(), ^{
    [self.controller applyRunning:self->_state.running detail:detail];
  });
}

@end
