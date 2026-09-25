/*
 * Microphone test: reads a MAX9814 on the ADC and prints its level over USB.
 *
 * Samples one ADC channel at a fixed rate, and every window prints the mean,
 * min, max and peak-to-peak value as a line of text on the board's USB serial
 * port. Silence shows a steady mean (the MAX9814 output idles at about 1.25 V)
 * with a small peak-to-peak; sound makes the peak-to-peak and the bar jump.
 *
 * Wiring: MAX9814 OUT -> header J2 pin 2 (PA22 = ADC channel 1).
 */
#include <asf.h>
#include <stdio.h>

// ADC input the microphone is wired to (J2 pin 2 on the UC3-A3 Xplained).
#define MIC_ADC_CHANNEL   1
#define MIC_ADC_PIN       AVR32_ADC_AD_1_PIN
#define MIC_ADC_FUNCTION  AVR32_ADC_AD_1_FUNCTION

#define SAMPLE_RATE_HZ    8000
#define WINDOW_SAMPLES    400       // 400 samples at 8 kHz = one line every 50 ms

// Assumed ADC reference voltage, only used to print millivolts.
#define ADC_REF_MV        3300

#define BAR_MAX_WIDTH     50

static volatile bool terminal_open = false;

// Called from the USB stack when the host raises or drops DTR (see conf_usb.h).
void mic_test_set_dtr(bool set)
{
	terminal_open = set;
}

static void mic_adc_init(void)
{
	static const gpio_map_t adc_gpio_map = {
		{MIC_ADC_PIN, MIC_ADC_FUNCTION}
	};
	gpio_enable_module(adc_gpio_map, 1);

	// conf_clock.h keeps only a minimal set of peripheral clocks running after
	// sysclk_init(), so the ADC's clock must be turned on explicitly. Without it
	// a conversion never finishes and adc_get_value() waits forever.
	sysclk_enable_pba_module(SYSCLK_ADC);

	// ADC clock = PBA / ((PRESCAL + 1) * 2) = 12 MHz / 4 = 3 MHz, within the
	// ADC's limit (same setting as ASF's ADC example).
	AVR32_ADC.mr |= 0x1 << AVR32_ADC_MR_PRESCAL_OFFSET;
	adc_configure(&AVR32_ADC);
	adc_enable(&AVR32_ADC, MIC_ADC_CHANNEL);
}

static uint16_t mic_adc_read(void)
{
	adc_start(&AVR32_ADC);
	return adc_get_value(&AVR32_ADC, MIC_ADC_CHANNEL);
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
		uint32_t sum = 0;
		uint16_t min = ADC_MAX_VALUE;
		uint16_t max = 0;

		// Pace samples off the CPU cycle counter so the rate stays fixed.
		uint32_t next = Get_sys_count();
		for (uint16_t i = 0; i < WINDOW_SAMPLES; i++) {
			while ((int32_t)(Get_sys_count() - next) < 0) {
			}
			next += cycles_per_sample;

			uint16_t v = mic_adc_read();
			sum += v;
			if (v < min) {
				min = v;
			}
			if (v > max) {
				max = v;
			}
		}

		// Heartbeat: LED0 toggles every window, whether or not anyone listens.
		LED_Toggle(LED0);

		if (!terminal_open) {
			header_printed = false;
			continue;
		}
		if (!header_printed) {
			printf("\r\nProject_9 mic test: ADC channel %d, %d Hz, %d samples/line\r\n",
					MIC_ADC_CHANNEL, SAMPLE_RATE_HZ, WINDOW_SAMPLES);
			printf("mean(counts/mV)   min   max   p2p\r\n");
			header_printed = true;
		}

		uint16_t mean = sum / WINDOW_SAMPLES;
		uint16_t p2p = max - min;
		uint16_t bar = (uint32_t)p2p * BAR_MAX_WIDTH / ADC_MAX_VALUE;

		printf("%4u / %4lu mV  %4u  %4u  %4u  |", mean,
				(unsigned long)mean * ADC_REF_MV / ADC_MAX_VALUE, min, max, p2p);
		for (uint16_t i = 0; i < bar; i++) {
			putchar('#');
		}
		printf("\r\n");
	}
}
