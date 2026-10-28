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

    /* A long queue shows a window of it (Gatefold's whole library is thousands of tracks: an item each, with its tags
       read from the SD card, ran the overlay out of memory and Atmosphère stopped it). m_start: the queue position of
       m_items[0]; m_first: the list index of the first item (1 after the header, 2 with the window's note). */
    u32 m_start = 0;
    s32 m_first = 1;

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