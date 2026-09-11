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
 * See usb_api_cdc.h. Link-up only for now; the ethernet data path is stubbed
 * (received frames are drained and discarded, nothing is transmitted).
 */

#include "usb_api_cdc.h"
#include "usb_endpoint.h"
#include "usb_descriptor.h"
#include "usb_api_transceiver.h"

#include <libopencm3/lpc43xx/wwdt.h>
#include <usb_queue.h>
#include <transceiver_mode.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Defined in usb_api_transceiver.c; starts/stops the RX pipeline. */
extern void request_transceiver_mode(transceiver_mode_t mode);

#define CDC_COMM_INTERFACE (1)
#define CDC_DATA_INTERFACE (2)
#define CDC_DATA_ALT_ON    (1)
#define ETH_FRAME_MAX      (1536)

/* Scratch for class-request OUT/IN data we accept but don't act on yet. */
static uint8_t cdc_scratch[128];
/* Receive buffer for host -> device ethernet frames (discarded for now). */
static uint8_t cdc_rx_frame[ETH_FRAME_MAX];
/* CDC NETWORK_CONNECTION notification packet. */
static uint8_t cdc_notify_buf[8];

/* IQ transmit buffer (device -> host), filled from main context by usb_cdc_send_iq. */
static uint8_t cdc_tx_frame[ETH_FRAME_MAX];
static volatile bool cdc_tx_busy = false;

/* Control-reply transmit buffer (ARP / ICMP / DHCP / PONG), filled from the USB
 * ISR by the cdc_rx_complete handlers. Kept separate from cdc_tx_frame so a
 * reply raised while usb_cdc_send_iq is mid-fill cannot corrupt an IQ frame, and
 * so ARP keeps being answered during streaming: the host's ARP entry for us
 * expires mid-session otherwise and its FREQ / STOP datagrams stop arriving.
 * usb_queue serialises the two contexts' transfers on the bulk IN endpoint. */
static uint8_t cdc_ctl_frame[ETH_FRAME_MAX];
static volatile bool cdc_ctl_busy = false;

static volatile bool cdc_rx_armed = false;

/* The gadget's fixed link-local identity. The descriptor's iMACAddress
 * (02:12:34:56:78:9a) is the MAC the HOST assigns to its end of the link, so the
 * device MUST use a DIFFERENT MAC here or the host drops our frames as its own and
 * ARP never resolves. The IP is static link-local so a host that self-assigns a
 * 169.254.x address on this link reaches us with no DHCP. */
static const uint8_t our_mac[6] = {0x02, 0x12, 0x34, 0x56, 0x78, 0x9b};
/* Private /24 the radio owns. The host gets .2 via DHCP so the route to us is
 * unambiguous (only this interface has 10.55.0.x), unlike link-local 169.254
 * which collides with WiFi on a multi-interface host. */
static const uint8_t our_ip[4] = {10, 55, 0, 1};
static const uint8_t offer_ip[4] = {10, 55, 0, 2};

/* UDP IQ transport. The app sends "START"/"STOP" to CMD_PORT at our_ip; we stream
 * frames back to the sender's IP:port from DATA_PORT. Payload is a synthetic
 * sequence+ramp for now (milestone 1: prove the pipe + measure throughput); real
 * RX samples replace it next. */
#define CMD_PORT   (5000)
#define DATA_PORT  (5001)
#define IQ_PAYLOAD (CDC_IQ_PAYLOAD)
static volatile bool iq_streaming = false;
static uint8_t iq_host_mac[6];
static uint8_t iq_host_ip[4];
static uint16_t iq_host_port = 0;
static uint32_t iq_seq = 0;

/* Diagnostics, readable via usb_vendor_request_cdc_debug (req 65). */
static volatile uint32_t cdc_rx_count = 0;
static volatile uint32_t cdc_arp_seen = 0;
static volatile uint32_t cdc_icmp_seen = 0;
static volatile uint32_t cdc_ipv6_seen = 0;
static volatile uint32_t cdc_dhcp_seen = 0;
static volatile uint32_t cdc_tx_count = 0;
static volatile uint32_t cdc_last_len = 0;
static uint8_t cdc_last_hdr[14];
static uint8_t cdc_dbg[64];

static void cdc_rx_complete(void* user_data, unsigned int bytes_transferred);

static void cdc_arm_rx(void)
{
	usb_transfer_schedule(
		&usb_endpoint_cdc_bulk_out,
		cdc_rx_frame,
		sizeof(cdc_rx_frame),
		cdc_rx_complete,
		NULL);
}

static void cdc_tx_complete(void* user_data, unsigned int bytes_transferred)
{
	(void) user_data;
	(void) bytes_transferred;
	cdc_tx_busy = false;
}

/* Send the frame in cdc_tx_frame (len bytes) to the host on the bulk IN endpoint. */
static void cdc_send(uint32_t len)
{
	cdc_tx_busy = true;
	cdc_tx_count++;
	usb_transfer_schedule(&usb_endpoint_cdc_bulk_in, cdc_tx_frame, len, cdc_tx_complete, NULL);
}

static void cdc_ctl_complete(void* user_data, unsigned int bytes_transferred)
{
	(void) user_data;
	(void) bytes_transferred;
	cdc_ctl_busy = false;
}

/* Send the control reply in cdc_ctl_frame (len bytes). ISR context. */
static void cdc_send_ctl(uint32_t len)
{
	cdc_ctl_busy = true;
	cdc_tx_count++;
	usb_transfer_schedule(&usb_endpoint_cdc_bulk_in, cdc_ctl_frame, len, cdc_ctl_complete, NULL);
}

/* One's-complement Internet checksum over len bytes (big-endian result). */
static uint16_t inet_checksum(const uint8_t* data, uint32_t len)
{
	uint32_t sum = 0;
	uint32_t i;
	for (i = 0; i + 1 < len; i += 2) {
		sum += ((uint32_t) data[i] << 8) | data[i + 1];
	}
	if (i < len) {
		sum += (uint32_t) data[i] << 8;
	}
	while (sum >> 16) {
		sum = (sum & 0xFFFF) + (sum >> 16);
	}
	return (uint16_t) ~sum;
}

/* Reply to an ARP request targeting our IP. */
static void handle_arp(const uint8_t* frame, unsigned int len)
{
	if (cdc_ctl_busy || len < 42) {
		return;
	}
	if (frame[20] != 0x00 || frame[21] != 0x01) {   /* oper == request */
		return;
	}
	if (memcmp(&frame[38], our_ip, 4) != 0) {        /* target proto addr == us */
		return;
	}
	uint8_t* t = cdc_ctl_frame;
	memcpy(&t[0], &frame[6], 6);   /* dst = requester MAC */
	memcpy(&t[6], our_mac, 6);     /* src = us */
	t[12] = 0x08; t[13] = 0x06;    /* ethertype ARP */
	t[14] = 0x00; t[15] = 0x01;    /* htype ethernet */
	t[16] = 0x08; t[17] = 0x00;    /* ptype IPv4 */
	t[18] = 6; t[19] = 4;          /* hlen / plen */
	t[20] = 0x00; t[21] = 0x02;    /* oper = reply */
	memcpy(&t[22], our_mac, 6);    /* sender hw = us */
	memcpy(&t[28], our_ip, 4);     /* sender proto = our IP */
	memcpy(&t[32], &frame[22], 6); /* target hw = requester */
	memcpy(&t[38], &frame[28], 4); /* target proto = requester IP */
	cdc_send_ctl(42);
}

/* Reply to an ICMP echo request (ping) addressed to our IP. */
static void handle_icmp(const uint8_t* frame, unsigned int len)
{
	if (cdc_ctl_busy || len < 34) {
		return;
	}
	const uint32_t ihl = (uint32_t)(frame[14] & 0x0F) * 4;
	if (ihl < 20 || 14 + ihl + 8 > len) {
		return;
	}
	if (frame[23] != 1) {                            /* protocol == ICMP */
		return;
	}
	if (memcmp(&frame[30], our_ip, 4) != 0) {        /* dst IP == us */
		return;
	}
	const uint32_t l3 = 14 + ihl;
	if (frame[l3] != 8) {                            /* ICMP type == echo request */
		return;
	}
	const uint32_t ip_total = ((uint32_t) frame[16] << 8) | frame[17];
	const uint32_t reply_len = 14 + ip_total;
	if (ip_total < ihl + 8 || reply_len > len || reply_len > ETH_FRAME_MAX) {
		return;
	}
	memcpy(cdc_ctl_frame, frame, reply_len);
	uint8_t* t = cdc_ctl_frame;
	memcpy(&t[0], &frame[6], 6);       /* dst = sender */
	memcpy(&t[6], our_mac, 6);         /* src = us */
	memcpy(&t[26], &frame[30], 4);     /* IP src = old dst (us) */
	memcpy(&t[30], &frame[26], 4);     /* IP dst = old src */
	t[24] = 0; t[25] = 0;              /* IP header checksum */
	uint16_t ipsum = inet_checksum(&t[14], ihl);
	t[24] = (uint8_t)(ipsum >> 8); t[25] = (uint8_t)(ipsum & 0xFF);
	t[l3] = 0;                         /* ICMP type = echo reply */
	t[l3 + 2] = 0; t[l3 + 3] = 0;      /* ICMP checksum */
	uint16_t icsum = inet_checksum(&t[l3], ip_total - ihl);
	t[l3 + 2] = (uint8_t)(icsum >> 8); t[l3 + 3] = (uint8_t)(icsum & 0xFF);
	cdc_send_ctl(reply_len);
}

/* Minimal DHCP server: OFFER/ACK offer_ip to the host so it configures this link
 * on our private /24. We deliberately send NO router (option 3) and no DNS: a
 * gateway here would make the host install a default route through us, hijacking
 * its internet path (and killing anything the host was doing over WiFi). The /24
 * is on-link, so the host still reaches us (our_ip) directly with no gateway. */
static void handle_dhcp(const uint8_t* f, unsigned int len)
{
	if (cdc_ctl_busy) {
		return;
	}
	const uint32_t ihl = (uint32_t)(f[14] & 0x0F) * 4;
	const uint32_t udp = 14 + ihl;
	const uint32_t bootp = udp + 8;
	if (bootp + 240 > len || f[bootp] != 1) {   /* op == BOOTREQUEST + room for magic */
		return;
	}
	uint8_t msgtype = 0;
	uint32_t o = bootp + 240;                    /* options, past the magic cookie */
	while (o + 1 < len) {
		const uint8_t code = f[o];
		if (code == 255) {
			break;
		}
		if (code == 0) {
			o++;
			continue;
		}
		const uint8_t olen = f[o + 1];
		if (code == 53 && olen >= 1 && o + 2 < len) {
			msgtype = f[o + 2];
		}
		o += 2 + olen;
	}
	uint8_t reply_type;
	if (msgtype == 1) {
		reply_type = 2;   /* DISCOVER -> OFFER */
	} else if (msgtype == 3) {
		reply_type = 5;   /* REQUEST -> ACK */
	} else {
		return;
	}

	uint8_t* t = cdc_ctl_frame;
	memset(t, 0, 320);
	memset(&t[0], 0xFF, 6);                  /* dst = broadcast */
	memcpy(&t[6], our_mac, 6);               /* src = us */
	t[12] = 0x08; t[13] = 0x00;              /* IPv4 */

	const uint32_t b = 42;                   /* BOOTP start */
	t[b + 0] = 2; t[b + 1] = 1; t[b + 2] = 6;
	memcpy(&t[b + 4], &f[bootp + 4], 4);     /* xid */
	memcpy(&t[b + 10], &f[bootp + 10], 2);   /* flags */
	memcpy(&t[b + 16], offer_ip, 4);         /* yiaddr */
	memcpy(&t[b + 20], our_ip, 4);           /* siaddr */
	memcpy(&t[b + 28], &f[bootp + 28], 16);  /* chaddr */
	t[b + 236] = 0x63; t[b + 237] = 0x82; t[b + 238] = 0x53; t[b + 239] = 0x63;

	uint32_t p = b + 240;
	t[p++] = 53; t[p++] = 1; t[p++] = reply_type;
	t[p++] = 54; t[p++] = 4; memcpy(&t[p], our_ip, 4); p += 4;
	t[p++] = 1;  t[p++] = 4; t[p++] = 255; t[p++] = 255; t[p++] = 255; t[p++] = 0;
	/* NO option 3 (router) on purpose — see comment above; a gateway here would
	 * steal the host's default route and kill its WiFi/internet path. */
	t[p++] = 51; t[p++] = 4; t[p++] = 0; t[p++] = 0x01; t[p++] = 0x51; t[p++] = 0x80;
	t[p++] = 255;

	const uint32_t frame_len = p;
	const uint32_t ip_total = frame_len - 14;
	const uint32_t udp_len = frame_len - 34;
	t[14] = 0x45;
	t[16] = (uint8_t)(ip_total >> 8); t[17] = (uint8_t)(ip_total & 0xFF);
	t[22] = 64; t[23] = 17;                  /* TTL, UDP */
	memcpy(&t[26], our_ip, 4);
	memset(&t[30], 0xFF, 4);                 /* dst = 255.255.255.255 */
	uint16_t ipsum = inet_checksum(&t[14], 20);
	t[24] = (uint8_t)(ipsum >> 8); t[25] = (uint8_t)(ipsum & 0xFF);
	t[34] = 0; t[35] = 67; t[36] = 0; t[37] = 68;   /* UDP ports 67 -> 68 */
	t[38] = (uint8_t)(udp_len >> 8); t[39] = (uint8_t)(udp_len & 0xFF);
	cdc_send_ctl(frame_len);
}

bool usb_cdc_iq_active(void)
{
	return iq_streaming;
}

/* Send one UDP IQ frame: 4-byte big-endian sequence number followed by len bytes
 * of IQ copied from src. The seq lets the host detect drops; it does not consume
 * IQ budget, so 16 frames of CDC_IQ_PAYLOAD carry exactly one 16 KB bulk block.
 * Spins until the previous frame has drained (the USB ISR clears cdc_tx_busy). */
void usb_cdc_send_iq(const uint8_t* src, uint32_t len)
{
	if (len > CDC_IQ_PAYLOAD) {
		len = CDC_IQ_PAYLOAD;
	}
	while (cdc_tx_busy) {
		/* wait for the prior frame to finish on the wire */
	}
	uint8_t* t = cdc_tx_frame;
	const uint32_t udp_payload = 4 + len;      /* seq + IQ */
	const uint32_t frame_len = 42 + udp_payload;
	/* Ethernet */
	memcpy(&t[0], iq_host_mac, 6);
	memcpy(&t[6], our_mac, 6);
	t[12] = 0x08; t[13] = 0x00;
	/* IPv4 header */
	memset(&t[14], 0, 20);
	t[14] = 0x45;
	const uint32_t ip_total = frame_len - 14;
	t[16] = (uint8_t)(ip_total >> 8); t[17] = (uint8_t)(ip_total & 0xFF);
	t[18] = (uint8_t)(iq_seq >> 8); t[19] = (uint8_t)(iq_seq & 0xFF);  /* IP id */
	t[22] = 64; t[23] = 17;                            /* TTL, proto = UDP */
	memcpy(&t[26], our_ip, 4);
	memcpy(&t[30], iq_host_ip, 4);
	uint16_t ipsum = inet_checksum(&t[14], 20);
	t[24] = (uint8_t)(ipsum >> 8); t[25] = (uint8_t)(ipsum & 0xFF);
	/* UDP header (checksum 0 = not computed, valid for IPv4) */
	const uint32_t udp_len = 8 + udp_payload;
	t[34] = (uint8_t)(DATA_PORT >> 8); t[35] = (uint8_t)(DATA_PORT & 0xFF);
	t[36] = (uint8_t)(iq_host_port >> 8); t[37] = (uint8_t)(iq_host_port & 0xFF);
	t[38] = (uint8_t)(udp_len >> 8); t[39] = (uint8_t)(udp_len & 0xFF);
	t[40] = 0; t[41] = 0;
	/* Payload: seq then IQ */
	t[42] = (uint8_t)(iq_seq >> 24); t[43] = (uint8_t)(iq_seq >> 16);
	t[44] = (uint8_t)(iq_seq >> 8);  t[45] = (uint8_t)(iq_seq & 0xFF);
	memcpy(&t[46], src, len);
	iq_seq++;
	cdc_send(frame_len);
}

/* Parse an unsigned decimal at p[0..len); returns the digit count consumed. */
static uint32_t parse_u64(const uint8_t* p, uint32_t len, uint64_t* out)
{
	uint64_t v = 0;
	uint32_t i = 0;
	while (i < len && p[i] >= '0' && p[i] <= '9') {
		v = v * 10 + (uint64_t)(p[i] - '0');
		i++;
	}
	*out = v;
	return i;
}

/* True when the datagram payload starts with the word (followed by end, space,
 * CR or LF), so "START" cannot match "STARTX". */
static bool cmd_is(const uint8_t* d, uint32_t dlen, const char* word)
{
	uint32_t n = 0;
	while (word[n] != 0) {
		n++;
	}
	if (dlen < n || memcmp(d, word, n) != 0) {
		return false;
	}
	return dlen == n || d[n] == ' ' || d[n] == '\r' || d[n] == '\n';
}

/* Reply to a command datagram (sender MAC/IP/port taken from the request
 * frame f) with an ASCII payload. ISR context; skipped if a control reply is
 * still in flight, the host simply asks again. */
static void cdc_send_udp_reply(const uint8_t* f, uint32_t udp, const char* text)
{
	if (cdc_ctl_busy) {
		return;
	}
	uint32_t len = 0;
	while (text[len] != 0 && len < 200) {
		len++;
	}
	uint8_t* t = cdc_ctl_frame;
	const uint32_t frame_len = 42 + len;
	memcpy(&t[0], &f[6], 6);                    /* dst = requester MAC */
	memcpy(&t[6], our_mac, 6);
	t[12] = 0x08; t[13] = 0x00;
	memset(&t[14], 0, 20);
	t[14] = 0x45;
	const uint32_t ip_total = frame_len - 14;
	t[16] = (uint8_t)(ip_total >> 8); t[17] = (uint8_t)(ip_total & 0xFF);
	t[22] = 64; t[23] = 17;
	memcpy(&t[26], our_ip, 4);
	memcpy(&t[30], &f[26], 4);                  /* dst IP = requester */
	uint16_t ipsum = inet_checksum(&t[14], 20);
	t[24] = (uint8_t)(ipsum >> 8); t[25] = (uint8_t)(ipsum & 0xFF);
	const uint32_t udp_len = 8 + len;
	t[34] = (uint8_t)(CMD_PORT >> 8); t[35] = (uint8_t)(CMD_PORT & 0xFF);
	t[36] = f[udp]; t[37] = f[udp + 1];          /* dst port = requester src port */
	t[38] = (uint8_t)(udp_len >> 8); t[39] = (uint8_t)(udp_len & 0xFF);
	t[40] = 0; t[41] = 0;
	memcpy(&t[42], text, len);
	cdc_send_ctl(frame_len);
}

/* Protocol version reported by PONG; bump when the command set changes. */
#define CDC_CMD_PROTOCOL "2"

/* UDP datagram on CMD_PORT. ASCII commands, one per datagram:
 *   START           stream IQ to the sender (this MAC/IP/port)
 *   STOP            stop streaming
 *   FREQ <hz>       tune (applies live while streaming; used for voice follow)
 *   RATE <hz>       sample rate
 *   BW <hz>         baseband filter bandwidth
 *   AMP <0|1>       RF amp
 *   LNA <db>        LNA gain (0..40, 8 dB steps)
 *   VGA <db>        VGA gain (0..62, 2 dB steps)
 *   PING            reply "PONG <proto> <streaming>" (liveness / discovery)
 * Numbers are unsigned decimal. Unknown commands are ignored. */
static void handle_cmd(const uint8_t* f, unsigned int len)
{
	const uint32_t ihl = (uint32_t)(f[14] & 0x0F) * 4;
	const uint32_t udp = 14 + ihl;
	const uint32_t data = udp + 8;
	if (data > len) {
		return;
	}
	const uint8_t* d = &f[data];
	const uint32_t dlen = len - data;
	uint64_t arg = 0;

	if (cmd_is(d, dlen, "START")) {
		memcpy(iq_host_mac, &f[6], 6);
		memcpy(iq_host_ip, &f[26], 4);
		iq_host_port = ((uint16_t) f[udp] << 8) | f[udp + 1];
		iq_seq = 0;
		iq_streaming = true;
		/* Kick the RX pipeline; rx_mode (main loop) will pump samples to
		 * usb_cdc_send_iq() while usb_cdc_iq_active() is true. */
		request_transceiver_mode(TRANSCEIVER_MODE_RX);
	} else if (cmd_is(d, dlen, "STOP")) {
		iq_streaming = false;
		request_transceiver_mode(TRANSCEIVER_MODE_OFF);
	} else if (cmd_is(d, dlen, "PING")) {
		cdc_send_udp_reply(f, udp,
			iq_streaming ? "PONG " CDC_CMD_PROTOCOL " 1" : "PONG " CDC_CMD_PROTOCOL " 0");
	} else if (cmd_is(d, dlen, "FREQ") && dlen > 5 && parse_u64(&d[5], dlen - 5, &arg) > 0) {
		transceiver_apply_freq(arg);
	} else if (cmd_is(d, dlen, "RATE") && dlen > 5 && parse_u64(&d[5], dlen - 5, &arg) > 0) {
		transceiver_apply_sample_rate((uint32_t) arg, 1);
	} else if (cmd_is(d, dlen, "BW") && dlen > 3 && parse_u64(&d[3], dlen - 3, &arg) > 0) {
		transceiver_apply_bb_bandwidth((uint32_t) arg);
	} else if (cmd_is(d, dlen, "AMP") && dlen > 4 && parse_u64(&d[4], dlen - 4, &arg) > 0) {
		transceiver_apply_amp(arg != 0);
	} else if (cmd_is(d, dlen, "LNA") && dlen > 4 && parse_u64(&d[4], dlen - 4, &arg) > 0) {
		transceiver_apply_lna_gain(arg > 40 ? 40 : (uint8_t) arg);
	} else if (cmd_is(d, dlen, "VGA") && dlen > 4 && parse_u64(&d[4], dlen - 4, &arg) > 0) {
		transceiver_apply_vga_gain(arg > 62 ? 62 : (uint8_t) arg);
	}
}

static void cdc_rx_complete(void* user_data, unsigned int bytes_transferred)
{
	(void) user_data;
	const uint8_t* f = cdc_rx_frame;
	cdc_rx_count++;
	if (bytes_transferred >= 14) {
		memcpy(cdc_last_hdr, f, 14);
		cdc_last_len = bytes_transferred;
		const uint16_t ethertype = ((uint16_t) f[12] << 8) | f[13];
		if (ethertype == 0x0806) {
			cdc_arp_seen++;
			handle_arp(f, bytes_transferred);
		} else if (ethertype == 0x0800) {
			cdc_icmp_seen++;   /* repurposed: IPv4 frames seen */
			if (bytes_transferred >= 34) {
				const uint32_t ihl = (uint32_t)(f[14] & 0x0F) * 4;
				const uint8_t proto = f[23];
				if (proto == 1) {
					handle_icmp(f, bytes_transferred);
				} else if (proto == 17 && 14 + ihl + 8 <= bytes_transferred) {
					const uint32_t udp = 14 + ihl;
					const uint16_t dport =
						((uint16_t) f[udp + 2] << 8) | f[udp + 3];
					if (dport == 67) {
						cdc_dhcp_seen++;
						handle_dhcp(f, bytes_transferred);
					} else if (dport == CMD_PORT) {
						handle_cmd(f, bytes_transferred);
					}
				}
			}
		} else if (ethertype == 0x86DD) {
			cdc_ipv6_seen++;
		}
	}
	cdc_arm_rx();
}

static void cdc_send_link_up(void)
{
	/* CDC NETWORK_CONNECTION notification, value 1 = connected. Sent on the
	 * interrupt notification endpoint so the host marks the link up. */
	cdc_notify_buf[0] = 0xA1;               /* bmRequestType: in|class|interface */
	cdc_notify_buf[1] = 0x00;               /* bNotification: NETWORK_CONNECTION */
	cdc_notify_buf[2] = 0x01;               /* wValue lo = 1 (connected) */
	cdc_notify_buf[3] = 0x00;               /* wValue hi */
	cdc_notify_buf[4] = CDC_COMM_INTERFACE; /* wIndex lo = comm interface */
	cdc_notify_buf[5] = 0x00;               /* wIndex hi */
	cdc_notify_buf[6] = 0x00;               /* wLength lo = 0 */
	cdc_notify_buf[7] = 0x00;               /* wLength hi */
	usb_transfer_schedule(
		&usb_endpoint_cdc_notify,
		cdc_notify_buf,
		sizeof(cdc_notify_buf),
		NULL,
		NULL);
}

void usb_cdc_init(void)
{
	/* New configuration: nothing armed until the host activates the data
	 * interface (alt 1). */
	cdc_rx_armed = false;
}

void usb_cdc_set_interface(
	usb_device_t* const device,
	const uint8_t interface,
	const uint8_t alt)
{
	(void) device;
	if (interface != CDC_DATA_INTERFACE) {
		return;
	}
	if (alt == CDC_DATA_ALT_ON) {
		if (!cdc_rx_armed) {
			cdc_rx_armed = true;
			cdc_arm_rx();
		}
		cdc_send_link_up();
	} else {
		cdc_rx_armed = false;
	}
}

usb_request_status_t usb_cdc_request(
	usb_endpoint_t* const endpoint,
	const usb_transfer_stage_t stage)
{
	const bool device_to_host = (endpoint->setup.request_type & 0x80) != 0;

	if (stage == USB_TRANSFER_STAGE_SETUP) {
		if (device_to_host) {
			/* GET-class requests (statistics, etc.): answer with zeros. */
			uint32_t n = endpoint->setup.length;
			if (n > sizeof(cdc_scratch)) {
				n = sizeof(cdc_scratch);
			}
			memset(cdc_scratch, 0, n);
			usb_transfer_schedule_block(
				endpoint->in, cdc_scratch, n, NULL, NULL);
			usb_transfer_schedule_ack(endpoint->out);
		} else if (endpoint->setup.length > 0) {
			/* SET-class requests carrying data (e.g. multicast filters):
			 * accept into scratch and drop. */
			uint32_t n = endpoint->setup.length;
			if (n > sizeof(cdc_scratch)) {
				n = sizeof(cdc_scratch);
			}
			usb_transfer_schedule_block(
				endpoint->out, cdc_scratch, n, NULL, NULL);
		} else {
			/* No-data SET (e.g. SET_ETHERNET_PACKET_FILTER): just ack. */
			usb_transfer_schedule_ack(endpoint->in);
		}
	} else if (stage == USB_TRANSFER_STAGE_DATA) {
		if (!device_to_host && endpoint->setup.length > 0) {
			usb_transfer_schedule_ack(endpoint->in);
		}
	}
	return USB_REQUEST_STATUS_OK;
}

/* --- Ethernet on/off toggle (persisted in no-init RAM, applied via reset) --- */

/* Config-descriptor field values for the two personalities. The composite blob
 * begins with exactly the vendor-only bytes (config header + IF0 + its two
 * endpoints = 32), so "vendor-only" is just the composite blob served short. */
#define CFG_TOTAL_COMPOSITE   (103)
#define CFG_TOTAL_VENDOR_ONLY (32)
#define CFG_NINTF_COMPOSITE   (3)
#define CFG_NINTF_VENDOR_ONLY (1)

/*
 * No-init persistence. ram_sleep (the top 8 KB SRAM bank at 0x10088000 on the
 * LPC4320) is unused by the linker layout, so the C startup never initializes it.
 * SRAM survives a warm/watchdog reset but is lost on a real power cycle, which is
 * exactly the behavior we want: the desktop's "off" sticks across the reset-based
 * re-enumeration, and unplugging (to move the radio to the iPhone) powers it down
 * and defaults back to "on". A mute host (iPhone) therefore always gets ethernet.
 */
#define ETH_NOINIT_ADDR  (0x10088000UL)
#define ETH_NOINIT_MAGIC (0xE7C0FFEEUL)

typedef struct {
	uint32_t magic;
	uint32_t eth_on;
} eth_noinit_t;

#define ETH_NOINIT (*(volatile eth_noinit_t*) ETH_NOINIT_ADDR)

static void set_ethernet_descriptor(bool on)
{
	const uint8_t total = on ? CFG_TOTAL_COMPOSITE : CFG_TOTAL_VENDOR_ONLY;
	const uint8_t nintf = on ? CFG_NINTF_COMPOSITE : CFG_NINTF_VENDOR_ONLY;
	/* Patch wTotalLength (bytes 2..3) and bNumInterfaces (byte 4) in place. */
	usb_descriptor_configuration_high_speed[2] = total;
	usb_descriptor_configuration_high_speed[3] = 0;
	usb_descriptor_configuration_high_speed[4] = nintf;
	usb_descriptor_configuration_full_speed[2] = total;
	usb_descriptor_configuration_full_speed[3] = 0;
	usb_descriptor_configuration_full_speed[4] = nintf;
}

void usb_cdc_boot_init(void)
{
	/* Fresh power-up (no-init RAM garbage): default to ethernet ON. Otherwise
	 * honor the persisted choice. Patch the descriptor before USB comes up. */
	if (ETH_NOINIT.magic != ETH_NOINIT_MAGIC) {
		ETH_NOINIT.magic = ETH_NOINIT_MAGIC;
		ETH_NOINIT.eth_on = 1;
	}
	set_ethernet_descriptor(ETH_NOINIT.eth_on != 0);
}

usb_request_status_t usb_vendor_request_set_ethernet_mode(
	usb_endpoint_t* const endpoint,
	const usb_transfer_stage_t stage)
{
	if (stage == USB_TRANSFER_STAGE_SETUP) {
		ETH_NOINIT.magic = ETH_NOINIT_MAGIC;
		ETH_NOINIT.eth_on = (endpoint->setup.value & 0xFF) ? 1 : 0;
		/* Re-enumerate via a device reset (the only trigger the Apple USB-C host
		 * reliably re-reads on — see the Asahi/Type-C notes). On reboot,
		 * usb_cdc_boot_init() applies the persisted mode. The watchdog timeout
		 * lets the ACK status stage complete first, mirroring the reset request. */
		usb_transfer_schedule_ack(endpoint->in);
		wwdt_reset(100000);
	}
	return USB_REQUEST_STATUS_OK;
}

usb_request_status_t usb_vendor_request_cdc_debug(
	usb_endpoint_t* const endpoint,
	const usb_transfer_stage_t stage)
{
	if (stage == USB_TRANSFER_STAGE_SETUP) {
		const uint32_t vals[6] = {cdc_rx_count, cdc_arp_seen, cdc_icmp_seen,
					  cdc_ipv6_seen, cdc_dhcp_seen, cdc_tx_count};
		memcpy(&cdc_dbg[0], vals, sizeof(vals));   /* 24 bytes */
		cdc_dbg[24] = (uint8_t) cdc_last_len;
		cdc_dbg[25] = (uint8_t)(cdc_last_len >> 8);
		memcpy(&cdc_dbg[26], cdc_last_hdr, 14);    /* dst6 src6 ethertype2 */
		usb_transfer_schedule_block(endpoint->in, cdc_dbg, 40, NULL, NULL);
		usb_transfer_schedule_ack(endpoint->out);
	}
	return USB_REQUEST_STATUS_OK;
}
