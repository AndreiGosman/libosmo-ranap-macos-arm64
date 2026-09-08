#include <osmocom/core/msgb.h>
#include <osmocom/ranap/ranap_ies_defs.h>
#include <osmocom/ranap/iu_helpers.h>
#include <osmocom/ranap/ranap_common.h>
#include <osmocom/ranap/RANAP_RAB-AssignmentResponse.h>

#include "hnb-test.h"

#include "hnb-test-layers.h"

static const char *printstr(OCTET_STRING_t *s)
{
	return osmo_hexdump((const unsigned char*)s->buf, s->size);
}

#define PP(octet_string_t) \
	printf(#octet_string_t " = %s\n",\
	       printstr(&octet_string_t))

void hnb_test_rua_dt_handle_ranap(void *priv, struct ranap_message_s *ranap_msg)
{
	int len;
	uint8_t *data;
	RANAP_PermittedIntegrityProtectionAlgorithms_t *algs;
	RANAP_IntegrityProtectionAlgorithm_t *first_alg;
	struct hnb_test *hnb = priv;

	printf("rx ranap_msg->procedureCode %d\n",
	       ranap_msg->procedureCode);

	switch (ranap_msg->procedureCode) {
	case RANAP_ProcedureCode_id_DirectTransfer:
		printf("rx DirectTransfer: presence = %hx\n",
		       ranap_msg->msg.directTransferIEs.presenceMask);
		PP(ranap_msg->msg.directTransferIEs.nas_pdu);

		len = ranap_msg->msg.directTransferIEs.nas_pdu.size;
		data = ranap_msg->msg.directTransferIEs.nas_pdu.buf;

		hnb_test_nas_rx_dtap(hnb, data, len);
		return;

	case RANAP_ProcedureCode_id_SecurityModeControl:
		printf("rx SecurityModeControl: presence = %hx\n",
		       ranap_msg->msg.securityModeCommandIEs.presenceMask);

		/* Just pick the first available IP alg, don't care about
		 * encryption (yet?) */
		algs = &ranap_msg->msg.securityModeCommandIEs.integrityProtectionInformation.permittedAlgorithms;
		if (algs->list.count < 1) {
			printf("Security Mode Command: No permitted algorithms.\n");
			return;
		}
		first_alg = *algs->list.array;

		hnb_test_rx_secmode_cmd(hnb, *first_alg);
		return;

	case RANAP_ProcedureCode_id_Iu_Release:
		hnb_test_rx_iu_release(hnb);
		return;

	case RANAP_ProcedureCode_id_RAB_Assignment:
		hnb_test_rx_rab_assign_req(hnb, &ranap_msg->msg.raB_AssignmentRequestIEs);
		return;
	}
}

/* RAB Assignment Response that reports the RAB as failed. This test HNB has
 * no Iu-UP and no GTP-U, so it cannot set up a radio access bearer; answer
 * with "user plane versions not supported" so the SGSN does not wait for
 * the bearer. */
static struct msgb *gen_rab_assign_fail(uint8_t rab_id)
{
	RANAP_RAB_AssignmentResponseIEs_t ies;
	RANAP_RAB_FailedItemIEs_t item;
	RANAP_RAB_AssignmentResponse_t out;
	struct msgb *msg;
	uint8_t rab_id_buf = rab_id;
	int rc;

	memset(&ies, 0, sizeof(ies));
	memset(&item, 0, sizeof(item));
	memset(&out, 0, sizeof(out));

	item.raB_FailedItem.rAB_ID.buf = &rab_id_buf;
	item.raB_FailedItem.rAB_ID.size = 1;
	item.raB_FailedItem.rAB_ID.bits_unused = 0;
	item.raB_FailedItem.cause.present = RANAP_Cause_PR_radioNetwork;
	item.raB_FailedItem.cause.choice.radioNetwork = RANAP_CauseRadioNetwork_user_plane_versions_not_supported;

	rc = ranap_encode_rab_faileditemies(&ies.raB_FailedList, &item);
	if (rc < 0) {
		printf("ranap_encode_rab_faileditemies() failed: %d\n", rc);
		return NULL;
	}
	ies.presenceMask = RAB_ASSIGNMENTRESPONSEIES_RANAP_RAB_FAILEDLIST_PRESENT;

	rc = ranap_encode_rab_assignmentresponseies(&out, &ies);
	if (rc < 0) {
		printf("ranap_encode_rab_assignmentresponseies() failed: %d\n", rc);
		return NULL;
	}

	msg = ranap_generate_outcome(RANAP_ProcedureCode_id_RAB_Assignment,
				     RANAP_Criticality_reject,
				     &asn_DEF_RANAP_RAB_AssignmentResponse, &out);
	ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_RANAP_RAB_AssignmentResponse, &out);
	return msg;
}

void hnb_test_rx_rab_assign_req(struct hnb_test *hnb, void *_ies)
{
	RANAP_RAB_AssignmentRequestIEs_t *ies = _ies;
	uint8_t rab_id = hnb->ps.nsapi ? hnb->ps.nsapi : 5;
	struct msgb *msg;

	printf("rx RAB Assignment Request: presence = %hx, %d RAB(s) to set up or modify, %d to release\n",
	       ies->presenceMask,
	       (ies->presenceMask & RAB_ASSIGNMENTREQUESTIES_RANAP_RAB_SETUPORMODIFYLIST_PRESENT) ?
	       ies->raB_SetupOrModifyList.list.count : 0,
	       (ies->presenceMask & RAB_ASSIGNMENTREQUESTIES_RANAP_RAB_RELEASELIST_PRESENT) ?
	       ies->raB_ReleaseList.raB_ReleaseList_ies.list.count : 0);
	printf("Iu-UP is not implemented in hnb-test: answering RAB Assignment Response, RAB %u failed\n", rab_id);

	msg = gen_rab_assign_fail(rab_id);
	if (msg)
		hnb_test_tx_dt(hnb, msg);
}

void hnb_test_rua_cl_handle_ranap(void *priv, struct ranap_message_s *ranap_msg)
{
	char imsi[16];
	struct hnb_test *hnb = priv;

	printf("rx ranap_msg->procedureCode %d\n",
	       ranap_msg->procedureCode);

	switch (ranap_msg->procedureCode) {
	case RANAP_ProcedureCode_id_Paging:
		if (ranap_msg->msg.pagingIEs.permanentNAS_UE_ID.present == RANAP_PermanentNAS_UE_ID_PR_iMSI) {
			ranap_bcd_decode(imsi, sizeof(imsi),
					 ranap_msg->msg.pagingIEs.permanentNAS_UE_ID.choice.iMSI.buf,
					 ranap_msg->msg.pagingIEs.permanentNAS_UE_ID.choice.iMSI.size);
		} else imsi[0] = '\0';

		printf("rx Paging: presence=%hx  domain=%ld  IMSI=%s\n",
		       ranap_msg->msg.pagingIEs.presenceMask,
		       ranap_msg->msg.pagingIEs.cN_DomainIndicator,
		       imsi
		       );

		hnb_test_rx_paging(hnb, imsi);
		return;
	}
}
