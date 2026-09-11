/*
 * Copyright 2026 Great Scott Gadgets <info@greatscottgadgets.com>
 *
 * This file is part of HackRF.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

#include "leds.h"

#include <stdint.h>

#include "delay.h"
#include "gpio.h"
#include "platform_detect.h"
#include "platform_gpio.h"

/*
 * LED customization: keep every firmware-controlled LED (led[0..3]) dark. The
 * bright blue/green LEDs on this praline board are hardwired to a power rail and
 * cannot be reached from firmware (the FPGA drives no LED, and these are not the
 * led[0..3] GPIOs), so this affects only the firmware LEDs. led_off() stays live
 * so the boot-time all-off in pins.c drives them off; led_on/led_toggle become
 * no-ops and set_leds forces off, so nothing can light one. Set LEDS_DISABLED to
 * 0 to restore stock LED behavior.
 */
#define LEDS_DISABLED 1

void led_on(const led_t led)
{
	if (LEDS_DISABLED) {
		return;
	}
#ifdef IS_PRALINE
	if (IS_PRALINE) {
		gpio_clear(platform_gpio()->led[led]);
	}
#endif
#ifdef IS_NOT_PRALINE
	if (IS_NOT_PRALINE) {
		gpio_set(platform_gpio()->led[led]);
	}
#endif
}

void led_off(const led_t led)
{
#ifdef IS_PRALINE
	if (IS_PRALINE) {
		gpio_set(platform_gpio()->led[led]);
	}
#endif
#ifdef IS_NOT_PRALINE
	if (IS_NOT_PRALINE) {
		gpio_clear(platform_gpio()->led[led]);
	}
#endif
}

void led_toggle(const led_t led)
{
	if (LEDS_DISABLED) {
		return;
	}
	gpio_toggle(platform_gpio()->led[led]);
}

void set_leds(const uint8_t state)
{
	const uint8_t effective = LEDS_DISABLED ? 0 : state;
	int num_leds = 3;
#ifdef IS_FOUR_LEDS
	if (IS_FOUR_LEDS) {
		num_leds = 4;
	}
#endif

	for (int i = 0; i < num_leds; i++) {
#ifdef IS_PRALINE
		if (IS_PRALINE) {
			gpio_write(platform_gpio()->led[i], ((effective >> i) & 1) == 0);
		}
#endif
#ifdef IS_NOT_PRALINE
		if (IS_NOT_PRALINE) {
			gpio_write(platform_gpio()->led[i], ((effective >> i) & 1) == 1);
		}
#endif
	}
}

void halt_and_flash(const uint32_t period_ms)
{
	/* blink LED1, LED2, and LED3 */
	while (1) {
		led_on(LED1);
		led_on(LED2);
		led_on(LED3);
		delay_ms(period_ms / 2);
		led_off(LED1);
		led_off(LED2);
		led_off(LED3);
		delay_ms(period_ms / 2);
	}
}
