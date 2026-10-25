#pragma once

#include "gui_base.hpp"
#include <tesla.hpp>
#include <functional>
#include <vector>

class PlaylistGui final : public SysTuneGui {
  private:
    tsl::elm::List *m_list;
    std::function<void(u32)> m_on_count_changed;

    /* Ordered pointers to every ButtonListItem in the list (index 0 = entry 1).
       Used for O(n) renumbering after a removal without having to iterate the
       Tesla list by string.  Pointers are owned by m_list — do NOT delete here. */
    std::vector<tsl::elm::ListItem*> m_items;

  public:
    /* on_count_changed is called immediately after any mutation with the new
       playlist size.  Pass nullptr (default) if you don't need the callback. */
    explicit PlaylistGui(std::function<void(u32)> on_count_changed = nullptr);
    ~PlaylistGui();

    tsl::elm::Element *createUI() override;
    void update() override;
    bool handleInput(u64 keysDown, u64 keysHeld, const HidTouchState &touchPos,
                     HidAnalogStickState joyStickPosLeft,
                     HidAnalogStickState joyStickPosRight) override;
};