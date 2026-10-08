/**
 * \file
 *
 * \brief Chip-specific system clock manager configuration
 *
 * Copyright (c) 2010-2018 Microchip Technology Inc. and its subsidiaries.
 *
 * \asf_license_start
 *
 * \page License
 *
 * Subject to your compliance with these terms, you may use Microchip
 * software and any derivatives exclusively with Microchip products.
 * It is your responsibility to comply with third party license terms applicable
 * to your use of third party software (including open source software) that
 * may accompany Microchip software.
 *
 * THIS SOFTWARE IS SUPPLIED BY MICROCHIP "AS IS". NO WARRANTIES,
 * WHETHER EXPRESS, IMPLIED OR STATUTORY, APPLY TO THIS SOFTWARE,
 * INCLUDING ANY IMPLIED WARRANTIES OF NON-INFRINGEMENT, MERCHANTABILITY,
 * AND FITNESS FOR A PARTICULAR PURPOSE. IN NO EVENT WILL MICROCHIP BE
 * LIABLE FOR ANY INDIRECT, SPECIAL, PUNITIVE, INCIDENTAL OR CONSEQUENTIAL
 * LOSS, DAMAGE, COST OR EXPENSE OF ANY KIND WHATSOEVER RELATED TO THE
 * SOFTWARE, HOWEVER CAUSED, EVEN IF MICROCHIP HAS BEEN ADVISED OF THE
 * POSSIBILITY OR THE DAMAGES ARE FORESEEABLE.  TO THE FULLEST EXTENT
 * ALLOWED BY LAW, MICROCHIP'S TOTAL LIABILITY ON ALL CLAIMS IN ANY WAY
 * RELATED TO THIS SOFTWARE WILL NOT EXCEED THE AMOUNT OF FEES, IF ANY,
 * THAT YOU HAVE PAID DIRECTLY TO MICROCHIP FOR THIS SOFTWARE.
 *
 * \asf_license_stop
 *
 */
/*
 * Support and FAQ: visit <a href="https://www.microchip.com/support/">Microchip Support</a>
 */
#ifndef CONF_CLOCK_H_INCLUDED
#define CONF_CLOCK_H_INCLUDED

// ===== Peripheral Clock Management Options

#define CONFIG_SYSCLK_INIT_CPUMASK   (0)
#define CONFIG_SYSCLK_INIT_PBAMASK   (0)
#define CONFIG_SYSCLK_INIT_PBBMASK   (0)
#define CONFIG_SYSCLK_INIT_HSBMASK   (0)

// Run the CPU and PBA from PLL0 at 48 MHz instead of straight from the 12 MHz
// crystal: the ADC clock and the sample timer both derive from PBA, and 12 MHz is
// too slow for 4 channels at 48 kHz. PLL0 = 12 MHz * 4 = 48 MHz (ASF runs the VCO
// at 96 MHz and divides by 2, since the VCO must stay above 80 MHz).
// sysclk_init() sets the flash wait state to match. PBA is undivided (48 MHz);
// the datasheet allows up to 66 MHz on CPU and PBA (verify in the PM chapter).
#define CONFIG_SYSCLK_SOURCE         (SYSCLK_SRC_PLL0)
#define CONFIG_SYSCLK_CPU_DIV        (0)
#define CONFIG_SYSCLK_PBA_DIV        (0)
#define CONFIG_SYSCLK_PBB_DIV        (0)
#define CONFIG_PLL0_SOURCE           (PLL_SRC_OSC0)
#define CONFIG_PLL0_MUL              (48000000UL / BOARD_OSC0_HZ)
#define CONFIG_PLL0_DIV              (1)
// USB keeps its own 12 MHz clock from OSC0, independent of the CPU clock.
#define CONFIG_USBCLK_SOURCE         (USBCLK_SRC_OSC0)
#define CONFIG_USBCLK_DIV            (1)
#define CONFIG_PLL1_SOURCE           (PLL_SRC_OSC0)
#define CONFIG_PLL1_DIV              (2)
#define CONFIG_PLL1_MUL              (8)

#endif /* CONF_CLOCK_H_INCLUDED */
