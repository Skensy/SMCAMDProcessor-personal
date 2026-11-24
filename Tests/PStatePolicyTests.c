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
    assert(pmRyzen_next_lower(0x05, 0, 1) == 2);
    assert(pmRyzen_next_lower(0x05, 2, 2) == 2);
    assert(pmRyzen_next_lower(0x07, 1, 1) == 1);
    assert(pmRyzen_next_lower(0x07, 1, 2) == 2);
    assert(pmRyzen_next_lower(0x01, 0, 2) == 0);
    assert(pmRyzen_next_lower(0xff, 0, 0) == 0);
}
