#pragma once
#include <stdint.h>
#define DIAG_FILTER_LIMIT 8u
typedef struct { uint32_t items[DIAG_FILTER_LIMIT]; unsigned count; } DiagFilterRefs;
typedef void (*DiagFilterRelease)(void *context,uint32_t filter);
/* Add/remove only manage membership. Caller owns COM AddRef and detach/release. */
int diag_filter_insert(DiagFilterRefs *refs,uint32_t filter); /* 0 success, 1 duplicate, 2 full, -1 invalid */
int diag_filter_remove(DiagFilterRefs *refs,uint32_t filter); /* 1 removed, 0 absent, -1 invalid */
void diag_filter_drain(DiagFilterRefs *refs,DiagFilterRelease release,void *context);
