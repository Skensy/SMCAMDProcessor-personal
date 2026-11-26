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

// Renoir has only one lower state on some laptops. Time hysteresis therefore
// distinguishes the two automatic modes even with a sparse P-state table.
static inline unsigned pmRyzen_down_windows(unsigned limit) {
    return limit == 2 ? 2 : 6;
}

static inline uint64_t pmRyzen_due_deadline(uint64_t deadline, uint64_t now) {
    return deadline && deadline <= now ? deadline : 0;
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

// Pick the current level if allowed, otherwise the deepest permitted level.
// An unknown hardware request starts from P0.
static inline uint8_t pmRyzen_reconciled_state(uint8_t mask, uint8_t current, unsigned limit) {
    if (current >= 8 || !(mask & (1U << current))) return 0;
    uint8_t selected = 0;
    for (unsigned id = 1; id <= current && limit; ++id) {
        if (mask & (1U << id)) {
            selected = (uint8_t)id;
            --limit;
        }
    }
    return selected;
}

#endif
