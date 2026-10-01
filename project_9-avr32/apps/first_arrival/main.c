/*
 * First arrival: which microphone heard a sharp sound first?
 *
 * The simplest direction finder we can build. All four mics are sampled at 48 kHz
 * each, hardware-timed. When a sound comes in, the mic nearest the source crosses
 * a fixed threshold first, and the others follow up to ~1.6 ms later (sound needs
 * that long to cross the tray diagonal). The nearest mic tells us which corner the
 * sound came from. Each event prints one line over USB with the delay of every mic
 * relative to the first, and lights the LED with the same number as that mic
 * (LED0 = mic 1 ... LED3 = mic 4).
 *
 * Sampling chain, no CPU involved per sample:
 *   TC0 channel 0 --TIOA rising edge, 48 kHz--> ADC converts AD1..AD4 in a row
 *   --each result--> PDCA channel 0 --> ring buffer in RAM
 * The main loop only reads the ring buffer behind the PDCA's write position.
 *
 * Limitation: threshold crossing depends on loudness. Each MAX9814 has its own AGC,
 * so the same sound reaches the threshold a little earlier on a mic with more gain.
 * The error is up to a fraction of the sound's waveform period. Use sounds with a
 * sharp onset (finger snap, clicker, two pens struck together) about 1 m away.
 * Don't tap the tray itself: sound travels faster through the tray than through
 * the air. GCC-PHAT (CLAUDE.md task 9) is the amplitude-independent fix.
 *
 * Wiring (see the pin table in CLAUDE.md):
 *   mic1 OUT -> PA20 = ADC channel 4
 *   mic2 OUT -> PA22 = ADC channel 1 (J2 pin 2)
 *   mic3 OUT -> PA23 = ADC channel 2
 *   mic4 OUT -> PA24 = ADC channel 3
 */
#include <asf.h>
#include <stdio.h>
#include <stdlib.h>

#define NUM_MICS          4

// ADC channel of each mic: mic N is at index N-1. Channels must match the pins below.
static const uint8_t mic_adc_channel[NUM_MICS] = {4, 1, 2, 3};

static const gpio_map_t mic_adc_gpio_map = {
	{AVR32_ADC_AD_4_PIN, AVR32_ADC_AD_4_FUNCTION},
	{AVR32_ADC_AD_1_PIN, AVR32_ADC_AD_1_FUNCTION},
	{AVR32_ADC_AD_2_PIN, AVR32_ADC_AD_2_FUNCTION},
	{AVR32_ADC_AD_3_PIN, AVR32_ADC_AD_3_FUNCTION},
};

// Which tray corner each mic sits in, printed with the result. Fill in once the
// corners are recorded (CLAUDE.md task 2), e.g. "front left".
static const char *const mic_corner[NUM_MICS] = {"?", "?", "?", "?"};

#define SAMPLE_RATE_HZ    48000     // per channel; one frame = 4 samples
#define FRAME_NS          (1000000000UL / SAMPLE_RATE_HZ)   // 20833 ns

// ADC timing. ADC clock = PBA / ((PRESCAL + 1) * 2) = 48 MHz / 10 = 4.8 MHz, just
// under the ~5 MHz limit (verify in the datasheet ADC chapter). Sample-and-hold
// lasts about (SHTIM + 1) ADC clocks; the MAX9814 drives its output with an op-amp,
// so its low output impedance charges the ADC's sampling capacitor quickly and
// ~0.8 us is plenty. ASF's adc_configure() would set SHTIM to 15 and STARTUP to 31,
// which makes every conversion slower, so we write the MR register ourselves.
#define ADC_PRESCAL       4
#define ADC_SHTIM         3
#define ADC_STARTUP       3         // only used when waking from sleep mode (SLEEP=0 here)
#define ADC_CONV_CLOCKS   (ADC_SHTIM + 1 + 10)  // estimate: S/H + 10 bit steps; measure it (task 8)

#define PDCA_CH_ADC       0
#define RING_FRAMES       1024      // 1024 frames * 4 ch * 2 B = 8 KB = 21 ms of sound
#define RING_SAMPLES      (RING_FRAMES * NUM_MICS)

// Detection settings.
#define THRESHOLD_COUNTS  150       // |sample - baseline| that counts as "sound arrived".
                                    // Quiet p2p is 60-110, i.e. about +-55 around the bias.
#define BASELINE_SHIFT    11        // baseline follows the DC level over 2^11 frames (~43 ms)
#define LISTEN_FRAMES     (2 * SAMPLE_RATE_HZ / 1000)   // 2 ms after the first crossing
#define MAX_DELAY_US      1700      // longest possible delay (0.57 m diagonal / 343 m/s);
                                    // anything later is an echo, not the direct sound
#define HOLDOFF_MS        500       // ignore echoes and let the AGC settle after an event

// The PDCA writes here. Each frame holds one sample per mic in ADC conversion
// order (lowest channel first), not in mic order; see mic_slot[].
static volatile uint16_t ring[RING_SAMPLES];

// mic_slot[m]: position of mic m's sample within a frame. The ADC converts the
// enabled channels lowest first, so with channels {4, 1, 2, 3} a frame is
// [AD1 = mic 2, AD2 = mic 3, AD3 = mic 4, AD4 = mic 1] and mic 1 is in slot 3.
static uint8_t mic_slot[NUM_MICS];

static uint32_t conv_ns;           // time between two neighbouring slots in a frame

static volatile bool terminal_open = false;

// Called from the USB stack when the host raises or drops DTR (see conf_usb.h).
void first_arrival_set_dtr(bool set)
{
	terminal_open = set;
}

// The PDCA has a "next buffer" register pair (MARR/TCRR). When the current buffer is
// full, the hardware switches to the next one without losing a sample and raises
// "reload counter zero". Pointing the next buffer at the same ring each time makes
// the ring endless. We have a whole lap (21 ms) to answer this interrupt.
ISR(pdca_ring_handler, AVR32_PDCA_IRQ_0, 1)
{
	pdca_reload_channel(PDCA_CH_ADC, (void *)ring, RING_SAMPLES);
}

static void sampling_init(void)
{
	const uint32_t pba_hz = sysclk_get_pba_hz();

	// conf_clock.h keeps peripheral clocks off after sysclk_init(), so every
	// peripheral we use must be clocked explicitly or it silently does nothing.
	// The PDCA sits on both the HSB (memory side) and the PBA (register side).
	sysclk_enable_pba_module(SYSCLK_ADC);
	sysclk_enable_pba_module(SYSCLK_TC0);
	sysclk_enable_hsb_module(SYSCLK_PDCA_HSB);
	sysclk_enable_pba_module(SYSCLK_PDCA_PB);

	for (uint8_t m = 0; m < NUM_MICS; m++) {
		mic_slot[m] = 0;
		for (uint8_t other = 0; other < NUM_MICS; other++) {
			if (mic_adc_channel[other] < mic_adc_channel[m]) {
				mic_slot[m]++;
			}
		}
	}

	// ADC: hardware trigger from TC0 channel 0's TIOA output (TRGSEL = 0, the same
	// setting as the course lab code; see the ADC trigger table in the datasheet).
	// One trigger converts every enabled channel once, one after another.
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

	// PDCA: every finished conversion raises the ADC's "data ready", and the PDCA
	// copies the result register into the next half-word of the ring.
	// Reading LCDR first clears any stale data ready. Otherwise the PDCA would copy
	// one old value at start-up, every frame would be shifted by one slot, and the
	// mics would silently swap places.
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

	// TC0 channel 0 counts PBA/8 = 6 MHz ticks from 0 up to RC and restarts, so the
	// period is RC ticks: 6 MHz / 48 kHz = 125. TIOA is cleared at RA and set at RC,
	// so it rises exactly once per period and that edge starts the ADC. The timer
	// starts last, once the ADC and PDCA are ready for the first trigger.
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
// MAR is the PDCA's write address, so it tells us how far it has got.
static uint32_t ring_write_frame(void)
{
	uint32_t mar = pdca_get_handler(PDCA_CH_ADC)->mar;
	uint32_t sample = (mar - (uint32_t)ring) / sizeof(ring[0]);
	return (sample / NUM_MICS) % RING_FRAMES;
}

// Counts frames written during one second, to check the whole chain really runs at
// SAMPLE_RATE_HZ. The ring wraps every 21 ms, so we poll and count the wraps.
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

int main(void)
{
	sysclk_init();
	board_init();

	irq_initialize_vectors();
	cpu_irq_enable();

	stdio_usb_init();
	sampling_init();

	const uint32_t cpu_hz = sysclk_get_cpu_hz();

	// Let the ring fill, then start each baseline at that mic's average level so
	// we don't trigger on the first few samples.
	LED_On(LED0 | LED1 | LED2 | LED3);
	cpu_delay_ms(50, cpu_hz);
	LED_Off(LED0 | LED1 | LED2 | LED3);

	uint32_t read_frame = ring_write_frame();
	int32_t baseline[NUM_MICS];      // DC level per mic, in counts * 256 (fixed point)
	for (uint8_t m = 0; m < NUM_MICS; m++) {
		uint32_t sum = 0;
		for (uint32_t i = 1; i <= 256; i++) {
			uint32_t f = (read_frame + RING_FRAMES - i) % RING_FRAMES;
			sum += ring[f * NUM_MICS + mic_slot[m]];
		}
		baseline[m] = sum;           // sum of 256 samples = mean * 256
	}

	bool header_printed = false;
	bool listening = false;          // inside the 2 ms window after a first crossing
	uint32_t listen_frame = 0;       // frames since the first crossing
	int32_t arrival_ns[NUM_MICS];    // first crossing per mic, -1 = not yet
	uint32_t event_count = 0;

	while (true) {
		if (!terminal_open) {
			header_printed = false;
		} else if (!header_printed && !listening) {
			uint32_t rate = measure_frame_rate(cpu_hz);
			printf("\r\nProject_9 first arrival: %lu Hz per mic (measured %lu frames/s)\r\n",
					(unsigned long)SAMPLE_RATE_HZ, (unsigned long)rate);
			printf("CPU %lu Hz, PBA %lu Hz, %lu ns between channels, threshold %d counts\r\n",
					(unsigned long)cpu_hz, (unsigned long)sysclk_get_pba_hz(),
					(unsigned long)conv_ns, THRESHOLD_COUNTS);
			printf("Baseline per mic:");
			for (uint8_t m = 0; m < NUM_MICS; m++) {
				printf(" %ld", (long)(baseline[m] >> 8));
			}
			printf("\r\nSnap your fingers ~1 m away. Delays in us after the first mic.\r\n");
			header_printed = true;
			read_frame = ring_write_frame();   // skip what arrived while printing
		}

		uint32_t write_frame = ring_write_frame();
		while (read_frame != write_frame) {
			const volatile uint16_t *frame = &ring[read_frame * NUM_MICS];
			read_frame = (read_frame + 1) % RING_FRAMES;

			bool crossed[NUM_MICS];
			bool any_crossed = false;
			for (uint8_t m = 0; m < NUM_MICS; m++) {
				int32_t dev = ((int32_t)frame[mic_slot[m]] << 8) - baseline[m];
				crossed[m] = abs(dev) > (THRESHOLD_COUNTS << 8);
				any_crossed |= crossed[m];
				// Track the slow DC drift only between events, so the sound itself
				// doesn't pull the baseline.
				if (!listening) {
					baseline[m] += dev >> BASELINE_SHIFT;
				}
			}

			if (!listening) {
				if (!any_crossed) {
					continue;
				}
				listening = true;
				listen_frame = 0;
				for (uint8_t m = 0; m < NUM_MICS; m++) {
					arrival_ns[m] = -1;
				}
			}

			// A sample's real time is its frame time plus its slot offset: within a
			// frame the channels are converted one after another, conv_ns apart.
			for (uint8_t m = 0; m < NUM_MICS; m++) {
				if (crossed[m] && arrival_ns[m] < 0) {
					arrival_ns[m] = listen_frame * FRAME_NS + mic_slot[m] * conv_ns;
				}
			}

			listen_frame++;
			if (listen_frame >= LISTEN_FRAMES) {
				break;
			}
		}

		if (!listening || listen_frame < LISTEN_FRAMES) {
			continue;
		}

		// Event complete: the earliest crossing is the mic nearest the sound.
		listening = false;
		event_count++;
		uint8_t first = 0;
		for (uint8_t m = 1; m < NUM_MICS; m++) {
			if (arrival_ns[m] >= 0
					&& (arrival_ns[first] < 0 || arrival_ns[m] < arrival_ns[first])) {
				first = m;
			}
		}
		LED_Off(LED0 | LED1 | LED2 | LED3);
		LED_On(LED0 << first);

		if (terminal_open) {
			printf("#%lu first: mic %u (%s) | delay us:", (unsigned long)event_count,
					first + 1, mic_corner[first]);
			for (uint8_t m = 0; m < NUM_MICS; m++) {
				if (arrival_ns[m] < 0) {
					// Never crossed: too quiet on this mic, or the threshold is too high.
					printf("  m%u ---!", m + 1);
					continue;
				}
				int32_t delay_us = (arrival_ns[m] - arrival_ns[first]) / 1000;
				printf("  m%u %+5ld%s", m + 1, (long)delay_us,
						delay_us > MAX_DELAY_US ? "!" : "");
			}
			printf("\r\n");
		}

		// The PDCA keeps filling the ring meanwhile; after the hold-off we skip
		// to the newest data instead of processing the echoes we missed.
		cpu_delay_ms(HOLDOFF_MS, cpu_hz);
		read_frame = ring_write_frame();
	}
}
