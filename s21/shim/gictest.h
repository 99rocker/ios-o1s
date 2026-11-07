#pragma once
#include <stdint.h>

void gictest_run(void);
void gictest_irq(void);
void gictest_report(uintptr_t dtb);
void gictest_delay_ms(unsigned ms);
