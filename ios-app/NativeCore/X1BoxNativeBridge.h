#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

@interface X1BoxNativeBridge : NSObject

+ (instancetype)shared;

- (UIViewController *)makeEmulatorViewController;
- (BOOL)startSessionWithConfigPath:(NSString *)configPath error:(NSError * _Nullable * _Nullable)error;
- (void)stopSession;
- (void)refreshEmbeddedCoreAvailability;
- (NSString *)embeddedCoreStatusSummary;
- (NSString * _Nullable)resolvedEmbeddedCorePath;
- (BOOL)canUseNativeSnapshots;
- (BOOL)saveNativeSnapshotNamed:(NSString *)name error:(NSError * _Nullable * _Nullable)error;
- (BOOL)loadNativeSnapshotNamed:(NSString *)name error:(NSError * _Nullable * _Nullable)error;
- (BOOL)deleteNativeSnapshotNamed:(NSString *)name error:(NSError * _Nullable * _Nullable)error;
- (void)updateVirtualButton:(NSString *)name pressed:(BOOL)pressed;
- (void)updateVirtualAxis:(NSString *)name x:(float)x y:(float)y;

@end

NS_ASSUME_NONNULL_END
