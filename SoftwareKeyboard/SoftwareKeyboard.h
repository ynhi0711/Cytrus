//
//  Keyboard.h
//  Cytrus
//
//  Created by Jarrod Norwell on 18/4/2025.
//  Copyright © 2025 Jarrod Norwell. All rights reserved.
//

#import <Foundation/Foundation.h>

#ifdef __cplusplus
#include "core/frontend/applets/swkbd.h"

#include <string>
#include <utility>

namespace SoftwareKeyboard {
class Keyboard final : public Frontend::SoftwareKeyboard {
public:
    ~Keyboard();

    void Execute(const Frontend::KeyboardConfig& config) override;
    void ShowError(const std::string& error) override;

    std::pair<std::string, uint8_t> GetKeyboardText(const Frontend::KeyboardConfig& config);
};
}
#endif
