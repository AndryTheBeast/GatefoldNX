/*
 * Songate · winshim.h
 * Force-included into every source on Windows: the C library's POSIX names, with paths in UTF-8 and "/" as source/ writes them
 * (a track called "Björk" or "東京" opens: the plain Windows calls read paths in the system's code page). Standard headers
 * first, so that the macros below cannot touch their declarations.
 */
#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <functional>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <direct.h>
#include <io.h>
#include <sys/stat.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
#include <SDL2/SDL_ttf.h>

#ifdef GF_FFMPEG
extern "C" {
#include <libavformat/avformat.h>
}
#endif

#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#endif

FILE* gfFopen(const char* path, const char* mode);
int gfMkdir(const char* path);
int gfRmdir(const char* path);
int gfRemove(const char* path);
int gfRename(const char* from, const char* to);

// Sizes over 2 GB (a long mix, a backup): the plain struct stat holds 32 bits of them
struct gf_stat : ::_stat64 {};
int gf_stat(const char* path, struct gf_stat* st);

#define stat gf_stat
#define fopen(p, m) gfFopen(p, m)
#define mkdir(p, m) gfMkdir(p)
#define rmdir(p) gfRmdir(p)
#define remove(p) gfRemove(p)
#define rename(a, b) gfRename(a, b)
