/* Gatefold NX preview · force-included into every app source.
 * Maps sdmc:/ and romfs:/ to folders on the PC, hides the window and lets a
 * script take screenshots. Standard headers come first so the macros below
 * cannot touch their declarations. */
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

FILE* hostFopen(const char* p, const char* m);
int hostStat(const char* p, struct stat* s);
int hostMkdir(const char* p, int mode);
int hostRemove(const char* p);
int hostRename(const char* a, const char* b);
int hostRmdir(const char* p);
TTF_Font* hostOpenFont(const char* p, int size);
Mix_Music* hostLoadMUS(const char* p);
void hostPresent(SDL_Renderer* r);
const char* hostPath(const char* p);

#define fopen(p, m) hostFopen(p, m)
#define stat(p, s) hostStat(p, s)
#define mkdir(p, m) hostMkdir(p, m)
#define remove(p) hostRemove(p)
#define rename(a, b) hostRename(a, b)
#define rmdir(p) hostRmdir(p)
#define TTF_OpenFont(p, s) hostOpenFont(p, s)
#define Mix_LoadMUS(p) hostLoadMUS(p)
#define SDL_RenderPresent(r) hostPresent(r)
#ifdef GF_FFMPEG
#define avformat_open_input(ctx, url, fmt, opts) avformat_open_input(ctx, hostPath(url), fmt, opts)
#endif
#define SDL_CreateWindow(t, x, y, w, h, f) SDL_CreateWindow(t, x, y, w, h, (f) | (getenv("GF_SHOW") ? 0 : SDL_WINDOW_HIDDEN))
