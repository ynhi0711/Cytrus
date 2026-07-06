//
//  CytrusTypes.h
//  Cytrus (xappify fork)
//
//  ObjC port of the model types from Cytrus.swift (CytrusGameInformation, CytrusCheat,
//  CytrusSaveState + the kernel memory-mode enums). The upstream ObjC++ managers import
//  these via the Swift interop header ("Cytrus-Swift.h"); this build compiles no Swift
//  (the app's adapter talks ObjC directly), so the data classes live here instead —
//  same approach as ManicEMU's prebuilt (its CitraGameInformation/CitraCheat).
//  Selector/property shapes mirror the @objcMembers Swift originals exactly.
//

#import <Foundation/Foundation.h>

typedef NS_ENUM(uint8_t, CytrusKernelMemoryMode) {
    CytrusKernelMemoryModeProd = 0, // 64 MB
    CytrusKernelMemoryModeDev1,     // 96 MB
    CytrusKernelMemoryModeDev2,     // 80 MB
    CytrusKernelMemoryModeDev3,     // 72 MB
    CytrusKernelMemoryModeDev4,     // 32 MB
};

typedef NS_ENUM(uint8_t, CytrusNew3DSKernelMemoryMode) {
    CytrusNew3DSKernelMemoryModeLegacy = 0, // uses legacy KernelMemoryMode
    CytrusNew3DSKernelMemoryModeProd,       // 124 MB
    CytrusNew3DSKernelMemoryModeDev1,       // 178 MB
    CytrusNew3DSKernelMemoryModeDev2,       // 124 MB
};

NS_ASSUME_NONNULL_BEGIN

@interface CytrusGameInformation : NSObject
@property (nonatomic, readonly) uint64_t identifier;
@property (nonatomic, readonly) CytrusKernelMemoryMode kernelMemoryMode;
@property (nonatomic, readonly) CytrusNew3DSKernelMemoryMode new3DSKernelMemoryMode;
@property (nonatomic, readonly, copy) NSString *publisher;
@property (nonatomic, readonly, copy) NSString *regions;
@property (nonatomic, readonly, copy) NSString *title;
@property (nonatomic, readonly, copy, nullable) NSData *icon;

- (instancetype)initWithIdentifier:(uint64_t)identifier
                  kernelMemoryMode:(CytrusKernelMemoryMode)kernelMemoryMode
            new3DSKernelMemoryMode:(CytrusNew3DSKernelMemoryMode)new3DSKernelMemoryMode
                         publisher:(NSString *)publisher
                           regions:(NSString *)regions
                             title:(NSString *)title
                              icon:(nullable NSData *)icon;
@end

@interface CytrusCheat : NSObject
@property (nonatomic) BOOL enabled;
@property (nonatomic, readonly, copy) NSString *code;
@property (nonatomic, readonly, copy) NSString *comments;
@property (nonatomic, readonly, copy) NSString *name;
@property (nonatomic, readonly, copy) NSString *type;

- (instancetype)initWithEnabled:(BOOL)enabled
                           code:(NSString *)code
                       comments:(NSString *)comments
                           name:(NSString *)name
                           type:(NSString *)type;
@end

@interface CytrusSaveState : NSObject
@property (nonatomic, readonly) uint32_t slot;
@property (nonatomic, readonly) NSInteger status;
@property (nonatomic, readonly) uint64_t time;
@property (nonatomic, readonly, copy) NSString *name;

- (instancetype)initWithSlot:(uint32_t)slot
                      status:(NSInteger)status
                        time:(uint64_t)time
                        name:(NSString *)name;
@end

NS_ASSUME_NONNULL_END
