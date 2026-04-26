/*
 * skins_config.h — controls which network skins are compiled into ganon.
 *
 * Set a value to 1 to enable, 0 to disable.
 * The build system reads this file to determine which source files to compile,
 * so a full rebuild is required after any change here.
 *
 * Note: SKIN_ENABLE_SSH also requires libssh to be built (make libssh /
 * make libssh-arm / make libssh-mips32be). If the library is missing the
 * build system will warn and skip the skin regardless of this setting.
 */

#ifndef GANON_SKINS_CONFIG_H
#define GANON_SKINS_CONFIG_H

#ifndef SKIN_ENABLE_MONOCYPHER
#define SKIN_ENABLE_MONOCYPHER  1   /* tcp-monocypher: X25519 + XChaCha20-Poly1305 (default) */
#endif
#ifndef SKIN_ENABLE_PLAIN
#define SKIN_ENABLE_PLAIN       1   /* tcp-plain: unencrypted length-prefixed frames           */
#endif
#ifndef SKIN_ENABLE_XOR
#define SKIN_ENABLE_XOR         1   /* tcp-xor: X25519 + repeating-key XOR obfuscation        */
#endif
#ifndef SKIN_ENABLE_CHACHA20
#define SKIN_ENABLE_CHACHA20    1   /* tcp-chacha20: ChaCha20-Poly1305 via libsodium/monocypher */
#endif
#ifndef SKIN_ENABLE_SSH
#define SKIN_ENABLE_SSH         1   /* tcp-ssh: SSH transport via libssh                       */
#endif
#ifndef SKIN_ENABLE_QUIC
#define SKIN_ENABLE_QUIC        0   /* udp-quic: QUIC (ngtcp2+picotls) — x64 native only       */
#endif
#ifndef SKIN_ENABLE_QUIC2
#define SKIN_ENABLE_QUIC2       1   /* udp-quic2: QUIC (ngtcp2+picotls-mbedtls) — all arches   */
#endif

#endif /* GANON_SKINS_CONFIG_H */
