/*
 * PC test for side.c: no board needed.
 *     cd apps/direction
 *     cc -I. test_side.c side.c -lm -o /tmp/test_side && /tmp/test_side
 *
 * Places a sound source on a circle around the tray, computes when it reaches each
 * mic, rounds that to what the board can resolve, and checks side_decide() says
 * LEFT / RIGHT / CENTRE as expected. Then a few failure cases (missing mic, echo).
 * The tray size is a guess (35 cm left-right, 45 cm up-down) until the capsules
 * are measured; the left/right answer only needs the mics on the correct corners.
 */
#include <math.h>
#include <stdio.h>
#include "side.h"

#define SPEED_OF_SOUND  343.0       // m/s
#define WIDTH_M         0.35        // left-right spacing (guess)
#define DEPTH_M         0.45        // up-down spacing (guess)
#define FRAME_NS        20833       // one sample period at 48 kHz
#define SLOT_NS         2917        // ADC time between neighbouring channels
#define CENTRE_NS       100000
#define MAX_DELAY_NS    1700000

static const int mic_slot[SIDE_NUM_MICS] = {0, 1, 2, 3};   // mic n on ADn, lowest first

static int failures;

static void check(int ok, const char *what)
{
	if (!ok) {
		printf("FAIL: %s\n", what);
		failures++;
	}
}

// Arrival times as the board would measure them: the first sample at or after the
// true arrival, i.e. frame time + slot offset, relative to the earliest mic.
static void arrivals_from(double angle_deg, double range_m, int32_t out_ns[])
{
	// x to the right, y towards UP; mic order as in side.h (AD1 up-left, AD2 up-right,
	// AD3 down-left, AD4 down-right).
	const double mx[SIDE_NUM_MICS] = {-WIDTH_M / 2,  WIDTH_M / 2, -WIDTH_M / 2,  WIDTH_M / 2};
	const double my[SIDE_NUM_MICS] = { DEPTH_M / 2,  DEPTH_M / 2, -DEPTH_M / 2, -DEPTH_M / 2};
	const double a = angle_deg * M_PI / 180.0;      // 0 = up, +90 = right
	const double sx = range_m * sin(a), sy = range_m * cos(a);
	double t_ns[SIDE_NUM_MICS], t_min = 1e18;
	for (int m = 0; m < SIDE_NUM_MICS; m++) {
		t_ns[m] = hypot(sx - mx[m], sy - my[m]) / SPEED_OF_SOUND * 1e9;
		if (t_ns[m] < t_min) {
			t_min = t_ns[m];
		}
	}
	for (int m = 0; m < SIDE_NUM_MICS; m++) {
		// Sample k of mic m is taken at k*FRAME + slot*SLOT (+ an arbitrary start).
		double t = t_ns[m] - t_min + 7000.0;   // arbitrary phase vs the sample clock
		int k = (int)ceil((t - mic_slot[m] * SLOT_NS) / FRAME_NS);
		out_ns[m] = k * FRAME_NS + mic_slot[m] * SLOT_NS;
	}
}

int main(void)
{
	printf("angle  side    dt_us     up  down  agree   (0 = up, +90 = right)\n");
	for (int deg = -180; deg <= 180; deg += 15) {
		int32_t t[SIDE_NUM_MICS];
		arrivals_from(deg, 1.0, t);
		side_result_t r = side_decide(t, CENTRE_NS, MAX_DELAY_NS);
		printf("%5d  %-6s %6ld %6ld %5ld  %s\n", deg, side_name(r.side),
				(long)r.dt_ns / 1000, (long)r.up_ns / 1000, (long)r.down_ns / 1000,
				r.agree ? "yes" : "no");

		// Expected: right half -> RIGHT, left half -> LEFT, straight up/down -> CENTRE.
		side_t want = SIDE_CENTRE;
		if (deg > 0 && deg < 180) {
			want = SIDE_RIGHT;
		} else if (deg < 0 && deg > -180) {
			want = SIDE_LEFT;
		}
		// Within ~10 degrees of the axis dt is under 100 us, so CENTRE is also right.
		int near_axis = (deg % 180 == 0) || (deg > -15 && deg < 15)
				|| deg > 165 || deg < -165;
		char what[64];
		snprintf(what, sizeof what, "angle %d gave %s", deg, side_name(r.side));
		check(r.side == want || (near_axis && r.side == SIDE_CENTRE), what);
		check(r.pairs == 2, "all four mics should be heard");
	}

	// Failure cases, all from the right (AD2 and AD4 early). Order: AD1, AD2, AD3, AD4.
	{
		int32_t t[SIDE_NUM_MICS] = {800000, 0, -1, 60000};     // AD3 never crossed
		side_result_t r = side_decide(t, CENTRE_NS, MAX_DELAY_NS);
		check(r.side == SIDE_RIGHT && r.pairs == 1 && r.up_ok && !r.down_ok,
				"one mic missing: up pair alone should say RIGHT");
	}
	{
		int32_t t[SIDE_NUM_MICS] = {-1, 0, -1, 60000};         // left mics too quiet
		side_result_t r = side_decide(t, CENTRE_NS, MAX_DELAY_NS);
		check(r.side == SIDE_RIGHT && r.pairs == 0, "no pair: first mic's side");
	}
	{
		// AD1 "arrives" 1.9 ms late: an echo, not the direct sound. The up pair
		// must be dropped, otherwise dt would be inflated.
		int32_t t[SIDE_NUM_MICS] = {1900000, 0, 850000, 60000};
		side_result_t r = side_decide(t, CENTRE_NS, MAX_DELAY_NS);
		check(r.side == SIDE_RIGHT && !r.up_ok && r.down_ok && r.dt_ns == 790000,
				"echo on AD1 should be ignored");
	}
	{
		// The pairs disagree (one mic late after AGC gain reduction): still a side,
		// but agree must be false so the UI can show it as uncertain.
		int32_t t[SIDE_NUM_MICS] = {0, 300000, 700000, 0};
		side_result_t r = side_decide(t, CENTRE_NS, MAX_DELAY_NS);
		check(!r.agree && r.pairs == 2, "disagreeing pairs must not agree");
	}
	{
		// Left side: AD1 and AD3 early.
		int32_t t[SIDE_NUM_MICS] = {0, 900000, 120000, 1000000};
		side_result_t r = side_decide(t, CENTRE_NS, MAX_DELAY_NS);
		check(r.side == SIDE_LEFT && r.agree && r.first == MIC_UP_LEFT,
				"AD1 and AD3 early should be LEFT");
	}
	{
		int32_t t[SIDE_NUM_MICS] = {-1, -1, -1, -1};
		side_result_t r = side_decide(t, CENTRE_NS, MAX_DELAY_NS);
		check(r.first == -1 && r.side == SIDE_CENTRE, "nothing heard");
	}

	printf(failures ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", failures);
	return failures != 0;
}
