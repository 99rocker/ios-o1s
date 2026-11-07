#pragma once
#include <stdint.h>

#define ACPM_READ	0
#define ACPM_WRITE	1
#define ACPM_UPDATE	2

int acpm_init(void);
int acpm_pmic(unsigned func, unsigned chan, unsigned type, unsigned reg,
	      unsigned value, unsigned mask, uint8_t *out);
