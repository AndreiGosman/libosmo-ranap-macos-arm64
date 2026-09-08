/* GMM (GPRS Mobility Management) side of the test HNB: a minimal UE that
 * performs a GPRS attach on the Iu-PS signalling connection.
 *
 * (C) 2026 by Andrei Gosman <andrei.gosman@gmail.com>
 *
 * All Rights Reserved
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

#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <osmocom/core/msgb.h>
#include <osmocom/core/utils.h>
#include <osmocom/gsm/gsm48.h>
#include <osmocom/gsm/protocol/gsm_04_08.h>
#include <osmocom/gsm/protocol/gsm_04_08_gprs.h>
#include <osmocom/ranap/ranap_msg_factory.h>

#include "hnb-test.h"
#include "hnb-test-layers.h"

/* IMEISV of the test UE (16 digits); the IMEI is the first 14 plus a check digit */
static const char hnb_test_imeisv[] = "3534900000000101";
static const char hnb_test_imei[] = "353490000000010";

/*! Walk the optional IEs of a GMM or SM message (3GPP TS 24.007 section
 *  11.2.1.1) and call \a cb for each. Type-only and half-octet IEs have
 *  bit 8 of the IEI set. The TV IEs with a fixed length are listed in
 *  \a tv_len, everything else is TLV. */
struct hnb_test_tv_len {
	uint8_t iei;
	uint8_t len;
};

void hnb_test_for_each_ie(const uint8_t *p, int len,
			  const struct hnb_test_tv_len *tv_len,
			  void (*cb)(uint8_t iei, const uint8_t *val, uint8_t vlen, void *priv),
			  void *priv)
{
	int i = 0;

	while (i < len) {
		uint8_t iei = p[i];
		const struct hnb_test_tv_len *tv;
		uint8_t vlen;

		if (iei & 0x80) {
			/* type only, or type plus half octet of value */
			cb(iei & 0xf0, &p[i], 1, priv);
			i++;
			continue;
		}
		for (tv = tv_len; tv && tv->iei; tv++)
			if (tv->iei == iei)
				break;
		if (tv && tv->iei) {
			vlen = tv->len;
			if (i + 1 + vlen > len)
				return;
			cb(iei, &p[i + 1], vlen, priv);
			i += 1 + vlen;
			continue;
		}
		if (i + 2 > len)
			return;
		vlen = p[i + 1];
		if (i + 2 + vlen > len)
			return;
		cb(iei, &p[i + 2], vlen, priv);
		i += 2 + vlen;
	}
}

/* Mobile Identity as an LV: gsm48_generate_mid*() write a TLV, drop the IEI */
static int gen_mi_lv(uint8_t *buf, const char *id, uint8_t mi_type)
{
	uint8_t tlv[GSM48_MID_MAX_SIZE];
	int len;

	if (mi_type == GSM_MI_TYPE_IMSI)
		len = gsm48_generate_mid_from_imsi(tlv, id);
	else
		len = gsm48_generate_mid(tlv, id, mi_type);
	if (len < 2)
		return -EINVAL;
	memcpy(buf, tlv + 1, len - 1);
	return len - 1;
}

/*! GMM Attach Request (3GPP TS 24.008 section 9.4.1): GPRS attach with the
 *  IMSI as identity, no ciphering key, old RAI 262-03-16931-255. */
int hnb_test_gen_gmm_attach_req(uint8_t *buf, size_t size, const char *imsi)
{
	/* MS Radio Access Capability of a common GSM/EGPRS phone; the SGSN
	 * stores it and passes it on, it does not act on it */
	static const uint8_t ra_cap[] = {
		0x18, 0xb3, 0x43, 0x2b, 0x25, 0x96, 0x62, 0x00, 0x60, 0x80,
		0x9a, 0xc2, 0xc6, 0x62, 0x00, 0x60, 0x80, 0xba, 0xc8, 0xc6,
		0x62, 0x00, 0x60, 0x80, 0x00
	};
	unsigned int len = 0;
	int rc;

	if (size < 2 + 3 + 1 + 2 + 9 + 6 + 1 + sizeof(ra_cap))
		return -ENOSPC;

	buf[len++] = GSM48_PDISC_MM_GPRS;
	buf[len++] = GSM48_MT_GMM_ATTACH_REQ;
	/* MS network capability (LV): GEA/1, SM over dedicated and GPRS channels, UCS2 */
	buf[len++] = 2;
	buf[len++] = 0xe5;
	buf[len++] = 0xe0;
	/* Ciphering key sequence number 7 (none), attach type 1 (GPRS attach) */
	buf[len++] = 0x71;
	/* DRX parameter: split PG cycle code 8, no split on CCCH */
	buf[len++] = 0x08;
	buf[len++] = 0x02;
	/* Mobile identity (LV): IMSI */
	rc = gen_mi_lv(buf + len, imsi, GSM_MI_TYPE_IMSI);
	if (rc < 0)
		return rc;
	len += rc;
	/* Old routing area identification: MCC 262, MNC 03, LAC 16931, RAC 255 */
	buf[len++] = 0x62;
	buf[len++] = 0xf2;
	buf[len++] = 0x30;
	buf[len++] = 0x42;
	buf[len++] = 0x23;
	buf[len++] = 0xff;
	/* MS radio access capability (LV) */
	buf[len++] = sizeof(ra_cap);
	memcpy(buf + len, ra_cap, sizeof(ra_cap));
	len += sizeof(ra_cap);

	return len;
}

/* GMM Identity Response (section 9.4.13) with the identity the SGSN asked for */
static struct msgb *gen_gmm_id_resp(struct hnb_test *hnb, uint8_t mi_type)
{
	uint8_t buf[2 + GSM48_MID_MAX_SIZE];
	unsigned int len = 0;
	const char *id;
	int rc;

	buf[len++] = GSM48_PDISC_MM_GPRS;
	buf[len++] = GSM48_MT_GMM_ID_RESP;

	switch (mi_type) {
	case GSM_MI_TYPE_IMSI:
		id = hnb->ps.chan ? hnb->ps.chan->imsi : "262420123456789";
		break;
	case GSM_MI_TYPE_IMEI:
		id = hnb_test_imei;
		break;
	case GSM_MI_TYPE_IMEISV:
		id = hnb_test_imeisv;
		break;
	default:
		printf("GMM Identity Request for unsupported identity type %u\n", mi_type);
		return NULL;
	}

	rc = gen_mi_lv(buf + len, id, mi_type);
	if (rc < 0)
		return NULL;
	len += rc;

	return ranap_new_msg_dt(0, buf, len);
}

/* GMM Authentication and Ciphering Response (section 9.4.10): the A&C
 * reference number of the request, the first four octets of the answer in
 * the Authentication Response Parameter IE, any further octets in its
 * extension, and the IMEISV that osmo-sgsn insists on. */
static struct msgb *gen_gmm_auth_ciph_resp(uint8_t ac_ref, const uint8_t *res, int res_len)
{
	uint8_t buf[3 + 5 + 2 + GSM48_MID_MAX_SIZE + 2 + 12];
	unsigned int len = 0;
	int rc;

	buf[len++] = GSM48_PDISC_MM_GPRS;
	buf[len++] = GSM48_MT_GMM_AUTH_CIPH_RESP;
	buf[len++] = ac_ref & 0x0f;
	buf[len++] = GSM48_IE_GMM_AUTH_SRES;
	memcpy(buf + len, res, 4);
	len += 4;
	buf[len++] = GSM48_IE_GMM_IMEISV;
	rc = gen_mi_lv(buf + len + 1, hnb_test_imeisv, GSM_MI_TYPE_IMEISV);
	if (rc < 0)
		return NULL;
	buf[len] = rc;
	len += 1 + rc;
	if (res_len > 4) {
		buf[len++] = GSM48_IE_GMM_AUTH_RES_EXT;
		buf[len++] = res_len - 4;
		memcpy(buf + len, res + 4, res_len - 4);
		len += res_len - 4;
	}

	return ranap_new_msg_dt(0, buf, len);
}

/* GMM Attach Complete (section 9.4.3) */
static struct msgb *gen_gmm_attach_compl(void)
{
	uint8_t buf[] = { GSM48_PDISC_MM_GPRS, GSM48_MT_GMM_ATTACH_COMPL };

	return ranap_new_msg_dt(0, buf, sizeof(buf));
}

struct auth_ciph_req {
	const uint8_t *rand;
	const uint8_t *autn;
};

static void auth_ciph_req_ie(uint8_t iei, const uint8_t *val, uint8_t vlen, void *priv)
{
	struct auth_ciph_req *req = priv;

	switch (iei) {
	case GSM48_IE_GMM_AUTH_RAND:
		req->rand = val;
		break;
	case GSM48_IE_GMM_AUTN:
		if (vlen == 16)
			req->autn = val;
		break;
	default:
		break;
	}
}

static int rx_gmm_auth_ciph_req(struct hnb_test *hnb, struct gsm48_hdr *gh, int len)
{
	static const struct hnb_test_tv_len tv_len[] = {
		{ GSM48_IE_GMM_AUTH_RAND, 16 },
		{ 0, 0 }
	};
	struct auth_ciph_req req = {};
	uint8_t ac_ref;
	uint8_t res[16];
	int res_len;

	if (len < sizeof(*gh) + 2) {
		printf("GMM Authentication and Ciphering Request too short\n");
		return -EINVAL;
	}

	/* octet 3: ciphering algorithm and IMEISV request;
	 * octet 4: force to standby (bits 1-4), A&C reference number (bits 5-8) */
	ac_ref = gh->data[1] >> 4;
	hnb_test_for_each_ie(&gh->data[2], len - sizeof(*gh) - 2, tv_len, auth_ciph_req_ie, &req);

	printf(" :) GMM Authentication and Ciphering Request, A&C ref %u %s\n", ac_ref,
	       req.autn ? "(UMTS AKA, AUTN present)" : "(GSM AKA)");
	if (!req.rand) {
		printf("GMM Authentication and Ciphering Request without RAND\n");
		return -EINVAL;
	}

	res_len = hnb_test_auth_answer(req.rand, req.autn, res, sizeof(res));
	if (res_len < 0)
		return res_len;

	return hnb_test_tx_dt(hnb, gen_gmm_auth_ciph_resp(ac_ref, res, res_len));
}

static void attach_acc_ie(uint8_t iei, const uint8_t *val, uint8_t vlen, void *priv)
{
	struct hnb_test *hnb = priv;
	struct osmo_mobile_identity mi;

	switch (iei) {
	case GSM48_IE_GMM_ALLOC_PTMSI:
		if (osmo_mobile_identity_decode(&mi, val, vlen, false) == 0 && mi.type == GSM_MI_TYPE_TMSI) {
			hnb->ps.ptmsi = mi.tmsi;
			printf("Attach Accept: P-TMSI 0x%08x\n", mi.tmsi);
		}
		break;
	case GSM48_IE_GMM_PTMSI_SIG:
		printf("Attach Accept: P-TMSI signature %s\n", osmo_hexdump_nospc(val, vlen));
		break;
	case GSM48_IE_GMM_CAUSE:
		printf("Attach Accept: GMM cause %s\n", get_value_string(gsm48_gmm_cause_names, val[0]));
		break;
	default:
		break;
	}
}

static int rx_gmm_attach_acc(struct hnb_test *hnb, struct gsm48_hdr *gh, int len)
{
	static const struct hnb_test_tv_len tv_len[] = {
		{ GSM48_IE_GMM_PTMSI_SIG, 3 },
		{ GSM48_IE_GMM_TIMER_READY, 1 },
		{ GSM48_IE_GMM_CAUSE, 1 },
		{ 0, 0 }
	};
	struct gsm48_attach_ack *aa;
	struct gprs_ra_id ra_id;

	if (len < sizeof(*gh) + sizeof(*aa)) {
		printf("GMM Attach Accept too short\n");
		return -EINVAL;
	}
	aa = (struct gsm48_attach_ack *)gh->data;
	gsm48_parse_ra(&ra_id, (const uint8_t *)&aa->ra_id);

	printf(" :D GMM Attach Accept :D result %u, RAI %u-%u-%u-%u, periodic RAU timer 0x%02x\n",
	       aa->att_result, ra_id.mcc, ra_id.mnc, ra_id.lac, ra_id.rac, aa->ra_upd_timer);

	hnb_test_for_each_ie((const uint8_t *)aa + sizeof(*aa), len - sizeof(*gh) - sizeof(*aa),
			     tv_len, attach_acc_ie, hnb);

	hnb->ps.attached = 1;
	return hnb_test_tx_dt(hnb, gen_gmm_attach_compl());
}

int hnb_test_nas_rx_gmm(struct hnb_test *hnb, struct gsm48_hdr *gh, int len)
{
	uint8_t msg_type = gsm48_hdr_msg_type(gh);

	if (!hnb->ps.chan) {
		printf("hnb_test_nas_rx_gmm(): No PS channel established yet.\n");
		return -1;
	}

	switch (msg_type) {
	case GSM48_MT_GMM_ID_REQ:
		if (len < sizeof(*gh) + 1)
			return -EINVAL;
		printf("GMM Identity Request, type %u\n", gh->data[0] & 0x0f);
		return hnb_test_tx_dt(hnb, gen_gmm_id_resp(hnb, gh->data[0] & 0x0f));

	case GSM48_MT_GMM_AUTH_CIPH_REQ:
		return rx_gmm_auth_ciph_req(hnb, gh, len);

	case GSM48_MT_GMM_AUTH_CIPH_REJ:
		printf("GMM Authentication and Ciphering Reject\n");
		return 0;

	case GSM48_MT_GMM_ATTACH_ACK:
		return rx_gmm_attach_acc(hnb, gh, len);

	case GSM48_MT_GMM_ATTACH_REJ:
		if (len < sizeof(*gh) + 1)
			return -EINVAL;
		printf("GMM Attach Reject, cause %u (%s)\n", gh->data[0],
		       get_value_string(gsm48_gmm_cause_names, gh->data[0]));
		return 0;

	case GSM48_MT_GMM_DETACH_REQ:
		printf("GMM Detach Request from the network\n");
		return 0;

	case GSM48_MT_GMM_INFO:
		printf("GMM Information\n");
		return 0;

	case GSM48_MT_GMM_STATUS:
		if (len < sizeof(*gh) + 1)
			return -EINVAL;
		printf("GMM Status, cause %u (%s)\n", gh->data[0],
		       get_value_string(gsm48_gmm_cause_names, gh->data[0]));
		return 0;

	default:
		printf("GMM message type not handled by hnb-test: 0x%02x\n", msg_type);
		return 0;
	}
}
