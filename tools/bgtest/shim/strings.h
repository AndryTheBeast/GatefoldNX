/* bgtest: POSIX strings.h for the MSVC runtime. */
#pragma once
#include <string.h>
#define strncasecmp _strnicmp
#define strcasecmp _stricmp
