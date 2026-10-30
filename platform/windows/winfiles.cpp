/*
 * Songate · winfiles.cpp
 * The file calls of winshim.h and dirent.h. Everything in source/ reaches the disk through these.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>

#include "dirent.h"
#include "winpath.hpp"

#undef stat
#undef fopen
#undef mkdir
#undef rmdir
#undef remove
#undef rename

namespace winpath {

std::wstring wideText(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::string utf8(const wchar_t* t) {
    if (!t || !*t) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, t, -1, nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n > 0 ? n - 1 : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, t, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::string slashes(std::string p) {
    for (char& c : p) if (c == '\\') c = '/';
    return p;
}

std::wstring wide(const char* path) {
    std::wstring w = wideText(path ? path : "");
    for (wchar_t& c : w) if (c == L'/') c = L'\\';
    while (w.size() > 3 && w.back() == L'\\') w.pop_back();
    if (w.size() == 2 && w[1] == L':') w += L'\\';
    if (w.size() >= 248 && w[1] == L':') w.insert(0, L"\\\\?\\");
    return w;
}

bool refused(const char* path) {
#ifdef GF_DEV
    static const std::string guard = [] {
        const char* e = getenv("GF_READONLY");
        return slashes(e ? e : "");
    }();
    if (guard.empty() || !path) return false;
    const std::string p = slashes(path);
    if (p.size() < guard.size() || _strnicmp(p.c_str(), guard.c_str(), guard.size()) != 0) return false;
    if (p.size() > guard.size() && p[guard.size()] != '/') return false;
    errno = EACCES;
    fprintf(stderr, "read-only: refused %s\n", path);
    return true;
#else
    (void)path;
    return false;
#endif
}

}  // namespace winpath

using namespace winpath;

FILE* gfFopen(const char* path, const char* mode) {
    if (strpbrk(mode, "wa+") && refused(path)) return nullptr;
    // Always binary: the console's files have no line-ending translation, and ftell must count bytes
    wchar_t m[8];
    size_t n = 0;
    bool binary = false;
    for (const char* c = mode; *c && n + 2 < 8; c++) {
        binary = binary || *c == 'b';
        m[n++] = wchar_t(*c);
    }
    if (!binary) m[n++] = L'b';
    m[n] = 0;
    return _wfsopen(wide(path).c_str(), m, _SH_DENYNO);
}

int gf_stat(const char* path, struct gf_stat* st) {
    memset(st, 0, sizeof *st);
    if (path && strcmp(path, "/") == 0) {   // the drives
        st->st_mode = _S_IFDIR;
        return 0;
    }
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(wide(path).c_str(), GetFileExInfoStandard, &a)) {
        errno = ENOENT;
        return -1;
    }
    st->st_mode = (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? _S_IFDIR : _S_IFREG;
    st->st_size = (__int64(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
    const __int64 ft = (__int64(a.ftLastWriteTime.dwHighDateTime) << 32) | a.ftLastWriteTime.dwLowDateTime;
    st->st_mtime = st->st_atime = st->st_ctime = ft / 10000000 - 11644473600LL;
    return 0;
}

int gfMkdir(const char* path) {
    if (refused(path)) return -1;
    if (CreateDirectoryW(wide(path).c_str(), nullptr)) return 0;
    errno = GetLastError() == ERROR_ALREADY_EXISTS ? EEXIST : ENOENT;
    return -1;
}

int gfRmdir(const char* path) {
    if (refused(path)) return -1;
    if (RemoveDirectoryW(wide(path).c_str())) return 0;
    errno = ENOTEMPTY;
    return -1;
}

int gfRemove(const char* path) {
    if (refused(path)) return -1;
    const std::wstring w = wide(path);
    if (DeleteFileW(w.c_str()) || RemoveDirectoryW(w.c_str())) return 0;
    errno = ENOENT;
    return -1;
}

/** As on the console: a file already there is replaced in one step (settings are written beside and renamed over). */
int gfRename(const char* from, const char* to) {
    if (refused(from) || refused(to)) return -1;
    if (MoveFileExW(wide(from).c_str(), wide(to).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) return 0;
    errno = EACCES;
    return -1;
}

struct DIR {
    HANDLE h = INVALID_HANDLE_VALUE;
    WIN32_FIND_DATAW fd{};
    bool first = true;
    DWORD drives = 0;   // "/": the drives left to list
    bool driveList = false;
    dirent ent{};
};

DIR* opendir(const char* path) {
    DIR* d = new DIR;
    if (path && (strcmp(path, "/") == 0 || !*path)) {
        d->driveList = true;
        d->drives = GetLogicalDrives();
        return d;
    }
    std::wstring w = wide(path);
    w += w.back() == L'\\' ? L"*" : L"\\*";
    d->h = FindFirstFileExW(w.c_str(), FindExInfoBasic, &d->fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (d->h == INVALID_HANDLE_VALUE) {
        delete d;
        errno = ENOENT;
        return nullptr;
    }
    return d;
}

dirent* readdir(DIR* d) {
    if (d->driveList) {
        for (int i = 0; i < 26; i++)
            if (d->drives & (1u << i)) {
                d->drives &= ~(1u << i);
                d->ent.d_name[0] = char('A' + i);
                d->ent.d_name[1] = ':';
                d->ent.d_name[2] = 0;
                d->ent.d_type = DT_DIR;
                return &d->ent;
            }
        return nullptr;
    }
    for (;;) {
        if (!d->first && !FindNextFileW(d->h, &d->fd)) return nullptr;
        d->first = false;
        // The system's own (the Recycle Bin, System Volume Information): as a console hides its files
        if ((d->fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) && (d->fd.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM)) continue;
        const std::string name = utf8(d->fd.cFileName);
        snprintf(d->ent.d_name, sizeof d->ent.d_name, "%s", name.c_str());
        d->ent.d_type = (d->fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? DT_DIR : DT_REG;
        return &d->ent;
    }
}

int closedir(DIR* d) {
    if (d->h != INVALID_HANDLE_VALUE) FindClose(d->h);
    delete d;
    return 0;
}
