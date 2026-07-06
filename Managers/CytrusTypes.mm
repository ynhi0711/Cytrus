//
//  CytrusTypes.mm
//  Cytrus (xappify fork)
//
//  See CytrusTypes.h — ObjC port of the Cytrus.swift model classes.
//

#import "CytrusTypes.h"

@implementation CytrusGameInformation
- (instancetype)initWithIdentifier:(uint64_t)identifier
                  kernelMemoryMode:(CytrusKernelMemoryMode)kernelMemoryMode
            new3DSKernelMemoryMode:(CytrusNew3DSKernelMemoryMode)new3DSKernelMemoryMode
                         publisher:(NSString *)publisher
                           regions:(NSString *)regions
                             title:(NSString *)title
                              icon:(nullable NSData *)icon {
    if (self = [super init]) {
        _identifier = identifier;
        _kernelMemoryMode = kernelMemoryMode;
        _new3DSKernelMemoryMode = new3DSKernelMemoryMode;
        _publisher = [publisher copy];
        _regions = [regions copy];
        _title = [title copy];
        _icon = [icon copy];
    }
    return self;
}
@end

@implementation CytrusCheat
- (instancetype)initWithEnabled:(BOOL)enabled
                           code:(NSString *)code
                       comments:(NSString *)comments
                           name:(NSString *)name
                           type:(NSString *)type {
    if (self = [super init]) {
        _enabled = enabled;
        _code = [code copy];
        _comments = [comments copy];
        _name = [name copy];
        _type = [type copy];
    }
    return self;
}
@end

@implementation CytrusSaveState
- (instancetype)initWithSlot:(uint32_t)slot
                      status:(NSInteger)status
                        time:(uint64_t)time
                        name:(NSString *)name {
    if (self = [super init]) {
        _slot = slot;
        _status = status;
        _time = time;
        _name = [name copy];
    }
    return self;
}
@end
