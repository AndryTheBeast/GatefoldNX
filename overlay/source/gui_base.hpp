#pragma once
#include <tesla.hpp>

class SysTuneGui : public tsl::Gui {
public:
    bool handleInput(u64 keysDown, u64 keysHeld,
                     const HidTouchState &touchPos,
                     HidAnalogStickState joyStickPosLeft,
                     HidAnalogStickState joyStickPosRight) override {
        static bool lastDirectionPressed = true;
        const bool directionPressed = (keysHeld & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT)) != 0;
        if (!directionPressed && lastDirectionPressed)
            tsl::elm::s_directionalKeyReleased.store(true, std::memory_order_release);
        else if (directionPressed && lastDirectionPressed)
            tsl::elm::s_directionalKeyReleased.store(false, std::memory_order_release);
        lastDirectionPressed = directionPressed;

        return tsl::Gui::handleInput(keysDown, keysHeld, touchPos, joyStickPosLeft, joyStickPosRight);
    }
};