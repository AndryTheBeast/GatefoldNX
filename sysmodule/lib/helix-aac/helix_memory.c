/* Gatefold NX: Helix's allocation hooks, on the plain heap. */
#include <stdlib.h>
#include "utils/helix_memory.h"
void* helix_malloc(int size) { return calloc(1, (size_t)size); }
void helix_free(void* ptr) { free(ptr); }
