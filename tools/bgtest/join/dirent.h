/* Gatefold NX · tools/bgtest/join/dirent.h: no folders are listed in the join test (the random-folder action is not run). */
#pragma once
#define DT_DIR 4
#define DT_REG 8
struct dirent { unsigned char d_type; char d_name[256]; };
struct DIR;
inline DIR* opendir(const char*) { return nullptr; }
inline dirent* readdir(DIR*) { return nullptr; }
inline int closedir(DIR*) { return 0; }
