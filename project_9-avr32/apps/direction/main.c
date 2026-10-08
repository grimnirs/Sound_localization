/*
 * Direction, step 1: did a sharp sound come from the LEFT or the RIGHT of the tray?
 *
 * Builds on first_arrival: the same hardware-timed sampling (4 mics at 48 kHz) and
 * the same threshold detector for "when did the sound reach each mic". What's new:
 *   - side.c turns the four arrival times into LEFT / RIGHT / CENTRE, using the
 *     up pair (AD1 vs AD2) and the down pair (AD3 vs AD4) as two independent
 *     left-right measurements. Read the comment at the top of side.c.
 *   - Each event prints one machine-readable line, so a program on the Mac
 *     (host/direction_ui.py) can draw it. You can still read it in `screen`.
 *   - The LEDs show the side: right = LED1 + LED3 (the LEDs of AD2 and AD4),
 *     left = LED0 + LED2 (AD1 and AD3), centre = all four.
 *   - The hold-off after an event no longer blocks the CPU. The main loop is a
 *     small state machine (ARMED -> LISTENING -> HOLDOFF -> ARMED) that keeps
 *     reading samples the whole time, so it can report mic levels while the AGC
 *     recovers.
 *
 * Sampling chain, no CPU involved per sample (details in first_arrival/main.c):
 *   TC0 channel 0 --TIOA rising edge, 48 kHz--> ADC converts AD1..AD4 in a row
 *   --each result--> PDCA channel 0 --> ring buffer in RAM
 *
 * Serial protocol (USB CDC, one line each, "\r\n" endings). Lines starting with
 * '#' are for humans; the others start with a keyword:
 *   CFG fs=48000 rate=48000 thr=150 centre_us=100 max_us=1700 hold_ms=1500
 *       settings; rate = frames per second measured on the board
 *   BASE 381 382 383 381          DC level per mic (counts), mic 1..4
 *   ARMED                         listening for the next sound
 *   LVL 52 48 61 55               loudest |sample - baseline| per mic (counts)
 *                                 over the last 100 ms. The threshold is thr.
 *   EVT n=7 side=RIGHT dt=+812 up=+790 down=+834 pairs=2 agree=1 first=2
 *       m1=790 m2=0 m3=895 m4=61  (all on one line)
 *       dt, up, down: left minus right in us ("-" if not measured);
 *       first: mic that heard it first; m1..m4: each mic's delay after the first
 *       mic in us ("-" = never crossed; a value above max_us is an echo and was
 *       ignored). Mic n is ADn.
 *   After an EVT the board ignores sound for hold_ms, then prints ARMED again.
 *
 * Wiring (layout of 2026-10-08, see the pin table in CLAUDE.md). Mic n is on ADn,
 * seen from above with UP pointing away from you:
 *   mic1 OUT -> PA22 = AD1    up left
 *   mic2 OUT -> PA23 = AD2    up right
 *   mic3 OUT -> PA24 = AD3    down left
 *   mic4 OUT -> PA20 = AD4    down right   (AD4 uses GPIO function 2)
 */
#include <asf.h>
#include <stdio.h>
#include <stdlib.h>
#include "side.h"

#define NUM_MICS          SIDE_NUM_MICS

// ADC channel of each mic: mic N is at index N-1. Channels must match the pins below.
// side.h says which corner each mic is in.
static const uint8_t mic_adc_channel[NUM_MICS] = {1, 2, 3, 4};

static const gpio_map_t mic_adc_gpio_map = {
	{AVR32_ADC_AD_1_PIN, AVR32_ADC_AD_1_FUNCTION},
	{AVR32_ADC_AD_2_PIN, AVR32_ADC_AD_2_FUNCTION},
	{AVR32_ADC_AD_3_PIN, AVR32_ADC_AD_3_FUNCTION},
	{AVR32_ADC_AD_4_PIN, AVR32_ADC_AD_4_FUNCTION},
};

// LED n-1 belongs to mic n (as in first_arrival), so a side lights its two mics' LEDs.
#define LEDS_RIGHT        (LED1 | LED3)     // AD2 and AD4
#define LEDS_LEFT         (LED0 | LED2)     // AD1 and AD3
#define LEDS_ALL          (LED0 | LED1 | LED2 | LED3)

#define SAMPLE_RATE_HZ    48000     // per channel; one frame = 4 samples
#define FRAME_NS          (1000000000UL / SAMPLE_RATE_HZ)   // 20833 ns

// ADC timing: unchanged from first_arrival, see the comments there.
#define ADC_PRESCAL       4         // ADC clock = 48 MHz / 10 = 4.8 MHz
#define ADC_SHTIM         3
#define ADC_STARTUP       3
#define ADC_CONV_CLOCKS   (ADC_SHTIM + 1 + 10)  // estimate; measure it (CLAUDE.md task 8)

#define PDCA_CH_ADC       0
#define RING_FRAMES       1024      // 1024 frames * 4 ch * 2 B = 8 KB = 21 ms of sound
#define RING_SAMPLES      (RING_FRAMES * NUM_MICS)

// Detection settings.
#define THRESHOLD_COUNTS  150       // |sample - baseline| that counts as "sound arrived"
#define BASELINE_SHIFT    11        // baseline follows the DC level over 2^11 frames (~43 ms)
#define LISTEN_FRAMES     (2 * SAMPLE_RATE_HZ / 1000)   // 2 ms after the first crossing
#define MAX_DELAY_US      1700      // tray diagonal / speed of sound; later = echo
#define CENTRE_US         100       // |dt| up to this is CENTRE: ~5 samples, 3.4 cm of
                                    // path difference, a few degrees off straight ahead
#define HOLDOFF_MS        1500      // the AGC needs 1-1.5 s to recover after a clap
                                    // (CLAUDE.md); a clap before that is detected late
#define HOLDOFF_FRAMES    ((uint32_t)HOLDOFF_MS * (SAMPLE_RATE_HZ / 1000))
#define LEVEL_FRAMES      (SAMPLE_RATE_HZ / 10)         // one LVL line every 100 ms
#define LEVEL_LINE_MAX    32        // longest LVL line in bytes, see print_levels()

// The PDCA writes here. Each frame holds one sample per mic in ADC conversion
// order (lowest channel first). With mic n on ADn that is also mic order, but
// mic_slot[] keeps it right if the wiring changes again.
static volatile uint16_t ring[RING_SAMPLES];
static uint8_t mic_slot[NUM_MICS];  // position of mic m's sample within a frame
static uint32_t conv_ns;            // time between two neighbouring slots in a frame

static volatile bool terminal_open = false;

// Called from the USB stack when the host raises or drops DTR (see conf_usb.h).
void direction_set_dtr(bool set)
{
	terminal_open = set;
}

// ---------------------------------------------------------------------------
// Sampling: identical to first_arrival.
// ---------------------------------------------------------------------------

// Endless ring: when the PDCA finishes the buffer it switches to the "next buffer"
// (the same ring again) and raises this interrupt, which queues the ring once more.
ISR(pdca_ring_handler, AVR32_PDCA_IRQ_0, 1)
{
	pdca_reload_channel(PDCA_CH_ADC, (void *)ring, RING_SAMPLES);
}

static void sampling_init(void)
{
	const uint32_t pba_hz = sysclk_get_pba_hz();

	// conf_clock.h keeps peripheral clocks off after sysclk_init(); enable each one.
	sysclk_enable_pba_module(SYSCLK_ADC);
	sysclk_enable_pba_module(SYSCLK_TC0);
	sysclk_enable_hsb_module(SYSCLK_PDCA_HSB);
	sysclk_enable_pba_module(SYSCLK_PDCA_PB);

	// The ADC converts enabled channels lowest first, so a mic's slot in the frame
	// is the number of mics on a lower channel.
	for (uint8_t m = 0; m < NUM_MICS; m++) {
		mic_slot[m] = 0;
		for (uint8_t other = 0; other < NUM_MICS; other++) {
			if (mic_adc_channel[other] < mic_adc_channel[m]) {
				mic_slot[m]++;
			}
		}
	}

	// ADC: triggered by TC0 TIOA (TRGSEL 0). Don't use adc_configure(): it forces
	// SHTIM 15 and STARTUP 31, far too slow for 4 channels at 48 kHz.
	gpio_enable_module(mic_adc_gpio_map, NUM_MICS);
	AVR32_ADC.mr = (ADC_PRESCAL << AVR32_ADC_MR_PRESCAL_OFFSET)
			| (ADC_STARTUP << AVR32_ADC_MR_STARTUP_OFFSET)
			| (ADC_SHTIM << AVR32_ADC_MR_SHTIM_OFFSET)
			| (0 << AVR32_ADC_MR_TRGSEL_OFFSET)
			| AVR32_ADC_MR_TRGEN_MASK;
	for (uint8_t m = 0; m < NUM_MICS; m++) {
		adc_enable(&AVR32_ADC, mic_adc_channel[m]);
	}
	const uint32_t adc_hz = pba_hz / ((ADC_PRESCAL + 1) * 2);
	conv_ns = (uint32_t)((uint64_t)ADC_CONV_CLOCKS * 1000000000UL / adc_hz);

	// PDCA: copies each result into the ring. Read LCDR first so a stale "data
	// ready" can't shift every frame by one slot (which would swap the mics).
	(void)AVR32_ADC.lcdr;
	const pdca_channel_options_t pdca_opt = {
		.addr = (void *)ring,
		.size = RING_SAMPLES,
		.r_addr = (void *)ring,
		.r_size = RING_SAMPLES,
		.pid = AVR32_PDCA_PID_ADC_RX,
		.transfer_size = PDCA_TRANSFER_SIZE_HALF_WORD,
	};
	pdca_init_channel(PDCA_CH_ADC, &pdca_opt);
	irq_register_handler(pdca_ring_handler, AVR32_PDCA_IRQ_0, 1);
	pdca_enable_interrupt_reload_counter_zero(PDCA_CH_ADC);
	pdca_enable(PDCA_CH_ADC);

	// TC0 channel 0: PBA/8 = 6 MHz ticks, period RC = 125 ticks = 48 kHz. TIOA
	// rises once per period and starts the ADC. Started last, once all is ready.
	const tc_waveform_opt_t tc_opt = {
		.channel  = 0,
		.wavsel   = TC_WAVEFORM_SEL_UP_MODE_RC_TRIGGER,
		.tcclks   = TC_CLOCK_SOURCE_TC3,   // PBA / 8
		.acpa     = TC_EVT_EFFECT_CLEAR,
		.acpc     = TC_EVT_EFFECT_SET,
	};
	const uint16_t rc = pba_hz / 8 / SAMPLE_RATE_HZ;
	tc_init_waveform(&AVR32_TC0, &tc_opt);
	tc_write_rc(&AVR32_TC0, 0, rc);
	tc_write_ra(&AVR32_TC0, 0, rc / 2);
	tc_start(&AVR32_TC0, 0);
}

// Index of the next frame the PDCA will write. Frames before it are complete.
static uint32_t ring_write_frame(void)
{
	uint32_t mar = pdca_get_handler(PDCA_CH_ADC)->mar;
	uint32_t sample = (mar - (uint32_t)ring) / sizeof(ring[0]);
	return (sample / NUM_MICS) % RING_FRAMES;
}

// Frames written during one second, to check the chain really runs at 48 kHz.
static uint32_t measure_frame_rate(uint32_t cpu_hz)
{
	uint32_t start = ring_write_frame();
	uint32_t prev = start;
	uint32_t wraps = 0;
	uint32_t end_count = Get_sys_count() + cpu_hz;
	while ((int32_t)(Get_sys_count() - end_count) < 0) {
		uint32_t now = ring_write_frame();
		if (now < prev) {
			wraps++;
		}
		prev = now;
	}
	return wraps * RING_FRAMES + prev - start;
}

// ---------------------------------------------------------------------------
// Detection: a small state machine, fed one frame at a time.
// ---------------------------------------------------------------------------

typedef enum {
	STATE_ARMED,        // waiting for any mic to cross the threshold
	STATE_LISTENING,    // first crossing seen; collect the others for LISTEN_FRAMES
	STATE_HOLDOFF,      // event reported; ignore echoes while the AGC recovers
} state_t;

static state_t state = STATE_ARMED;
static uint32_t state_frames;               // frames spent in the current state
static int32_t baseline[NUM_MICS];          // DC level per mic, counts * 256 (fixed point)
static int32_t peak[NUM_MICS];              // largest |sample - baseline| since the last
                                            // LVL line, counts * 256
static uint32_t level_frames;               // frames since the last LVL line
static int32_t arrival_ns[NUM_MICS];        // first crossing per mic, -1 = not yet
static uint32_t event_count;

// Start each baseline at that mic's average over the last 256 frames.
static void init_baselines(uint32_t newest_frame)
{
	for (uint8_t m = 0; m < NUM_MICS; m++) {
		uint32_t sum = 0;
		for (uint32_t i = 1; i <= 256; i++) {
			uint32_t f = (newest_frame + RING_FRAMES - i) % RING_FRAMES;
			sum += ring[f * NUM_MICS + mic_slot[m]];
		}
		baseline[m] = sum;           // sum of 256 samples = mean * 256
		peak[m] = 0;
	}
}

static long ns_to_us(int32_t ns)
{
	return (ns >= 0 ? ns + 500 : ns - 500) / 1000;   // round to nearest
}

// Prints " key=+123" (signed) or " key=-" when there is no value.
static void print_field(const char *key, bool valid, int32_t ns)
{
	if (valid) {
		printf(" %s=%+ld", key, ns_to_us(ns));
	} else {
		printf(" %s=-", key);
	}
}

static void report_event(void)
{
	const side_result_t r = side_decide(arrival_ns,
			(int32_t)CENTRE_US * 1000, (int32_t)MAX_DELAY_US * 1000);
	event_count++;

	// LEDs first: they work without a terminal.
	LED_Off(LEDS_ALL);
	LED_On(r.side == SIDE_RIGHT ? LEDS_RIGHT : r.side == SIDE_LEFT ? LEDS_LEFT : LEDS_ALL);

	if (!terminal_open) {
		return;
	}
	printf("EVT n=%lu side=%s", (unsigned long)event_count, side_name(r.side));
	print_field("dt", r.pairs > 0, r.dt_ns);
	print_field("up", r.up_ok, r.up_ns);
	print_field("down", r.down_ok, r.down_ns);
	printf(" pairs=%u agree=%u first=%d", r.pairs, r.agree ? 1 : 0, r.first + 1);
	for (uint8_t m = 0; m < NUM_MICS; m++) {
		if (arrival_ns[m] < 0) {
			printf(" m%u=-", m + 1);
		} else {
			printf(" m%u=%ld", m + 1, ns_to_us(arrival_ns[m] - arrival_ns[r.first]));
		}
	}
	printf("\r\n");
}

// Mic levels for the UI's meters. Skipped (not queued) when the USB transmit
// buffer is short of room: a level line is never worth stalling the sampling loop,
// because printf waits for buffer space and the ring overwrites itself after 21 ms.
static void print_levels(void)
{
	if (terminal_open && udi_cdc_get_free_tx_buffer() >= LEVEL_LINE_MAX) {
		printf("LVL %ld %ld %ld %ld\r\n", (long)(peak[0] >> 8), (long)(peak[1] >> 8),
				(long)(peak[2] >> 8), (long)(peak[3] >> 8));
	}
	for (uint8_t m = 0; m < NUM_MICS; m++) {
		peak[m] = 0;
	}
}

static void process_frame(const volatile uint16_t *frame)
{
	bool any_crossed = false;
	bool crossed[NUM_MICS];

	for (uint8_t m = 0; m < NUM_MICS; m++) {
		int32_t dev = ((int32_t)frame[mic_slot[m]] << 8) - baseline[m];
		int32_t mag = abs(dev);
		crossed[m] = mag > (THRESHOLD_COUNTS << 8);
		any_crossed |= crossed[m];
		if (mag > peak[m]) {
			peak[m] = mag;
		}
		// Follow the slow DC drift only while armed, so a sound and its echoes
		// don't pull the baseline.
		if (state == STATE_ARMED) {
			baseline[m] += dev >> BASELINE_SHIFT;
		}
	}

	switch (state) {
	case STATE_ARMED:
		if (!any_crossed) {
			break;
		}
		state = STATE_LISTENING;
		state_frames = 0;
		for (uint8_t m = 0; m < NUM_MICS; m++) {
			arrival_ns[m] = -1;
		}
		// fall through: this frame already holds the first crossing
	case STATE_LISTENING:
		// A sample's real time is its frame time plus its slot offset: within a
		// frame the channels are converted one after another, conv_ns apart.
		for (uint8_t m = 0; m < NUM_MICS; m++) {
			if (crossed[m] && arrival_ns[m] < 0) {
				arrival_ns[m] = state_frames * FRAME_NS + mic_slot[m] * conv_ns;
			}
		}
		if (++state_frames >= LISTEN_FRAMES) {
			report_event();
			state = STATE_HOLDOFF;
			state_frames = 0;
		}
		break;
	case STATE_HOLDOFF:
		if (++state_frames >= HOLDOFF_FRAMES) {
			state = STATE_ARMED;
			if (terminal_open) {
				printf("ARMED\r\n");
			}
		}
		break;
	}

	// Levels every 100 ms, but not in the middle of the 2 ms listening window.
	if (++level_frames >= LEVEL_FRAMES && state != STATE_LISTENING) {
		level_frames = 0;
		print_levels();
	}
}

static void print_header(uint32_t cpu_hz)
{
	const uint32_t rate = measure_frame_rate(cpu_hz);
	printf("\r\n# Project_9 direction: LEFT / RIGHT from 4 mics\r\n");
	printf("# CPU %lu Hz, PBA %lu Hz, %lu ns between ADC channels\r\n",
			(unsigned long)cpu_hz, (unsigned long)sysclk_get_pba_hz(),
			(unsigned long)conv_ns);
	printf("# Snap or clap ~1 m to the left or right of the tray; wait for ARMED.\r\n");
	printf("CFG fs=%lu rate=%lu thr=%d centre_us=%d max_us=%d hold_ms=%d\r\n",
			(unsigned long)SAMPLE_RATE_HZ, (unsigned long)rate, THRESHOLD_COUNTS,
			CENTRE_US, MAX_DELAY_US, HOLDOFF_MS);
	printf("BASE %ld %ld %ld %ld\r\n", (long)(baseline[0] >> 8), (long)(baseline[1] >> 8),
			(long)(baseline[2] >> 8), (long)(baseline[3] >> 8));
	printf("ARMED\r\n");
}

int main(void)
{
	sysclk_init();
	board_init();

	irq_initialize_vectors();
	cpu_irq_enable();

	stdio_usb_init();
	sampling_init();

	const uint32_t cpu_hz = sysclk_get_cpu_hz();

	// Let the ring fill (all LEDs on meanwhile), then start the baselines.
	LED_On(LEDS_ALL);
	cpu_delay_ms(50, cpu_hz);
	LED_Off(LEDS_ALL);

	uint32_t read_frame = ring_write_frame();
	init_baselines(read_frame);

	bool header_printed = false;

	while (true) {
		// A terminal just opened: say hello. Only while armed, because the frame
		// rate check below blocks for a second and must not cut an event short.
		if (!terminal_open) {
			header_printed = false;
		} else if (!header_printed && state == STATE_ARMED) {
			print_header(cpu_hz);
			header_printed = true;
			read_frame = ring_write_frame();   // the ring lapped while we measured
		}

		// Process every complete frame the PDCA has written since last time.
		const uint32_t write_frame = ring_write_frame();
		while (read_frame != write_frame) {
			process_frame(&ring[read_frame * NUM_MICS]);
			read_frame = (read_frame + 1) % RING_FRAMES;
		}
	}
}
