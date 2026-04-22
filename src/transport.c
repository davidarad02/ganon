#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>

#include "common.h"
#include "err.h"
#include "logging.h"
#include "network.h"
#include "protocol.h"
#include "transport.h"
#include "monocypher.h"

/* Stack buffer size for TRANSPORT__send_msg to avoid malloc on the hot path.
 * Must be >= TUNNEL_BUF_SIZE (65536) + tunnel header (8) + protocol header (32). */
#define TRANSPORT_SEND_STACK_SIZE 65792

/* Encryption frame overhead: 4 (length) + 24 (nonce) + 16 (MAC) = 44 bytes */
#define ENC_FRAME_OVERHEAD 44
#define ENC_NONCE_SIZE 24
#define ENC_MAC_SIZE 16

ssize_t TRANSPORT__recv(int fd, uint8_t *buf, size_t len) {
    return recv(fd, buf, len, 0);
}

ssize_t TRANSPORT__send(int fd, const uint8_t *buf, size_t len) {
    return send(fd, buf, len, 0);
}

static int get_random_bytes(uint8_t *buf, size_t len) {
#if defined(__linux__) && defined(SYS_getrandom)
    ssize_t n = getrandom(buf, len, 0);
    if (n == (ssize_t)len) {
        return 0;
    }
#endif
    static int urandom_fd = -1;
    if (urandom_fd < 0) {
        urandom_fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
        if (urandom_fd < 0) {
            return -1;
        }
    }
    size_t total = 0;
    while (total < len) {
        ssize_t n = read(urandom_fd, buf + total, len - total);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        total += (size_t)n;
    }
    return 0;
}

static void derive_keys(const uint8_t shared[32],
                        uint8_t send_key[32], uint8_t recv_key[32],
                        uint8_t session_id[8]) {
    uint8_t out[32];
    const char send_ctx = 'S';
    const char recv_ctx = 'R';
    const char sess_ctx = 'I';

    crypto_blake2b_keyed(out, 32, shared, 32, (const uint8_t *)&send_ctx, 1);
    memcpy(send_key, out, 32);

    crypto_blake2b_keyed(out, 32, shared, 32, (const uint8_t *)&recv_ctx, 1);
    memcpy(recv_key, out, 32);

    crypto_blake2b_keyed(out, 32, shared, 32, (const uint8_t *)&sess_ctx, 1);
    memcpy(session_id, out, 8);

    crypto_wipe(out, sizeof(out));
}

static void build_nonce(uint8_t nonce[24], uint64_t counter) {
    memset(nonce, 0, 24);
    /* Store counter as little-endian in last 8 bytes */
    nonce[16] = (uint8_t)(counter);
    nonce[17] = (uint8_t)(counter >> 8);
    nonce[18] = (uint8_t)(counter >> 16);
    nonce[19] = (uint8_t)(counter >> 24);
    nonce[20] = (uint8_t)(counter >> 32);
    nonce[21] = (uint8_t)(counter >> 40);
    nonce[22] = (uint8_t)(counter >> 48);
    nonce[23] = (uint8_t)(counter >> 56);
}

transport_t *TRANSPORT__create(int fd) {
    transport_t *t = malloc(sizeof(transport_t));
    if (NULL == t) {
        LOG_ERROR("Failed to allocate transport");
        return NULL;
    }

    t->fd = fd;
    t->is_incoming = 0;
    t->client_ip[0] = '\0';
    t->client_port = 0;
    t->node_id = 0;
    t->ctx = NULL;
    t->recv = TRANSPORT__recv;
    t->send = TRANSPORT__send;

    t->enc_state = ENC__INIT;
    memset(t->enc_ephemeral_priv, 0, 32);
    memset(t->enc_ephemeral_pub, 0, 32);
    memset(t->enc_send_key, 0, 32);
    memset(t->enc_recv_key, 0, 32);
    memset(t->enc_session_id, 0, 8);
    t->enc_send_nonce = 0;
    t->enc_recv_nonce = 0;
    t->enc_is_initiator = 0;

    return t;
}

void TRANSPORT__destroy(transport_t *t) {
    if (NULL == t) {
        return;
    }
    crypto_wipe(t->enc_ephemeral_priv, 32);
    crypto_wipe(t->enc_send_key, 32);
    crypto_wipe(t->enc_recv_key, 32);
    if (t->fd >= 0) {
        shutdown(t->fd, SHUT_RDWR);
        close(t->fd);
        t->fd = -1;
    }
    FREE(t);
}

int TRANSPORT__get_fd(transport_t *t) {
    if (NULL == t) {
        return -1;
    }
    return t->fd;
}

uint32_t TRANSPORT__get_node_id(transport_t *t) {
    if (NULL == t) {
        return 0;
    }
    return t->node_id;
}

void TRANSPORT__set_node_id(transport_t *t, uint32_t node_id) {
    if (NULL == t) {
        return;
    }
    t->node_id = node_id;
}

err_t TRANSPORT__recv_all(transport_t *t, uint8_t *buf, size_t len, ssize_t *bytes_read) {
    err_t rc = E__SUCCESS;

    VALIDATE_ARGS(t, buf, bytes_read);

    *bytes_read = 0;

    size_t total_read = 0;
    while (total_read < len) {
        ssize_t n = t->recv(t->fd, buf + total_read, len - total_read);
        if (0 > n) {
            if (EAGAIN == errno || EWOULDBLOCK == errno) {
                continue;
            }
            LOG_WARNING("recv failed on fd %d: %s", t->fd, strerror(errno));
            FAIL(E__NET__SOCKET_CONNECT_FAILED);
        } else if (0 == n) {
            LOG_WARNING("Socket disconnected (fd=%d)", t->fd);
            FAIL(E__NET__SOCKET_CONNECT_FAILED);
        }
        total_read += (size_t)n;
    }

    *bytes_read = (ssize_t)total_read;

l_cleanup:
    return rc;
}

err_t TRANSPORT__send_all(transport_t *t, const uint8_t *buf, size_t len, ssize_t *bytes_sent) {
    err_t rc = E__SUCCESS;

    if (NULL == t || NULL == buf) {
        rc = E__INVALID_ARG_NULL_POINTER;
        goto l_cleanup;
    }

    if (NULL != bytes_sent) {
        *bytes_sent = 0;
    }

    size_t total_sent = 0;
    while (total_sent < len) {
        ssize_t n = t->send(t->fd, buf + total_sent, len - total_sent);
        if (0 > n) {
            if (EAGAIN == errno || EWOULDBLOCK == errno) {
                continue;
            }
            LOG_WARNING("send failed on fd %d: %s", t->fd, strerror(errno));
            FAIL(E__NET__SOCKET_CONNECT_FAILED);
        }
        total_sent += (size_t)n;
    }

    if (NULL != bytes_sent) {
        *bytes_sent = (ssize_t)total_sent;
    }

l_cleanup:
    return rc;
}

err_t TRANSPORT__do_handshake(IN transport_t *t, IN int is_initiator) {
    err_t rc = E__SUCCESS;
    uint8_t peer_pub[32];
    uint8_t shared[32];
    ssize_t dummy;

    VALIDATE_ARGS(t);

    if (0 != get_random_bytes(t->enc_ephemeral_priv, 32)) {
        LOG_ERROR("Failed to generate random bytes for keypair");
        FAIL(E__CRYPTO__HANDSHAKE_FAILED);
    }

    crypto_x25519_public_key(t->enc_ephemeral_pub, t->enc_ephemeral_priv);
    t->enc_is_initiator = is_initiator;

    if (is_initiator) {
        /* Send our pubkey */
        rc = TRANSPORT__send_all(t, t->enc_ephemeral_pub, 32, NULL);
        FAIL_IF(E__SUCCESS != rc, rc);

        /* Receive peer pubkey */
        rc = TRANSPORT__recv_all(t, peer_pub, 32, &dummy);
        FAIL_IF(E__SUCCESS != rc, rc);
    } else {
        /* Receive peer pubkey */
        rc = TRANSPORT__recv_all(t, peer_pub, 32, &dummy);
        FAIL_IF(E__SUCCESS != rc, rc);

        /* Send our pubkey */
        rc = TRANSPORT__send_all(t, t->enc_ephemeral_pub, 32, NULL);
        FAIL_IF(E__SUCCESS != rc, rc);
    }

    crypto_x25519(shared, t->enc_ephemeral_priv, peer_pub);

    if (is_initiator) {
        derive_keys(shared, t->enc_send_key, t->enc_recv_key, t->enc_session_id);
    } else {
        derive_keys(shared, t->enc_recv_key, t->enc_send_key, t->enc_session_id);
    }

    t->enc_send_nonce = 0;
    t->enc_recv_nonce = 0;
    t->enc_state = ENC__ESTABLISHED;

    /* Precompute XChaCha20 subkeys.  Our nonces always have 16 zero bytes
     * in the first half, so crypto_chacha20_h() produces the same subkey
     * for every message.  Caching it avoids 20 ChaCha20 rounds per frame. */
    {
        uint8_t zero_nonce_prefix[16] = {0};
        crypto_chacha20_h(t->enc_send_subkey, t->enc_send_key, zero_nonce_prefix);
        crypto_chacha20_h(t->enc_recv_subkey, t->enc_recv_key, zero_nonce_prefix);
    }

    LOG_DEBUG("Encryption handshake complete on fd=%d (session_id=%02x%02x%02x%02x)",
              t->fd, t->enc_session_id[0], t->enc_session_id[1],
              t->enc_session_id[2], t->enc_session_id[3]);

l_cleanup:
    crypto_wipe(shared, sizeof(shared));
    return rc;
}

err_t TRANSPORT__recv_msg(transport_t *t, protocol_msg_t *msg, uint8_t **data) {
    err_t rc = E__SUCCESS;
    uint8_t len_buf[4];
    uint32_t frame_len = 0;
    uint8_t *frame = NULL;
    uint8_t *plaintext = NULL;
    ssize_t dummy;

    VALIDATE_ARGS(t, msg, data);

    *data = NULL;

    /* Read 4-byte length prefix */
    rc = TRANSPORT__recv_all(t, len_buf, 4, &dummy);
    FAIL_IF(E__SUCCESS != rc, rc);

    frame_len = ((uint32_t)len_buf[0] << 24) |
                ((uint32_t)len_buf[1] << 16) |
                ((uint32_t)len_buf[2] <<  8) |
                ((uint32_t)len_buf[3]);

    /* frame_len is payload after length prefix: nonce(24) + mac(16) + ciphertext(N) */
    if (frame_len < (ENC_NONCE_SIZE + ENC_MAC_SIZE) || frame_len > 200000) {
        LOG_WARNING("Invalid encrypted frame length: %u on fd=%d", frame_len, t->fd);
        FAIL(E__NET__SOCKET_CONNECT_FAILED);
    }

    frame = malloc(frame_len);
    if (NULL == frame) {
        FAIL(E__INVALID_ARG_NULL_POINTER);
    }

    rc = TRANSPORT__recv_all(t, frame, frame_len, &dummy);
    if (E__SUCCESS != rc) {
        FREE(frame);
        goto l_cleanup;
    }

    /* Decrypt frame: nonce(24) || mac(16) || ciphertext(N) */
    uint8_t *nonce = frame;
    uint8_t *mac = frame + ENC_NONCE_SIZE;
    uint8_t *ciphertext = frame + ENC_NONCE_SIZE + ENC_MAC_SIZE;
    size_t ciphertext_len = frame_len - ENC_NONCE_SIZE - ENC_MAC_SIZE;

    /* Verify nonce: must exactly match expected counter */
    uint64_t recv_counter = (uint64_t)nonce[16] |
                           ((uint64_t)nonce[17] << 8) |
                           ((uint64_t)nonce[18] << 16) |
                           ((uint64_t)nonce[19] << 24) |
                           ((uint64_t)nonce[20] << 32) |
                           ((uint64_t)nonce[21] << 40) |
                           ((uint64_t)nonce[22] << 48) |
                           ((uint64_t)nonce[23] << 56);

    if (recv_counter != t->enc_recv_nonce) {
        LOG_WARNING("Replay detected on fd=%d: expected nonce %llu, got %llu",
                    t->fd, (unsigned long long)t->enc_recv_nonce,
                    (unsigned long long)recv_counter);
        FREE(frame);
        FAIL(E__CRYPTO__REPLAY_DETECTED);
    }
    t->enc_recv_nonce++;

    plaintext = malloc(ciphertext_len);
    if (NULL == plaintext) {
        FREE(frame);
        FAIL(E__INVALID_ARG_NULL_POINTER);
    }

    /* Use a local DJB context with the cached subkey.  This is exactly
     * what crypto_aead_unlock() does internally, minus the redundant
     * crypto_chacha20_h() call (our nonce prefix is always 16 zeros). */
    {
        crypto_aead_ctx ctx;
        memcpy(ctx.key, t->enc_recv_subkey, 32);
        memcpy(ctx.nonce, nonce + 16, 8);
        ctx.counter = 0;
        if (0 != crypto_aead_read(&ctx, plaintext, mac,
                                  NULL, 0, ciphertext, ciphertext_len)) {
            LOG_WARNING("Decryption failed on fd=%d", t->fd);
            FREE(frame);
            FREE(plaintext);
            FAIL(E__CRYPTO__DECRYPT_FAILED);
        }
    }

    FREE(frame);

    size_t unserialize_data_len = 0;
    rc = PROTOCOL__unserialize(plaintext, ciphertext_len, msg, data, &unserialize_data_len);
    if (E__SUCCESS != rc) {
        LOG_WARNING("Failed to unserialize decrypted message from fd %d", t->fd);
        FREE(plaintext);
        goto l_cleanup;
    }

    LOG_TRACE("RECV msg: orig_src=%u, src=%u, dst=%u, msg_id=%u, type=%u, data_len=%u, ttl=%u, channel=%u, fd=%d",
              msg->orig_src_node_id, msg->src_node_id, msg->dst_node_id,
              msg->message_id, msg->type, msg->data_length, msg->ttl, msg->channel_id, t->fd);

    FREE(plaintext);
l_cleanup:
    return rc;
}

err_t TRANSPORT__send_msg(transport_t *t, const protocol_msg_t *msg, const uint8_t *data) {
    err_t rc = E__SUCCESS;
    uint8_t *frame = NULL;

    VALIDATE_ARGS(t, msg);

    LOG_TRACE("SEND msg: orig_src=%u, src=%u, dst=%u, msg_id=%u, type=%u, data_len=%u, ttl=%u, channel=%u, fd=%d",
              msg->orig_src_node_id, msg->src_node_id, msg->dst_node_id,
              msg->message_id, msg->type, msg->data_length, msg->ttl, msg->channel_id, t->fd);

    size_t plain_len = PROTOCOL_HEADER_SIZE;
    if (NULL != data && msg->data_length > 0) {
        plain_len += msg->data_length;
    }

    uint8_t *plain_buf;
    uint8_t plain_stack[TRANSPORT_SEND_STACK_SIZE];
    int plain_heap = (plain_len > TRANSPORT_SEND_STACK_SIZE);
    if (plain_heap) {
        plain_buf = malloc(plain_len);
        if (NULL == plain_buf) {
            FAIL(E__INVALID_ARG_NULL_POINTER);
        }
    } else {
        plain_buf = plain_stack;
    }

    size_t bytes_written = 0;
    rc = PROTOCOL__serialize(msg, data, plain_buf, plain_len, &bytes_written);
    if (E__SUCCESS != rc) {
        if (plain_heap) FREE(plain_buf);
        goto l_cleanup;
    }

    /* Encrypt into frame: length(4) || nonce(24) || mac(16) || ciphertext(N) */
    size_t ciphertext_len = bytes_written;
    size_t payload_len = ENC_NONCE_SIZE + ENC_MAC_SIZE + ciphertext_len;
    size_t total_len = 4 + payload_len;

    frame = malloc(total_len);
    if (NULL == frame) {
        if (plain_heap) FREE(plain_buf);
        FAIL(E__INVALID_ARG_NULL_POINTER);
    }

    uint32_t net_len = htonl((uint32_t)payload_len);
    memcpy(frame, &net_len, 4);

    uint8_t *nonce = frame + 4;
    uint8_t *mac = frame + 4 + ENC_NONCE_SIZE;
    uint8_t *ciphertext = frame + 4 + ENC_NONCE_SIZE + ENC_MAC_SIZE;

    build_nonce(nonce, t->enc_send_nonce);
    t->enc_send_nonce++;

    /* Use a local DJB context with the cached subkey.  This is exactly
     * what crypto_aead_lock() does internally, minus the redundant
     * crypto_chacha20_h() call (our nonce prefix is always 16 zeros). */
    {
        crypto_aead_ctx ctx;
        memcpy(ctx.key, t->enc_send_subkey, 32);
        memcpy(ctx.nonce, nonce + 16, 8);
        ctx.counter = 0;
        crypto_aead_write(&ctx, ciphertext, mac, NULL, 0,
                          plain_buf, bytes_written);
    }

    if (plain_heap) FREE(plain_buf);

    rc = TRANSPORT__send_all(t, frame, total_len, NULL);
    FREE(frame);
    FAIL_IF(E__SUCCESS != rc, rc);

l_cleanup:
    return rc;
}

err_t TRANSPORT__send_to_node_id(network_t *net, uint32_t node_id, const protocol_msg_t *msg, const uint8_t *data) {
    if (NULL == net || NULL == msg) {
        return E__INVALID_ARG_NULL_POINTER;
    }

    transport_t *t = NETWORK__get_transport(net, node_id);
    if (NULL == t) {
        LOG_WARNING("TRANSPORT__send_to_node_id: no transport for node_id=%u", node_id);
        return E__NET__SOCKET_CONNECT_FAILED;
    }

    return TRANSPORT__send_msg(t, msg, data);
}
