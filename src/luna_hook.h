#ifndef LUNA_TEST_LUNA_HOOK_H
#define LUNA_TEST_LUNA_HOOK_H
#include <stdlib.h>
#include <stdbool.h>
#include "lua.h"

extern size_t malloc_used_mem(void);

extern size_t malloc_mem_block(void);

extern void mem_info_dump(const char *opts);

extern size_t malloc_current_mem(void);

#endif //LUNA_TEST_LUNA_HOOK_H
