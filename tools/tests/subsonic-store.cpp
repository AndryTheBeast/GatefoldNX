// Run in the test script's disposable directory: Store writes a fake sdmc: tree here.
#include "../../source/store.hpp"
#include <cassert>
#include <filesystem>

int main() {
    std::filesystem::create_directories(Store::Dir);
    Store saved;
    saved.musicRoot = "sdmc:/Music";
    saved.subsonic = { true, "https://music.example/base", "user & café", "secret=+&é" };
    saved.crossfade = 3.5f;
    saved.viewPlaylists = 3;
    saved.eq.on = 1;
    saved.eq.preamp = -3;
    saved.save();
    Store::flush();
    Store loaded;
    loaded.load();
    assert(loaded.musicRoot == saved.musicRoot);
    assert(loaded.subsonic.enabled && loaded.subsonic.url == saved.subsonic.url);
    assert(loaded.subsonic.user == saved.subsonic.user && loaded.subsonic.password == saved.subsonic.password);
    assert(loaded.crossfade == saved.crossfade && loaded.viewPlaylists == saved.viewPlaylists);
    assert(loaded.eq.on && loaded.eq.preamp == saved.eq.preamp);
    saved.subsonic = {};
    saved.save();
    Store::flush();
    loaded.load();
    assert(!loaded.subsonic.enabled && loaded.subsonic.url.empty() && loaded.subsonic.password.empty());
    assert(loaded.musicRoot == saved.musicRoot && loaded.crossfade == saved.crossfade);
    Store::stopWriter();
}
