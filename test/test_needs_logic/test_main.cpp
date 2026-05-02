// Unit tests for src/needs_logic.{h,cpp}.
//
// Runs on the native PIO platform — no Arduino, no M5, no hardware.
// Build & execute:
//
//     pio test -e native
//
// The test_build_src=no setting in platformio.ini means we explicitly
// pull in needs_logic.cpp here via test_filter so the production sources
// don't get compiled (they pull in Arduino headers that aren't available
// on native).

#include <unity.h>
#include "../src/needs_logic.h"
// Bring the implementation into the test translation unit. test_build_src
// is no, so PIO won't compile src/ for the native env — including the .cpp
// here keeps the tests self-contained.
#include "../src/needs_logic.cpp"

// ─── computeMood ──────────────────────────────────────────────────────────

void test_computeMood_returns_happiness_when_others_above_30(void) {
    // happiness alone determines mood when energy & fullness ≥ 30
    Needs n{60, 80, 80};
    TEST_ASSERT_EQUAL_UINT8(60, computeMood(n));

    n = Needs{95, 50, 100};
    TEST_ASSERT_EQUAL_UINT8(95, computeMood(n));
}

void test_computeMood_low_energy_drags_down(void) {
    // energy=15 → caps mood at 15*100/30 = 50, even with high happiness
    Needs n{90, 15, 90};
    TEST_ASSERT_EQUAL_UINT8(50, computeMood(n));
}

void test_computeMood_low_fullness_drags_down(void) {
    Needs n{90, 90, 9};
    // fullness=9 → caps at 9*100/30 = 30
    TEST_ASSERT_EQUAL_UINT8(30, computeMood(n));
}

void test_computeMood_zero_critical_need_means_zero(void) {
    Needs n{100, 0, 100};
    TEST_ASSERT_EQUAL_UINT8(0, computeMood(n));

    n = Needs{100, 100, 0};
    TEST_ASSERT_EQUAL_UINT8(0, computeMood(n));
}

void test_computeMood_takes_lower_of_critical_needs(void) {
    // both energy and fullness below 30; lower one wins
    Needs n{80, 15, 9};   // energy → 50, fullness → 30 → result 30
    TEST_ASSERT_EQUAL_UINT8(30, computeMood(n));
}

void test_computeMood_threshold_boundary(void) {
    // exactly 30 should NOT drag mood down
    Needs n{70, 30, 30};
    TEST_ASSERT_EQUAL_UINT8(70, computeMood(n));
}

// ─── decayNeeds ────────────────────────────────────────────────────────────

void test_decayNeeds_does_nothing_under_one_second(void) {
    Needs n{50, 50, 50};
    uint32_t last = 1000;
    decayNeeds(n, 1500, last, false);  // 500 ms elapsed
    TEST_ASSERT_EQUAL_UINT8(50, n.happiness);
    TEST_ASSERT_EQUAL_UINT8(50, n.energy);
    TEST_ASSERT_EQUAL_UINT8(50, n.fullness);
    TEST_ASSERT_EQUAL_UINT32(1000, last);
}

void test_decayNeeds_first_call_sets_baseline(void) {
    // last_decay_ms = 0 → seeded to now_ms; no actual decay happens.
    Needs n{50, 50, 50};
    uint32_t last = 0;
    decayNeeds(n, 12345, last, false);
    TEST_ASSERT_EQUAL_UINT32(12345, last);
    TEST_ASSERT_EQUAL_UINT8(50, n.happiness);
}

void test_decayNeeds_awake_decays_at_documented_rates(void) {
    // 60 ticks of 1 s while awake:
    //   happiness: −1 every 5 s → −12
    //   energy:    −1 every 10 s → −6
    //   fullness:  −1 every 12 s → −5
    // Seed time is deliberately non-zero: needs_logic.cpp uses
    // `last_decay_ms == 0` as the "not yet seeded" sentinel, so
    // seeding with now_ms=0 collides with the sentinel and leaves
    // last_decay_ms unset.
    Needs n{100, 100, 100};
    uint32_t last = 0;
    decayNeeds(n, 1000, last, false);           // seed at t=1 s
    decayNeeds(n, 61000, last, false);          // 60 ticks elapsed
    TEST_ASSERT_EQUAL_UINT8(100 - 12, n.happiness);
    TEST_ASSERT_EQUAL_UINT8(100 - 6,  n.energy);
    TEST_ASSERT_EQUAL_UINT8(100 - 5,  n.fullness);
}

void test_decayNeeds_sleeping_only_regenerates_energy(void) {
    // 20 ticks of 1 s while sleeping:
    //   energy: +1 every 2 s → +10
    //   happiness, fullness: frozen
    Needs n{50, 50, 50};
    uint32_t last = 0;
    decayNeeds(n, 1000, last, true);            // seed at t=1 s
    decayNeeds(n, 21000, last, true);           // 20 ticks elapsed
    TEST_ASSERT_EQUAL_UINT8(50 + 10, n.energy);
    TEST_ASSERT_EQUAL_UINT8(50, n.happiness);
    TEST_ASSERT_EQUAL_UINT8(50, n.fullness);
}

void test_decayNeeds_clamps_at_floor_zero(void) {
    Needs n{0, 0, 0};
    uint32_t last = 0;
    decayNeeds(n, 0, last, false);
    decayNeeds(n, 60000, last, false);
    TEST_ASSERT_EQUAL_UINT8(0, n.happiness);
    TEST_ASSERT_EQUAL_UINT8(0, n.energy);
    TEST_ASSERT_EQUAL_UINT8(0, n.fullness);
}

void test_decayNeeds_clamps_at_ceiling_100(void) {
    // sleeping pet at 100 energy stays at 100
    Needs n{50, 100, 50};
    uint32_t last = 0;
    decayNeeds(n, 0, last, true);
    decayNeeds(n, 60000, last, true);
    TEST_ASSERT_EQUAL_UINT8(100, n.energy);
}

// ─── Unity entry point ─────────────────────────────────────────────────────

void setUp(void)    {}
void tearDown(void) {}

int main(int /*argc*/, char** /*argv*/) {
    UNITY_BEGIN();
    RUN_TEST(test_computeMood_returns_happiness_when_others_above_30);
    RUN_TEST(test_computeMood_low_energy_drags_down);
    RUN_TEST(test_computeMood_low_fullness_drags_down);
    RUN_TEST(test_computeMood_zero_critical_need_means_zero);
    RUN_TEST(test_computeMood_takes_lower_of_critical_needs);
    RUN_TEST(test_computeMood_threshold_boundary);
    RUN_TEST(test_decayNeeds_does_nothing_under_one_second);
    RUN_TEST(test_decayNeeds_first_call_sets_baseline);
    RUN_TEST(test_decayNeeds_awake_decays_at_documented_rates);
    RUN_TEST(test_decayNeeds_sleeping_only_regenerates_energy);
    RUN_TEST(test_decayNeeds_clamps_at_floor_zero);
    RUN_TEST(test_decayNeeds_clamps_at_ceiling_100);
    return UNITY_END();
}
