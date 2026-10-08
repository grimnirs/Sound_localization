/*
 * side.c: the left/right decision. See side.h for the interface.
 *
 * Seen from above, with UP pointing away from you:
 *
 *              UP
 *      AD1 ---------- AD2
 *       |              |
 *  LEFT |              | RIGHT
 *       |              |
 *      AD3 ---------- AD4
 *             DOWN
 *
 * Sound from the right reaches AD2 and AD4 before AD1 and AD3. How much earlier
 * depends on the angle: for a source far away compared with the tray, the extra
 * path to the left-hand mics is (left-right spacing) * sin(angle), so
 *     dt = t_left - t_right = spacing * sin(angle) / 343 m/s.
 * The tray gives us that measurement twice, once with the up pair (AD1 vs AD2) and
 * once with the down pair (AD3 vs AD4). Both see the same left-right spacing, so for a
 * distant source they should agree, and averaging them halves the effect of a
 * single mic crossing the threshold early or late (AGC, noise).
 *
 * The sign of dt is all we need for left/right. Its size is the step after this
 * one: angle = asin(343 * dt / spacing) once the capsule spacing is measured.
 */
#include "side.h"

static const side_t side_of_mic[SIDE_NUM_MICS] = {
	[MIC_UP_LEFT]     = SIDE_LEFT,
	[MIC_UP_RIGHT]    = SIDE_RIGHT,
	[MIC_DOWN_LEFT]   = SIDE_LEFT,
	[MIC_DOWN_RIGHT]  = SIDE_RIGHT,
};

static side_t classify(int32_t dt_ns, int32_t centre_ns)
{
	if (dt_ns > centre_ns) {
		return SIDE_RIGHT;
	}
	if (dt_ns < -centre_ns) {
		return SIDE_LEFT;
	}
	return SIDE_CENTRE;
}

// One pair's time difference, left arrival minus right arrival. Only valid when
// both mics heard the direct sound.
static bool pair_dt(const int32_t arrival_ns[], const bool heard[],
		uint8_t left, uint8_t right, int32_t *dt_ns)
{
	if (!heard[left] || !heard[right]) {
		return false;
	}
	*dt_ns = arrival_ns[left] - arrival_ns[right];
	return true;
}

side_result_t side_decide(const int32_t arrival_ns[SIDE_NUM_MICS],
		int32_t centre_ns, int32_t max_delay_ns)
{
	side_result_t r = {
		.side = SIDE_CENTRE,
		.first = -1,
	};

	// 1. The earliest mic. Its arrival is the reference for the echo check.
	for (uint8_t m = 0; m < SIDE_NUM_MICS; m++) {
		if (arrival_ns[m] >= 0
				&& (r.first < 0 || arrival_ns[m] < arrival_ns[r.first])) {
			r.first = m;
		}
	}
	if (r.first < 0) {
		return r;    // nobody heard anything; can't happen after a real trigger
	}

	// 2. Which mics heard the direct sound (crossed, and not suspiciously late).
	bool heard[SIDE_NUM_MICS];
	for (uint8_t m = 0; m < SIDE_NUM_MICS; m++) {
		heard[m] = arrival_ns[m] >= 0
				&& arrival_ns[m] - arrival_ns[r.first] <= max_delay_ns;
	}

	// 3. Time difference per pair, then combine.
	r.up_ok = pair_dt(arrival_ns, heard, MIC_UP_LEFT, MIC_UP_RIGHT, &r.up_ns);
	r.down_ok = pair_dt(arrival_ns, heard, MIC_DOWN_LEFT, MIC_DOWN_RIGHT, &r.down_ns);
	r.pairs = (uint8_t)r.up_ok + (uint8_t)r.down_ok;

	if (r.pairs == 2) {
		r.dt_ns = (r.up_ns + r.down_ns) / 2;
		r.agree = classify(r.up_ns, centre_ns) == classify(r.down_ns, centre_ns);
	} else if (r.up_ok) {
		r.dt_ns = r.up_ns;
	} else if (r.down_ok) {
		r.dt_ns = r.down_ns;
	}

	if (r.pairs > 0) {
		r.side = classify(r.dt_ns, centre_ns);
	} else {
		// No complete pair, e.g. only the two right-hand mics crossed because the
		// left ones were too quiet. Weakest evidence: go with the first mic's side.
		r.side = side_of_mic[r.first];
	}
	return r;
}

const char *side_name(side_t side)
{
	switch (side) {
	case SIDE_LEFT:  return "LEFT";
	case SIDE_RIGHT: return "RIGHT";
	default:         return "CENTRE";
	}
}
