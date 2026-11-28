#include "../AMDRyzenCPUPowerManagement/PStatePolicy.h"
#include <assert.h>

int main(void) {
    const uint64_t windows[] = {160000, 600000, 1000000};
    for (unsigned i = 0; i < sizeof(windows) / sizeof(windows[0]); ++i) {
        uint64_t ticks = windows[i];
        assert(pmRyzen_load_direction(ticks, ticks / 10 * 9) == -1);
        assert(pmRyzen_load_direction(ticks, ticks / 2) == 1);
    }
    assert(pmRyzen_load_direction(100, 88) == 0);
    assert(pmRyzen_load_direction(100, 62) == 0);
    assert(pmRyzen_load_direction(101, 89) == -1);
    assert(pmRyzen_load_direction(101, 62) == 1);
    assert(pmRyzen_load_direction(UINT64_MAX, UINT64_MAX) == -1);
    assert(pmRyzen_load_direction(UINT64_MAX, 0) == 1);
    assert(pmRyzen_load_direction(0, 0) == 0);
    assert(pmRyzen_load_direction(5, 6) == 0);

    assert(!pmRyzen_busy_for_window(1000, 0, 150));
    assert(!pmRyzen_busy_for_window(999, 1000, 150));
    assert(!pmRyzen_busy_for_window(1149, 1000, 150));
    assert(pmRyzen_busy_for_window(1150, 1000, 150));
    const uint64_t delays[] = {10, 50};
    assert(pmRyzen_promotion_interval(delays, 1, 2, 1) == 10);
    assert(pmRyzen_promotion_interval(delays, 2, 2, 1) == 50);
    assert(pmRyzen_promotion_interval(delays, 0, 2, 1) == 0);
    assert(pmRyzen_promotion_interval(delays, 1, 2, 0) == 0);
    assert(pmRyzen_promotion_interval(delays, 1, 0, 1) == 0);
    assert(pmRyzen_promotion_interval(delays, 1, 8, 1) == 0);
    assert(pmRyzen_needs_timer_resync(UINT64_MAX, 0, 110));
    assert(pmRyzen_needs_timer_resync(0, 0, 110));
    assert(!pmRyzen_needs_timer_resync(105, 0, 110));
    assert(!pmRyzen_needs_timer_resync(110, 0, 110));
    assert(pmRyzen_needs_timer_resync(110, 110, 0));
    assert(!pmRyzen_needs_timer_resync(105, 110, 0));
    assert(pmRyzen_needs_timer_resync(110, 110, 150));
    assert(pmRyzen_needs_timer_resync(150, 150, 110));
    assert(!pmRyzen_needs_timer_resync(110, 110, 110));
    assert(pmRyzen_down_windows(2) == 2);
    assert(pmRyzen_down_windows(1) == 6);
    assert(pmRyzen_reconciled_state(0x05, 2, 1) == 2);
    assert(pmRyzen_reconciled_state(0x05, 2, 2) == 2);
    assert(pmRyzen_reconciled_state(0x07, 2, 1) == 1);
    assert(pmRyzen_reconciled_state(0x05, 1, 1) == 0);
    assert(pmRyzen_reconciled_state(0x05, 8, 1) == 0);
    assert(pmRyzen_next_lower(0x05, 0, 1) == 2);
    assert(pmRyzen_next_lower(0x05, 2, 2) == 2);
    assert(pmRyzen_next_lower(0x07, 1, 1) == 1);
    assert(pmRyzen_next_lower(0x07, 1, 2) == 2);
    assert(pmRyzen_next_lower(0x01, 0, 2) == 0);
    assert(pmRyzen_next_lower(0xff, 0, 0) == 0);
}
