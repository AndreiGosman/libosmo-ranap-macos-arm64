/* hnb-test: GTP-U user plane for the Iu-PS RAB set up by the SGSN.
 *
 * With osmo-sgsn the Iu-PS user plane is a direct tunnel: the RAB Assignment
 * Request carries the GGSN's GTP-U address and TEID, and the SGSN hands our
 * address and TEID from the RAB Assignment Response to the GGSN in an Update
 * PDP Context Request. So this file talks GTP-U (TS 29.281) straight to the
 * GGSN: T-PDUs out to the GGSN's TEID, T-PDUs in on our own TEID. Iu UP is
 * in transparent mode (TS 25.415 4.2.2), so there is no Iu UP frame at all,
 * the GTP-U payload is the IP packet of the UE.
 */

/* (C) 2026 by Andrei Gosman <andrei.gosman@gmail.com>
 * All Rights Reserved
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include <osmocom/core/socket.h>
#include <osmocom/core/select.h>
#include <osmocom/core/utils.h>

#include "hnb-test.h"
#include "hnb-test-layers.h"

#define GTPU_PORT		2152
#define GTPU_FLAGS_V1_GPDU	0x30	/* version 1, GTP (not GTP'), no E/S/PN */
#define GTPU_MSG_TPDU		0xff
#define GTPU_HDR_LEN		8

/* RFC 1071 checksum over a byte buffer */
static uint16_t inet_csum(const uint8_t *buf, unsigned int len)
{
	uint32_t sum = 0;
	unsigned int i;

	for (i = 0; i + 1 < len; i += 2)
		sum += (buf[i] << 8) | buf[i + 1];
	if (i < len)
		sum += buf[i] << 8;
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	return (uint16_t) ~sum;
}

static void print_tpdu(const uint8_t *ip, unsigned int len)
{
	char src[INET_ADDRSTRLEN], dst[INET_ADDRSTRLEN];

	if (len < 20 || (ip[0] >> 4) != 4) {
		printf("GTP-U rx T-PDU: %u bytes, not IPv4\n", len);
		return;
	}
	inet_ntop(AF_INET, ip + 12, src, sizeof(src));
	inet_ntop(AF_INET, ip + 16, dst, sizeof(dst));
	if (ip[9] == IPPROTO_ICMP && len >= 28) {
		const uint8_t *icmp = ip + (ip[0] & 0x0f) * 4;
		const char *what = icmp[0] == 0 ? "echo reply" : icmp[0] == 8 ? "echo request" : "message";
		printf("GTP-U rx T-PDU: ICMP %s type %u, %s > %s, id %u, seq %u, %u bytes\n",
		       what, icmp[0], src, dst, (icmp[4] << 8) | icmp[5], (icmp[6] << 8) | icmp[7], len);
		return;
	}
	printf("GTP-U rx T-PDU: IPv4 proto %u, %s > %s, %u bytes\n", ip[9], src, dst, len);
}

static int gtpu_read_cb(struct osmo_fd *ofd, unsigned int what)
{
	struct hnb_test *hnb = ofd->data;
	uint8_t buf[2048];
	struct sockaddr_in from;
	socklen_t from_len = sizeof(from);
	unsigned int hdr_len = GTPU_HDR_LEN;
	uint32_t teid;
	uint16_t plen;
	int rc;

	if (!(what & OSMO_FD_READ))
		return 0;

	rc = recvfrom(ofd->fd, buf, sizeof(buf), 0, (struct sockaddr *) &from, &from_len);
	if (rc < (int) GTPU_HDR_LEN) {
		printf("GTP-U rx: short datagram (%d)\n", rc);
		return 0;
	}
	if ((buf[0] & 0xe0) != 0x20) {
		printf("GTP-U rx: not GTP version 1 (flags 0x%02x)\n", buf[0]);
		return 0;
	}
	plen = (buf[2] << 8) | buf[3];
	teid = ((uint32_t) buf[4] << 24) | (buf[5] << 16) | (buf[6] << 8) | buf[7];
	/* E, S or PN present: 4 more header octets, which the length field
	 * already counts (TS 29.281 5.1). libgtp sets S on downlink G-PDUs. */
	if (buf[0] & 0x07) {
		if (plen < 4) {
			printf("GTP-U rx: length %u too short for the optional header fields\n", plen);
			return 0;
		}
		hdr_len += 4;
		plen -= 4;
	}

	if (buf[1] != GTPU_MSG_TPDU) {
		printf("GTP-U rx: message type 0x%02x, TEID 0x%08x, %u bytes from %s:%u (ignored)\n",
		       buf[1], teid, plen, inet_ntoa(from.sin_addr), ntohs(from.sin_port));
		return 0;
	}
	if (teid != hnb->ps.teid_local)
		printf("GTP-U rx: T-PDU on unexpected TEID 0x%08x (ours is 0x%08x)\n", teid, hnb->ps.teid_local);
	if ((unsigned int) rc < hdr_len + plen) {
		printf("GTP-U rx: truncated T-PDU (%d < %u)\n", rc, hdr_len + plen);
		return 0;
	}
	print_tpdu(buf + hdr_len, plen);
	return 0;
}

/* Bind the GTP-U socket on the address given with -G, once. The GGSN sends
 * downlink T-PDUs there after the SGSN's Update PDP Context. */
int hnb_test_gtpu_open(struct hnb_test *hnb)
{
	int rc;

	if (hnb->ps.gtpu_open)
		return 0;

	hnb->ps.gtpu_fd.cb = gtpu_read_cb;
	hnb->ps.gtpu_fd.data = hnb;
	rc = osmo_sock_init_ofd(&hnb->ps.gtpu_fd, AF_INET, SOCK_DGRAM, IPPROTO_UDP,
				hnb->gtpu_addr, GTPU_PORT, OSMO_SOCK_F_BIND);
	if (rc < 0) {
		printf("GTP-U: cannot bind %s:%u: %s\n", hnb->gtpu_addr, GTPU_PORT, strerror(errno));
		return rc;
	}
	hnb->ps.gtpu_open = true;
	printf("GTP-U: listening on %s:%u\n", hnb->gtpu_addr, GTPU_PORT);
	return 0;
}

/* Send one T-PDU to the GGSN's Iu-PS endpoint with the GGSN's TEID. */
int hnb_test_gtpu_tx_tpdu(struct hnb_test *hnb, const uint8_t *data, unsigned int len)
{
	uint8_t buf[2048];
	struct sockaddr_in to;
	int rc;

	if (!hnb->ps.rab_up) {
		printf("GTP-U: no RAB set up yet, nothing to send on\n");
		return -ENOTCONN;
	}
	if (len + GTPU_HDR_LEN > sizeof(buf))
		return -EMSGSIZE;

	buf[0] = GTPU_FLAGS_V1_GPDU;
	buf[1] = GTPU_MSG_TPDU;
	buf[2] = len >> 8;
	buf[3] = len & 0xff;
	buf[4] = hnb->ps.teid_remote >> 24;
	buf[5] = (hnb->ps.teid_remote >> 16) & 0xff;
	buf[6] = (hnb->ps.teid_remote >> 8) & 0xff;
	buf[7] = hnb->ps.teid_remote & 0xff;
	memcpy(buf + GTPU_HDR_LEN, data, len);

	memset(&to, 0, sizeof(to));
	to.sin_family = AF_INET;
	to.sin_port = htons(GTPU_PORT);
	to.sin_addr.s_addr = hnb->ps.gtpu_remote;

	rc = sendto(hnb->ps.gtpu_fd.fd, buf, len + GTPU_HDR_LEN, 0, (struct sockaddr *) &to, sizeof(to));
	if (rc < 0) {
		printf("GTP-U: sendto %s:%u failed: %s\n", inet_ntoa(to.sin_addr), GTPU_PORT, strerror(errno));
		return -errno;
	}
	return 0;
}

/* Craft an ICMP echo request from the PDP address to dst and send it as a
 * T-PDU. The reply, if any, comes back through gtpu_read_cb(). */
int hnb_test_ps_ping(struct hnb_test *hnb, const char *dst)
{
	uint8_t pkt[20 + 8 + 32];
	struct in_addr dst_addr;
	uint16_t seq = ++hnb->ps.icmp_seq;
	uint16_t csum;
	unsigned int i;

	if (!hnb->ps.pdp_addr) {
		printf("ping: no PDP address yet, activate a PDP context first\n");
		return -ENOTCONN;
	}
	if (inet_pton(AF_INET, dst, &dst_addr) != 1) {
		printf("ping: invalid IPv4 address %s\n", dst);
		return -EINVAL;
	}

	memset(pkt, 0, sizeof(pkt));
	/* IPv4 header */
	pkt[0] = 0x45;
	pkt[2] = sizeof(pkt) >> 8;
	pkt[3] = sizeof(pkt) & 0xff;
	pkt[4] = seq >> 8;
	pkt[5] = seq & 0xff;
	pkt[6] = 0x40;				/* don't fragment */
	pkt[8] = 64;				/* TTL */
	pkt[9] = IPPROTO_ICMP;
	pkt[12] = hnb->ps.pdp_addr >> 24;
	pkt[13] = (hnb->ps.pdp_addr >> 16) & 0xff;
	pkt[14] = (hnb->ps.pdp_addr >> 8) & 0xff;
	pkt[15] = hnb->ps.pdp_addr & 0xff;
	memcpy(pkt + 16, &dst_addr, 4);
	csum = inet_csum(pkt, 20);
	pkt[10] = csum >> 8;
	pkt[11] = csum & 0xff;
	/* ICMP echo request, id 0x4842 ("HB"), 32 bytes of payload */
	pkt[20] = 8;
	pkt[24] = 0x48;
	pkt[25] = 0x42;
	pkt[26] = seq >> 8;
	pkt[27] = seq & 0xff;
	for (i = 0; i < 32; i++)
		pkt[28 + i] = 'a' + (i % 26);
	csum = inet_csum(pkt + 20, sizeof(pkt) - 20);
	pkt[22] = csum >> 8;
	pkt[23] = csum & 0xff;

	printf("GTP-U tx T-PDU: ICMP echo request %u.%u.%u.%u > %s, id 0x4842, seq %u, to GGSN TEID 0x%08x\n",
	       pkt[12], pkt[13], pkt[14], pkt[15], dst, seq, hnb->ps.teid_remote);
	return hnb_test_gtpu_tx_tpdu(hnb, pkt, sizeof(pkt));
}
