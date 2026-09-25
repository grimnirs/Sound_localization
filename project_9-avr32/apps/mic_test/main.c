/*
 * Microphone test: reads four MAX9814s on the ADC and prints their levels over USB.
 *
 * Samples four ADC channels at a fixed rate, and every window prints the mean and
 * peak-to-peak value of each mic as one line of text on the board's USB serial
 * port. Silence shows a steady mean (the MAX9814 output idles at about 1.25 V,
 * roughly 388 counts) with a small peak-to-peak; sound makes the peak-to-peak and
 * that mic's bar jump. Tap each mic in turn to check the channel->mic mapping.
 *
 * Wiring (see the pin table in CLAUDE.md):
 *   mic1 OUT -> PA20 = ADC channel 4 (AD0/PA21 is not used: it picked up a steady
 *                                     noise signal whichever mic was connected)
 *   mic2 OUT -> PA22 = ADC channel 1 (J2 pin 2)
 *   mic3 OUT -> PA23 = ADC channel 2
 *   mic4 OUT -> PA24 = ADC channel 3
 * An input with nothing connected floats, so its numbers are meaningless.
 */
#include <asf.h>
#include <stdio.h>

#define NUM_MICS          4

// ADC channel of each mic: mic N is at index N-1. Channels must match the pins below.
static const uint8_t mic_adc_channel[NUM_MICS] = {4, 1, 2, 3};

static const gpio_map_t mic_adc_gpio_map = {
	{AVR32_ADC_AD_4_PIN, AVR32_ADC_AD_4_FUNCTION},
	{AVR32_ADC_AD_1_PIN, AVR32_ADC_AD_1_FUNCTION},
	{AVR32_ADC_AD_2_PIN, AVR32_ADC_AD_2_FUNCTION},
	{AVR32_ADC_AD_3_PIN, AVR32_ADC_AD_3_FUNCTION},
};

#define SAMPLE_RATE_HZ    8000
#define WINDOW_SAMPLES    400       // 400 samples at 8 kHz = one line every 50 ms

#define BAR_MAX_WIDTH     10

static volatile bool terminal_open = false;

// Called from the USB stack when the host raises or drops DTR (see conf_usb.h).
void mic_test_set_dtr(bool set)
{
	terminal_open = set;
}

static void mic_adc_init(void)
{
	gpio_enable_module(mic_adc_gpio_map, NUM_MICS);

	// conf_clock.h keeps only a minimal set of peripheral clocks running after
	// sysclk_init(), so the ADC's clock must be turned on explicitly. Without it
	// a conversion never finishes and adc_get_value() waits forever.
	sysclk_enable_pba_module(SYSCLK_ADC);

	// ADC clock = PBA / ((PRESCAL + 1) * 2) = 12 MHz / 4 = 3 MHz, within the
	// ADC's limit (same setting as ASF's ADC example).
	AVR32_ADC.mr |= 0x1 << AVR32_ADC_MR_PRESCAL_OFFSET;
	adc_configure(&AVR32_ADC);
	for (uint8_t m = 0; m < NUM_MICS; m++) {
		adc_enable(&AVR32_ADC, mic_adc_channel[m]);
	}
}

// Takes one sample from every mic. There is only one converter: a single start
// converts all enabled channels one after another, lowest channel first, so the
// four samples are a few microseconds apart rather than simultaneous. Mic 1 is
// on channel 4, so it is now converted last, after mics 2-4. That skew
// doesn't matter for a level meter but must be corrected for TDoA later.
static void mic_adc_read_all(uint16_t v[NUM_MICS])
{
	adc_start(&AVR32_ADC);
	for (uint8_t m = 0; m < NUM_MICS; m++) {
		v[m] = adc_get_value(&AVR32_ADC, mic_adc_channel[m]);
	}
}

int main(void)
{
	sysclk_init();
	board_init();

	irq_initialize_vectors();
	cpu_irq_enable();

	stdio_usb_init();
	mic_adc_init();

	const uint32_t cpu_hz = sysclk_get_cpu_hz();
	const uint32_t cycles_per_sample = cpu_hz / SAMPLE_RATE_HZ;
	bool header_printed = false;

	while (true) {
		uint32_t sum[NUM_MICS];
		uint16_t min[NUM_MICS];
		uint16_t max[NUM_MICS];
		for (uint8_t m = 0; m < NUM_MICS; m++) {
			sum[m] = 0;
			min[m] = ADC_MAX_VALUE;
			max[m] = 0;
		}

		// Pace samples off the CPU cycle counter so the rate stays fixed.
		// At 12 MHz one sample period is 1500 cycles (125 us); converting all
		// four channels takes about 35 us of that.
		uint32_t next = Get_sys_count();
		for (uint16_t i = 0; i < WINDOW_SAMPLES; i++) {
			while ((int32_t)(Get_sys_count() - next) < 0) {
			}
			next += cycles_per_sample;

			uint16_t v[NUM_MICS];
			mic_adc_read_all(v);
			for (uint8_t m = 0; m < NUM_MICS; m++) {
				sum[m] += v[m];
				if (v[m] < min[m]) {
					min[m] = v[m];
				}
				if (v[m] > max[m]) {
					max[m] = v[m];
				}
			}
		}

		// Heartbeat: LED0 toggles every window, whether or not anyone listens.
		LED_Toggle(LED0);

		if (!terminal_open) {
			header_printed = false;
			continue;
		}
		if (!header_printed) {
			printf("\r\nProject_9 mic test: %d Hz, %d samples/line, ADC channel of mic 1-%d:",
					SAMPLE_RATE_HZ, WINDOW_SAMPLES, NUM_MICS);
			for (uint8_t m = 0; m < NUM_MICS; m++) {
				printf(" %u", mic_adc_channel[m]);
			}
			printf("\r\n");
			printf("Per mic: N: mean p2p |level| (ADC counts 0-%d, idle mean ~388)\r\n",
					ADC_MAX_VALUE);
			header_printed = true;
		}

		for (uint8_t m = 0; m < NUM_MICS; m++) {
			uint16_t mean = sum[m] / WINDOW_SAMPLES;
			uint16_t p2p = max[m] - min[m];
			uint16_t bar = (uint32_t)p2p * BAR_MAX_WIDTH / ADC_MAX_VALUE;

			printf("%u:%4u %4u |", m + 1, mean, p2p);
			for (uint16_t i = 0; i < BAR_MAX_WIDTH; i++) {
				putchar(i < bar ? '#' : ' ');
			}
			printf("|  ");
		}
		printf("\r\n");
	}
}
