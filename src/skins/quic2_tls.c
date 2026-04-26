/*
 * quic2 TLS helper — generates a self-signed ECDSA P-256 certificate in
 * memory using mbedTLS and wires it into a picotls server context.
 *
 * Called once from quic2 listener_create; never on the hot path.
 */

#include "quic2_tls.h"

#include <mbedtls/ecp.h>
#include <mbedtls/x509_crt.h>
#include <string.h>
#include <stdlib.h>

/* ── Signing callback for picotls ─────────────────────────────────────── */

static int quic2_sign_cb(ptls_sign_certificate_t *_self,
                          ptls_t *tls, ptls_async_job_t **async,
                          uint16_t *selected_algo,
                          ptls_buffer_t *outbuf, ptls_iovec_t input,
                          const uint16_t *algorithms, size_t num_algorithms) {
    quic2_sign_cert_t *self = (quic2_sign_cert_t *)_self;
    size_t i;
    int ret;
    int found = 0;

    (void)tls;
    (void)async;

    for (i = 0; i < num_algorithms; i++) {
        if (algorithms[i] == PTLS_SIGNATURE_ECDSA_SECP256R1_SHA256) {
            found = 1;
            break;
        }
    }
    if (!found) return PTLS_ALERT_HANDSHAKE_FAILURE;

    *selected_algo = PTLS_SIGNATURE_ECDSA_SECP256R1_SHA256;

    size_t sig_max = MBEDTLS_PK_SIGNATURE_MAX_SIZE;
    if ((ret = ptls_buffer_reserve(outbuf, sig_max)) != 0) return ret;

    mbedtls_entropy_context  entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy, NULL, 0);

    unsigned char hash[32];
    size_t hash_len;
    psa_status_t psa_rc = psa_hash_compute(PSA_ALG_SHA_256,
                                           input.base, input.len,
                                           hash, sizeof(hash), &hash_len);
    if (PSA_SUCCESS != psa_rc) {
        ret = PTLS_ERROR_LIBRARY;
        goto done;
    }

    size_t sig_len;
    ret = mbedtls_pk_sign(self->key, MBEDTLS_MD_SHA256,
                          hash, hash_len,
                          outbuf->base + outbuf->off, sig_max, &sig_len,
                          mbedtls_ctr_drbg_random, &ctr_drbg);
    if (ret == 0) {
        outbuf->off += sig_len;
    } else {
        ret = PTLS_ERROR_LIBRARY;
    }

done:
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    return ret;
}

/* ── Public entry point ────────────────────────────────────────────────── */

int quic2_tls_listener_init(ptls_context_t          *tls_ctx,
                             quic2_sign_cert_t       *sign_cert,
                             mbedtls_pk_context      *pkey,
                             ptls_key_exchange_algorithm_t **key_ex,
                             ptls_cipher_suite_t           **ciphers,
                             uint8_t                **cert_der,
                             size_t                  *cert_der_len,
                             ptls_iovec_t            *cert_iov) {
    int ret = -1;
    mbedtls_x509write_cert   crt;
    mbedtls_entropy_context  entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    const char *pers = "ganon_quic2_gen_cert";
    unsigned char cert_buf[4096];
    int cert_len;

    mbedtls_x509write_crt_init(&crt);
    mbedtls_pk_init(pkey);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);

    if ((ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                     (const unsigned char *)pers,
                                     strlen(pers))) != 0)
        goto cleanup;

    if ((ret = mbedtls_pk_setup(pkey,
                                mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY))) != 0)
        goto cleanup;
    if ((ret = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1,
                                   mbedtls_pk_ec(*pkey),
                                   mbedtls_ctr_drbg_random, &ctr_drbg)) != 0)
        goto cleanup;

    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    {
        unsigned char serial[] = { 0x01 };
        mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial));
    }
    mbedtls_x509write_crt_set_validity(&crt, "20200101000000", "20501231235959");
    mbedtls_x509write_crt_set_subject_name(&crt, "CN=ganon-q2");
    mbedtls_x509write_crt_set_issuer_name(&crt,  "CN=ganon-q2");
    mbedtls_x509write_crt_set_subject_key(&crt, pkey);
    mbedtls_x509write_crt_set_issuer_key(&crt,  pkey);

    cert_len = mbedtls_x509write_crt_der(&crt, cert_buf, sizeof(cert_buf),
                                         mbedtls_ctr_drbg_random, &ctr_drbg);
    if (cert_len < 0) { ret = cert_len; goto cleanup; }

    /* mbedtls_x509write_crt_der writes at END of buffer — copy to fresh alloc */
    *cert_der = malloc((size_t)cert_len);
    if (*cert_der == NULL) { ret = -1; goto cleanup; }
    memcpy(*cert_der,
           cert_buf + sizeof(cert_buf) - (size_t)cert_len,
           (size_t)cert_len);
    *cert_der_len  = (size_t)cert_len;
    cert_iov->base = *cert_der;
    cert_iov->len  = *cert_der_len;

    /* Wire up signing callback */
    sign_cert->super.cb = quic2_sign_cb;
    sign_cert->key      = pkey;

    /* picotls TLS context */
    key_ex[0] = &ptls_mbedtls_secp256r1;
    key_ex[1] = NULL;
    ciphers[0] = &ptls_mbedtls_aes128gcmsha256;
    ciphers[1] = &ptls_mbedtls_aes256gcmsha384;
    ciphers[2] = NULL;

    memset(tls_ctx, 0, sizeof(*tls_ctx));
    tls_ctx->random_bytes       = ptls_mbedtls_random_bytes;
    tls_ctx->get_time           = &ptls_get_time;
    tls_ctx->key_exchanges      = key_ex;
    tls_ctx->cipher_suites      = ciphers;
    tls_ctx->certificates.list  = cert_iov;
    tls_ctx->certificates.count = 1;
    tls_ctx->sign_certificate   = &sign_cert->super;

    ngtcp2_crypto_picotls_configure_server_context(tls_ctx);

    ret = 0;

cleanup:
    mbedtls_x509write_crt_free(&crt);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    return ret;
}
