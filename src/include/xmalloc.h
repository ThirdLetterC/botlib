#ifndef XMALLOC_H
#define XMALLOC_H

#include <stddef.h>

[[nodiscard]] void *xmalloc(size_t size);
[[nodiscard]] void *xrealloc(void *ptr, size_t size);
void xfree(void *ptr);
#endif
