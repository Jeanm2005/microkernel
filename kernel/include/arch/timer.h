#pragma once
#include <stdint.h>

/* Start the periodic scheduler tick at `hz` interrupts per second. Each
 * tick calls sched_timer_tick(). Interrupts must still be disabled; the
 * first tick arrives once something enables them. */
 void arch_timer_init(uint32_t hz);