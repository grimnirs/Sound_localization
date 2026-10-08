/*
 * side.h: decide LEFT / RIGHT / CENTRE from the arrival times of the four mics.
 *
 * This file and side.c are deliberately hardware-free (no ASF, no registers), so
 * the same code compiles on a PC. test_side.c feeds it arrival times computed
 * from known source positions and checks the answers:
 *     cc -I. test_side.c side.c -lm -o /tmp/test_side && /tmp/test_side
 * Keeping the "thinking" apart from the "sampling" is what makes it testable
 * without the board.
 */
#ifndef SIDE_H
#define SIDE_H

#include <stdbool.h>
#include <stdint.h>

#define SIDE_NUM_MICS     4

// Mic index = mic number - 1, and mic n is wired to ADC channel ADn.
// Square seen from above, UP pointing away from you (layout of 2026-10-08):
//     AD1 up-left    AD2 up-right
//     AD3 down-left  AD4 down-right
#define MIC_UP_LEFT       0    // mic 1, AD1
#define MIC_UP_RIGHT      1    // mic 2, AD2
#define MIC_DOWN_LEFT     2    // mic 3, AD3
#define MIC_DOWN_RIGHT    3    // mic 4, AD4

typedef enum {
	SIDE_LEFT   = -1,
	SIDE_CENTRE =  0,          // straight up or down: left/right can't tell
	SIDE_RIGHT  =  1,
} side_t;

typedef struct {
	side_t  side;
	int8_t  first;             // index of the mic that heard it first, -1 if none
	uint8_t pairs;             // how many left/right pairs were measured: 0, 1 or 2
	bool    up_ok;             // up pair measured (both AD1 and AD2 heard it)
	bool    down_ok;           // down pair measured (both AD3 and AD4 heard it)
	bool    agree;             // both pairs measured and both point the same way
	int32_t up_ns;             // AD1 - AD2 (left minus right), valid if up_ok
	int32_t down_ns;           // AD3 - AD4 (left minus right), valid if down_ok
	int32_t dt_ns;             // mean of the measured pairs, valid if pairs > 0.
	                           // > 0: left mics heard it later, so the sound is on the RIGHT
} side_result_t;

// arrival_ns[m]: when mic m first crossed the threshold, or < 0 if it never did.
// centre_ns:     |dt| at or below this counts as CENTRE.
// max_delay_ns:  a mic arriving later than this after the first mic is treated as
//                an echo and ignored (the direct sound can't take longer than the
//                tray diagonal divided by the speed of sound).
side_result_t side_decide(const int32_t arrival_ns[SIDE_NUM_MICS],
		int32_t centre_ns, int32_t max_delay_ns);

const char *side_name(side_t side);

#endif // SIDE_H
