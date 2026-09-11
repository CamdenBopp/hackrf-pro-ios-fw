/*
 * Copyright 2026 Camden Bopp
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

/*
 * CDC-ECM (USB ethernet gadget) interface for talking to an iPhone.
 *
 * This is a second USB personality alongside the vendor-specific HackRF
 * transceiver interface (IF0). A USB host that speaks CDC-ECM (iOS, macOS)
 * binds it as a network device; the vendor interface is untouched so desktop
 * tools keep working. Data path is stubbed for now: the link comes up so the
 * host creates a network interface, but no IQ is carried yet.
 */

#ifndef __USB_API_CDC_H__
#define __USB_API_CDC_H__

#include <usb_type.h>
#include <usb_request.h>
#include <stdbool.h>
#include <stdint.h>

/* Class-request handler (bmRequestType type == class), wired into
 * usb_request_handlers.class. Answers the CDC-ECM management requests. */
usb_request_status_t usb_cdc_request(
	usb_endpoint_t* const endpoint,
	const usb_transfer_stage_t stage);

/* Called on USB (re)configuration to reset CDC state. */
void usb_cdc_init(void);

/* Registered as the SET_INTERFACE callback. When the host selects alt-setting 1
 * on the CDC data interface it starts the data path and reports link-up. */
void usb_cdc_set_interface(
	usb_device_t* const device,
	const uint8_t interface,
	const uint8_t alt);

/* Toggle vendor request (host-driven): wValue 0 = ethernet off (vendor-only
 * descriptor), 1 = ethernet on (composite). A changed mode triggers a deferred
 * self-reconnect so the host re-enumerates with the new interface set. The
 * default on every power-up is ethernet ON, so a mute host (iPhone) always gets
 * it; the desktop sends 0 to opt out and restore plain libhackrf access. */
usb_request_status_t usb_vendor_request_set_ethernet_mode(
	usb_endpoint_t* const endpoint,
	const usb_transfer_stage_t stage);

/* Call from main() BEFORE usb_run(): apply the persisted (or default-on) ethernet
 * mode to the config descriptor so the first enumeration reflects it. */
void usb_cdc_boot_init(void);

/* Diagnostics: returns 4 LE uint32 counters {rx_frames, arp_seen, icmp_seen, tx_sent}. */
usb_request_status_t usb_vendor_request_cdc_debug(
	usb_endpoint_t* const endpoint,
	const usb_transfer_stage_t stage);

/* --- Real IQ streaming over UDP (milestone 2) --- */

/* IQ bytes per UDP frame. One 16 KB bulk block must be a whole multiple of this
 * so it splits into complete frames with no ring-buffer wrap. */
#define CDC_IQ_PAYLOAD (1024)

/* True after START (and before STOP) on the command port. rx_mode checks this to
 * route samples to UDP instead of the vendor bulk endpoint 0x81. */
bool usb_cdc_iq_active(void);

/* Send one UDP IQ frame carrying len bytes from src to the host that sent START.
 * Spins until the CDC TX path is free; called from rx_mode (main context). */
void usb_cdc_send_iq(const uint8_t* src, uint32_t len);

#endif /* __USB_API_CDC_H__ */
