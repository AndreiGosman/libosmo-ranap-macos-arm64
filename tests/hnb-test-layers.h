#pragma once

struct ANY;
struct ranap_message_s;
struct hnb_test;

/* main calls RUA */
void hnb_test_rua_dt_handle(struct hnb_test *hnb, struct ANY *in);
void hnb_test_rua_cl_handle(struct hnb_test *hnb, struct ANY *in);
void hnb_test_rua_disc_handle(struct hnb_test *hnb, struct ANY *in);

/* RUA calls RANAP */
void hnb_test_rua_dt_handle_ranap(void *priv, struct ranap_message_s *ranap_msg);
void hnb_test_rua_cl_handle_ranap(void *priv, struct ranap_message_s *ranap_msg);

/* RANAP calls main with actual payload*/
void hnb_test_nas_rx_dtap(struct hnb_test *hnb, void *data, int len);
void hnb_test_rx_secmode_cmd(struct hnb_test *hnb, long ip_alg);
void hnb_test_rx_iu_release(struct hnb_test *hnb);
void hnb_test_rx_paging(struct hnb_test *hnb, const char *imsi);

/* authentication answer shared by the MM and GMM handlers (hnb-test.c) */
extern const uint8_t hnb_test_subscr_key[16];
int hnb_test_auth_answer(const uint8_t *rand, const uint8_t *autn, uint8_t *res, size_t res_size);

/* hnb-test.c */
struct msgb;
int hnb_test_tx_dt(struct hnb_test *hnb, struct msgb *txm);

/* hnb-test-gmm.c */
struct gsm48_hdr;
struct hnb_test_tv_len;
void hnb_test_for_each_ie(const uint8_t *p, int len, const struct hnb_test_tv_len *tv_len,
			  void (*cb)(uint8_t iei, const uint8_t *val, uint8_t vlen, void *priv), void *priv);
int hnb_test_gen_gmm_attach_req(uint8_t *buf, size_t size, const char *imsi);
int hnb_test_nas_rx_gmm(struct hnb_test *hnb, struct gsm48_hdr *gh, int len);
