/* Gatefold NX preview · a minimal dirent.h over FindFirstFile */
#pragma once
enum { DT_UNKNOWN = 0, DT_DIR = 4, DT_REG = 8 };
struct dirent { char d_name[260]; unsigned char d_type; };
struct DIR;
DIR* opendir(const char* path);
dirent* readdir(DIR* d);
int closedir(DIR* d);
