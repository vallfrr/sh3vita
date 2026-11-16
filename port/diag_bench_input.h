#pragma once
#include <stdint.h>
void diag_bench_input_enable(int enabled);
int diag_bench_input_active(void);
int diag_bench_input_key(const char *name,int down);
unsigned diag_bench_input_reads(void);
void diag_bench_input_apply(uint8_t *state,unsigned bytes);
