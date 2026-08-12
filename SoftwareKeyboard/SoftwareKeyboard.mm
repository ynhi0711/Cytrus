//
//  SoftwareKeyboard.mm
//  Cytrus
//
//  Created by Jarrod Norwell on 18/4/2025.
//  Copyright © 2025 Jarrod Norwell. All rights reserved.
//

#import "SoftwareKeyboard.h"
#import "CommonTypes.h"

#include <condition_variable>
#include <memory>
#include <mutex>

@implementation KeyboardConfig
-(KeyboardConfig *) initWithHintText:(NSString *)hintText buttonConfig:(KeyboardButtonConfig)buttonConfig {
    if (self = [super init]) {
        self.hintText = hintText;
        self.buttonConfig = buttonConfig;
    } return self;
}
@end

namespace SoftwareKeyboard {
Keyboard::~Keyboard() = default;

void Keyboard::Execute(const Frontend::KeyboardConfig& config) {
    SoftwareKeyboard::Execute(config);
    
    std::pair<std::string, uint8_t> it = this->GetKeyboardText(config);
    if (this->config.button_config != Frontend::ButtonConfig::None)
        it.second = static_cast<uint8_t>(this->config.button_config);
    
    Finalize(it.first, it.second);
}

void Keyboard::ShowError(const std::string& error) {
    printf("error = %s\n", error.c_str());
}

namespace {
// xappify fork: per-request reply state, owned by a shared_ptr captured BY VALUE in the
// observer block. Upstream captured a stack condition_variable by reference and never
// removed the observer, so every prompt after the first fired stale observers that
// notified a destroyed cv (EXC_BAD_ACCESS in pthread_cond_broadcast).
struct KeyboardResult {
    std::mutex mutex;
    std::condition_variable cv;
    bool ready = false;
    std::string text;
    uint8_t button = 0;
};
}

std::pair<std::string, uint8_t> Keyboard::GetKeyboardText(const Frontend::KeyboardConfig& config) {
    auto state = std::make_shared<KeyboardResult>();

    // Register before posting openKeyboard so a fast reply can't slip past the observer.
    id observer = [[NSNotificationCenter defaultCenter] addObserverForName:@"closeKeyboard" object:NULL queue:[NSOperationQueue mainQueue]
                                                                usingBlock:^(NSNotification *notification) {
        std::lock_guard<std::mutex> lock(state->mutex);
        // xappify fork: unbox the NSNumber — upstream cast the object POINTER to an integer,
        // so every reply looked like a garbage button index.
        state->button = [notification.userInfo[@"buttonPressed"] unsignedIntegerValue];

        NSString *_Nullable text = notification.userInfo[@"keyboardText"];
        const char *_Nullable utf8 = [text UTF8String];
        if (utf8 != NULL)
            state->text = utf8;

        state->ready = true;
        state->cv.notify_all();
    }];

    [[NSNotificationCenter defaultCenter] postNotification:[NSNotification notificationWithName:@"openKeyboard"
                                                                                         object:[[KeyboardConfig alloc] initWithHintText:[NSString stringWithCString:config.hint_text.c_str() encoding:NSUTF8StringEncoding] buttonConfig:static_cast<KeyboardButtonConfig>(config.button_config)]]];

    {
        std::unique_lock<std::mutex> lock(state->mutex);
        state->cv.wait(lock, [&state] { return state->ready; });
    }
    // One prompt = one observer; stray closeKeyboard posts can no longer accumulate
    // observers or touch freed state.
    [[NSNotificationCenter defaultCenter] removeObserver:observer];

    return std::make_pair(state->text, state->button);
}
}
