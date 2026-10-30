/* Songate · winpath.hpp: UTF-8 "D:/Music/…" paths ↔ the UTF-16 ones Windows takes. */
#pragma once
#include <string>

namespace winpath {
/** "D:/a/b" → L"D:\a\b" ("D:" → L"D:\"); past the old 260-character limit, the \\?\ form. */
std::wstring wide(const char* path);
std::wstring wideText(const std::string& utf8);   // no path rules: only the encoding
std::string utf8(const wchar_t* text);
/** Forward slashes, as source/ writes paths. */
std::string slashes(std::string path);
/** The dev build's read-only folder (GF_READONLY): a write there fails, as on a locked card. Never in a release. */
bool refused(const char* path);
}  // namespace winpath
