#ifndef SMCAMD_PSTATE_POLICY_H
#define SMCAMD_PSTATE_POLICY_H

#include <stdint.h>

// These helpers also run in interrupt context. Use integer math throughout.
static inline uint64_t pmRyzen_percent_ticks(uint64_t interval, unsigned percent) {
    return interval / 100 * percent + interval % 100 * percent / 100;
}

static inline int pmRyzen_load_direction(uint64_t total, uint64_t idle) {
    if (!total || idle > total) return 0;
    uint64_t active = total - idle;
    if (active > pmRyzen_percent_ticks(total, 38)) return 1;
    // Above 88% idle is strictly below 12% active, even for fractional ticks.
    return idle > pmRyzen_percent_ticks(total, 88) ? -1 : 0;
}

static inline int pmRyzen_busy_for_window(uint64_t now, uint64_t sinceWake, uint64_t window) {
    return sinceWake && now >= sinceWake && now - sinceWake >= window;
}

// Limit is the number of lower performance levels the governor may use.
// Keep hardware IDs when firmware leaves a hole (e.g. P0, P2 on Renoir).
static inline uint8_t pmRyzen_next_lower(uint8_t mask, uint8_t current, unsigned limit) {
    for (unsigned i = 1; i < 8 && limit; ++i) {
        if (!(mask & (1U << i))) continue;
        if (i > current) return (uint8_t)i;
        --limit;
    }
    return current;
}

#endif
