// Gatefold NX · plicons.inc
// The icons a playlist can wear (the playlist page's ⋯ → Icon): Gatefold's own symbols (icons.inc) and more drawn
// in the same line style (24 px grid, stroke 1.9, round caps and joins). The keys are saved (playlist-icons.tsv):
// never rename one. Include after icons.inc.
#pragma once

#define PLI_SVG R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 24 24" fill="none" stroke="#ffffff" stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round">)SVG"
#define PLI_DOT(x, y) R"SVG(<circle cx=")SVG" #x R"SVG(" cy=")SVG" #y R"SVG(" r="1.15" fill="#ffffff" stroke="none"/>)SVG"

inline constexpr char PLI_NOTES[] = PLI_SVG R"SVG(<path d="M12 17.5V3.5l6 2.5"/><circle cx="9" cy="17.5" r="3"/></svg>)SVG";
inline constexpr char PLI_HEADPHONES[] = PLI_SVG R"SVG(<path d="M3.5 17v-5a8.5 8.5 0 0 1 17 0v5"/><path d="M20.5 18a2.5 2.5 0 0 1-2.5 2.5h-.5a1.5 1.5 0 0 1-1.5-1.5v-4a1.5 1.5 0 0 1 1.5-1.5h3zM3.5 18A2.5 2.5 0 0 0 6 20.5h.5A1.5 1.5 0 0 0 8 19v-4a1.5 1.5 0 0 0-1.5-1.5h-3z"/></svg>)SVG";
inline constexpr char PLI_GUITAR[] = PLI_SVG R"SVG(<path d="M14 10l6-6M18.5 2.5l3 3"/><path d="M13.6 10.4a3.4 3.4 0 0 0-4.8 0c-.7.7-.8 1.7-1.7 2.1-1 .4-2.1.1-3 1a4.6 4.6 0 0 0 6.5 6.5c.9-.9.6-2 1-3 .4-.9 1.4-1 2.1-1.7a3.4 3.4 0 0 0 0-4.8z"/><circle cx="10" cy="14" r="1.4"/></svg>)SVG";
inline constexpr char PLI_PIANO[] = PLI_SVG R"SVG(<rect x="3" y="4" width="18" height="16" rx="2"/><path d="M7.5 14v6M12 14v6M16.5 14v6"/><rect x="6.2" y="4" width="2.6" height="10" rx=".6" fill="#ffffff" stroke="none"/><rect x="10.7" y="4" width="2.6" height="10" rx=".6" fill="#ffffff" stroke="none"/><rect x="15.2" y="4" width="2.6" height="10" rx=".6" fill="#ffffff" stroke="none"/></svg>)SVG";
inline constexpr char PLI_DRUM[] = PLI_SVG R"SVG(<ellipse cx="12" cy="9" rx="8" ry="3"/><path d="M4 9v7.5c0 1.7 3.6 3 8 3s8-1.3 8-3V9"/><path d="M8 11.6v7.4M16 11.6v7.4M6.5 2.5l9 4M17.5 2.5l-9 4"/></svg>)SVG";
inline constexpr char PLI_RADIO[] = PLI_SVG R"SVG(<rect x="3" y="8" width="18" height="12" rx="2"/><path d="M7 8l10-5"/><circle cx="15.5" cy="14" r="2.5"/><path d="M6.5 12.5h3M6.5 15.5h3"/></svg>)SVG";
inline constexpr char PLI_SPEAKER[] = PLI_SVG R"SVG(<rect x="5" y="2.5" width="14" height="19" rx="2.5"/><circle cx="12" cy="14.5" r="3.5"/>)SVG" PLI_DOT(12, 7) R"SVG(</svg>)SVG";
inline constexpr char PLI_CASSETTE[] = PLI_SVG R"SVG(<rect x="2.5" y="5" width="19" height="14" rx="2"/><circle cx="8" cy="11" r="2"/><circle cx="16" cy="11" r="2"/><path d="M10 11h4M6.5 19l1.5-3h8l1.5 3"/></svg>)SVG";
inline constexpr char PLI_STAR[] = PLI_SVG R"SVG(<path d="M12 3.5l2.6 5.3 5.8.8-4.2 4.1 1 5.8L12 16.8l-5.2 2.7 1-5.8-4.2-4.1 5.8-.8z"/></svg>)SVG";
inline constexpr char PLI_FLAME[] = PLI_SVG R"SVG(<path d="M12 21a6 6 0 0 0 6-6c0-4-3-6.5-4.5-11-1 3.2-2.6 4.6-3.6 6C9 9 8.6 8 8.6 6.6 7 8.2 6 10.6 6 15a6 6 0 0 0 6 6z"/></svg>)SVG";
inline constexpr char PLI_SMILE[] = PLI_SVG R"SVG(<circle cx="12" cy="12" r="9"/><path d="M8.5 14.3a4.5 4.5 0 0 0 7 0"/>)SVG" PLI_DOT(9, 9.6) PLI_DOT(15, 9.6) R"SVG(</svg>)SVG";
inline constexpr char PLI_CLOUD[] = PLI_SVG R"SVG(<path d="M7 18a4.5 4.5 0 1 1 .9-8.9A6 6 0 0 1 19 11a3.5 3.5 0 0 1-.5 7z"/></svg>)SVG";
inline constexpr char PLI_RAIN[] = PLI_SVG R"SVG(<path d="M7 14a4.5 4.5 0 1 1 .9-8.9A6 6 0 0 1 19 7a3.5 3.5 0 0 1-.5 7z"/><path d="M8 17.5l-1 3M12.5 17.5l-1 3M17 17.5l-1 3"/></svg>)SVG";
inline constexpr char PLI_SNOW[] = PLI_SVG R"SVG(<path d="M12 2.5v19M3.8 7.2l16.4 9.6M3.8 16.8l16.4-9.6"/><path d="M9.6 4 12 6.3 14.4 4M9.6 20 12 17.7l2.4 2.3"/></svg>)SVG";
inline constexpr char PLI_LEAF[] = PLI_SVG R"SVG(<path d="M5 19c0-8 5-14 15-15-1 10-7 15-15 15z"/><path d="M5 19l8-8"/></svg>)SVG";
inline constexpr char PLI_FLOWER[] = PLI_SVG R"SVG(<circle cx="12" cy="12" r="2.5"/><path d="M12 9.5a3 3 0 1 1 0-6 3 3 0 1 1 0 6zM12 14.5a3 3 0 1 1 0 6 3 3 0 1 1 0-6zM9.5 12a3 3 0 1 1-6 0 3 3 0 1 1 6 0zM14.5 12a3 3 0 1 1 6 0 3 3 0 1 1-6 0z"/></svg>)SVG";
inline constexpr char PLI_TREE[] = PLI_SVG R"SVG(<path d="M12 3 6.5 10.5h3L5 17h14l-4.5-6.5h3z"/><path d="M12 17v4"/></svg>)SVG";
inline constexpr char PLI_MOUNTAIN[] = PLI_SVG R"SVG(<path d="M2.5 20 9 8.5l4 6.5 2.5-3.5 6 8.5z"/></svg>)SVG";
inline constexpr char PLI_WAVES[] = PLI_SVG R"SVG(<path d="M2.5 8.5c2-1.6 4-1.6 6 0s4 1.6 6 0 4-1.6 6.5 0M2.5 13c2-1.6 4-1.6 6 0s4 1.6 6 0 4-1.6 6.5 0M2.5 17.5c2-1.6 4-1.6 6 0s4 1.6 6 0 4-1.6 6.5 0"/></svg>)SVG";
inline constexpr char PLI_COFFEE[] = PLI_SVG R"SVG(<path d="M4 8h12v5.5a5.5 5.5 0 0 1-5.5 5.5h-1A5.5 5.5 0 0 1 4 13.5z"/><path d="M16 10h1.5a2.5 2.5 0 0 1 0 5H16M8 2.5V5M12 2.5V5M3 21.5h14"/></svg>)SVG";
inline constexpr char PLI_GLASS[] = PLI_SVG R"SVG(<path d="M5 4h14l-7 8z"/><path d="M12 12v8M8 20h8M8 7.5h8"/></svg>)SVG";
inline constexpr char PLI_DUMBBELL[] = PLI_SVG R"SVG(<path d="M6.5 6.5v11M17.5 6.5v11M3.5 9v6M20.5 9v6M6.5 12h11"/></svg>)SVG";
inline constexpr char PLI_BIKE[] = PLI_SVG R"SVG(<circle cx="5.5" cy="16.5" r="3.5"/><circle cx="18.5" cy="16.5" r="3.5"/><path d="M5.5 16.5 9 10h6.5l3 6.5M12 16.5 15.5 10M7.5 7h3M14.5 6.5h2.5"/><path d="M9 10 8 7"/></svg>)SVG";
inline constexpr char PLI_CAR[] = PLI_SVG R"SVG(<path d="M3.5 16.5v-4l2.2-5A1.5 1.5 0 0 1 7.1 6.5h9.8a1.5 1.5 0 0 1 1.4 1l2.2 5v4z"/><path d="M3.5 12.5h17"/><path d="M6 16.5V19M18 16.5V19"/>)SVG" PLI_DOT(7.5, 14.5) PLI_DOT(16.5, 14.5) R"SVG(</svg>)SVG";
inline constexpr char PLI_GLOBE[] = PLI_SVG R"SVG(<circle cx="12" cy="12" r="9"/><path d="M3 12h18M12 3a14 14 0 0 1 0 18 14 14 0 0 1 0-18z"/></svg>)SVG";
inline constexpr char PLI_HOME[] = PLI_SVG R"SVG(<path d="M3 11 12 3.5 21 11"/><path d="M5.5 9v11h13V9"/><path d="M10 20v-5.5h4V20"/></svg>)SVG";
inline constexpr char PLI_BRIEFCASE[] = PLI_SVG R"SVG(<rect x="3" y="7" width="18" height="13" rx="2"/><path d="M9 7V5.5A1.5 1.5 0 0 1 10.5 4h3A1.5 1.5 0 0 1 15 5.5V7M3 13h18"/></svg>)SVG";
inline constexpr char PLI_BOOK[] = PLI_SVG R"SVG(<path d="M4 4.5A1.5 1.5 0 0 1 5.5 3H20v15H5.5A1.5 1.5 0 0 0 4 19.5zM4 19.5A1.5 1.5 0 0 0 5.5 21H20v-3"/></svg>)SVG";
inline constexpr char PLI_BED[] = PLI_SVG R"SVG(<path d="M3 19V6M3 15h18v4M21 15v-3a3 3 0 0 0-3-3h-7v6"/><circle cx="7" cy="11.5" r="2"/></svg>)SVG";
inline constexpr char PLI_BULB[] = PLI_SVG R"SVG(<path d="M9 18h6M10 21h4M12 3a6 6 0 0 0-3.5 10.9c.6.5 1 1.2 1 2.1h5c0-.9.4-1.6 1-2.1A6 6 0 0 0 12 3z"/></svg>)SVG";
inline constexpr char PLI_CODE[] = PLI_SVG R"SVG(<path d="m8 7-5 5 5 5M16 7l5 5-5 5M14 4l-4 16"/></svg>)SVG";
inline constexpr char PLI_ROCKET[] = PLI_SVG R"SVG(<path d="M12 2.5c2.5 2 4 5 4 8.5v5H8v-5c0-3.5 1.5-6.5 4-8.5z"/><path d="M8 12.5 5 15.5V19l3-2.5M16 12.5l3 3V19l-3-2.5M10.5 19.5 12 22l1.5-2.5"/><circle cx="12" cy="9.5" r="1.6"/></svg>)SVG";
inline constexpr char PLI_CROWN[] = PLI_SVG R"SVG(<path d="M3 8l4.5 4L12 5l4.5 7L21 8l-2 11H5z"/></svg>)SVG";
inline constexpr char PLI_GEM[] = PLI_SVG R"SVG(<path d="M6 3.5h12l3.5 5L12 21 2.5 8.5z"/><path d="M2.5 8.5h19M9.5 3.5 8 8.5l4 12.5 4-12.5-1.5-5"/></svg>)SVG";
inline constexpr char PLI_TARGET[] = PLI_SVG R"SVG(<circle cx="12" cy="12" r="9"/><circle cx="12" cy="12" r="5"/>)SVG" PLI_DOT(12, 12) R"SVG(</svg>)SVG";
inline constexpr char PLI_FLAG[] = PLI_SVG R"SVG(<path d="M5 21V4M5 4h12l-2.5 4 2.5 4H5"/></svg>)SVG";
inline constexpr char PLI_BOOKMARK[] = PLI_SVG R"SVG(<path d="M6.5 3.5h11v17L12 16.5l-5.5 4z"/></svg>)SVG";
inline constexpr char PLI_GHOST[] = PLI_SVG R"SVG(<path d="M5 20.5V11a7 7 0 0 1 14 0v9.5l-2.3-1.6-2.4 1.6-2.3-1.6-2.3 1.6-2.4-1.6z"/>)SVG" PLI_DOT(9.5, 11) PLI_DOT(14.5, 11) R"SVG(</svg>)SVG";
inline constexpr char PLI_INFINITY[] = PLI_SVG R"SVG(<path d="M12 12c-2-2.7-4-4-6-4a4 4 0 0 0 0 8c2 0 4-1.3 6-4zM12 12c2 2.7 4 4 6 4a4 4 0 0 0 0-8c-2 0-4 1.3-6 4z"/></svg>)SVG";

// Files: any other file, and the tab's own icon (an SD card)
inline constexpr char PLI_FILE[] = PLI_SVG R"SVG(<path d="M14 3H7a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2V8z"/><path d="M14 3v5h5"/></svg>)SVG";
inline constexpr char PLI_SDCARD[] = PLI_SVG R"SVG(<path d="M9 3h8.5A1.5 1.5 0 0 1 19 4.5v15a1.5 1.5 0 0 1-1.5 1.5h-11A1.5 1.5 0 0 1 5 19.5V7z"/><path d="M10 7v3M13 7v3M16 7v3"/></svg>)SVG";

// Settings → Appearance → Sidebar: a tab one place up or down
inline constexpr char PLI_UP[] = PLI_SVG R"SVG(<path d="M12 19V5M6 11l6-6 6 6"/></svg>)SVG";
inline constexpr char PLI_DOWN[] = PLI_SVG R"SVG(<path d="M12 5v14M6 13l6 6 6-6"/></svg>)SVG";

#undef PLI_DOT
#undef PLI_SVG

/** One a tile in the icon sheet, in this order: music first, then moods, nature, places and things. */
struct PlaylistIcon { const char* key; const char* svg; };
inline const PlaylistIcon kPlaylistIcons[] = {
    { "list", ICON_LIST },          { "folder", ICON_FOLDER },      { "notes", PLI_NOTES },         { "music", ICON_MUSIC },
    { "disc", ICON_DISC },          { "headphones", PLI_HEADPHONES }, { "mic", ICON_MIC },        { "guitar", PLI_GUITAR },
    { "piano", PLI_PIANO },         { "drum", PLI_DRUM },           { "radio", PLI_RADIO },         { "speaker", PLI_SPEAKER },
    { "cassette", PLI_CASSETTE },   { "wave", ICON_WAVE },          { "eq", ICON_EQ },              { "sliders", ICON_SLIDERS },
    { "heart", ICON_HEART },        { "star", PLI_STAR },           { "sparkle", ICON_SPARKLE },    { "bolt", ICON_BOLT },
    { "flame", PLI_FLAME },         { "smile", PLI_SMILE },         { "crown", PLI_CROWN },         { "gem", PLI_GEM },
    { "sun", ICON_SUN },            { "moon", ICON_MOON },          { "cloud", PLI_CLOUD },         { "rain", PLI_RAIN },
    { "snow", PLI_SNOW },           { "leaf", PLI_LEAF },           { "flower", PLI_FLOWER },       { "tree", PLI_TREE },
    { "mountain", PLI_MOUNTAIN },   { "waves", PLI_WAVES },         { "globe", PLI_GLOBE },         { "home", PLI_HOME },
    { "coffee", PLI_COFFEE },       { "glass", PLI_GLASS },         { "dumbbell", PLI_DUMBBELL },   { "bike", PLI_BIKE },
    { "car", PLI_CAR },             { "rocket", PLI_ROCKET },       { "game", ICON_GAME },          { "book", PLI_BOOK },
    { "briefcase", PLI_BRIEFCASE }, { "code", PLI_CODE },           { "bulb", PLI_BULB },           { "bed", PLI_BED },
    { "history", ICON_HISTORY },    { "calendar", ICON_CALENDAR },  { "hourglass", ICON_HOURGLASS }, { "target", PLI_TARGET },
    { "flag", PLI_FLAG },           { "bookmark", PLI_BOOKMARK },   { "ghost", PLI_GHOST },         { "infinity", PLI_INFINITY },
};

/** The icon saved under a key, or nullptr when there is none (a key from a later version). */
inline const char* playlistIconSvg(const std::string& key) {
    for (const PlaylistIcon& i : kPlaylistIcons) if (key == i.key) return i.svg;
    return nullptr;
}
