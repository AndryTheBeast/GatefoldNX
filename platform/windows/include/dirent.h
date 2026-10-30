/* Songate · dirent.h: folders on Windows (winfiles.cpp). "/" lists the drives. */
#pragma once
enum { DT_UNKNOWN = 0, DT_DIR = 4, DT_REG = 8 };
struct dirent {
    char d_name[1024];   // UTF-8: 255 UTF-16 units can take three bytes each
    unsigned char d_type;
};
struct DIR;
DIR* opendir(const char* path);
dirent* readdir(DIR* d);
int closedir(DIR* d);
