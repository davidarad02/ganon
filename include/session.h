#ifndef GANON_SESSION_H
#define GANON_SESSION_H

#include "protocol.h"
#include "transport.h"

#define E__SESSION__HANDLE_PING_FAILED 0x401
#define E__SESSION__HANDLE_MESSAGE_FAILED 0x402

err_t SESSION__process(transport_t *t);

#endif /* #ifndef GANON_SESSION_H */
