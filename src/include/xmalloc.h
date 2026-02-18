#pragma once

#include <stddef.h>

[[nodiscard]] void *xmalloc(size_t size);
[[nodiscard]] void *xrealloc(void *ptr, size_t size);
void xfree(void *ptr);
