/* Gatefold NX · tools/bgtest/join/pre.h: force-included into the engine's PC build. The standard headers first, then
 * sdmc:/ mapped to a folder (bgjoin.cpp's card), mallinfo and POSIX's bits the MSVC runtime lacks. */
#pragma once
#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <functional>
#include <map>
#include <random>
#include <set>
#include <unordered_map>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <vector>
#include <direct.h>
#include <malloc.h>
#include <sys/stat.h>

#ifndef S_ISREG
#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#endif
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

struct mallinfo { size_t uordblks; };
struct mallinfo mallinfo_();
#define mallinfo() mallinfo_()

FILE* hostFopen(const char* p, const char* m);
int hostStat(const char* p, struct stat* s);
int hostMkdir(const char* p, int mode);
int hostRemove(const char* p);
int hostRename(const char* a, const char* b);
#define fopen(p, m) hostFopen(p, m)
#define stat(p, s) hostStat(p, s)
#define mkdir(p, m) hostMkdir(p, m)
#define remove(p) hostRemove(p)
#define rename(a, b) hostRename(a, b)

// the test reaches the engine's insides (bgjoin.cpp); after every standard header, and in every file alike (MSVC's names
// say whether a member is private)
#define private public
