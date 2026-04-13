#ifndef GANON_TRANSPORT_H
#define GANON_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

typedef struct transport transport_t;

struct transport {
    int fd;
    ssize_t (*recv)(int fd, uint8_t *buf, size_t len);
    ssize_t (*send)(int fd, const uint8_t *buf, size_t len);
};

ssize_t TRANSPORT__recv(int fd, uint8_t *buf, size_t len);
ssize_t TRANSPORT__send(int fd, const uint8_t *buf, size_t len);

transport_t *TRANSPORT__create(int fd);
void TRANSPORT__destroy(transport_t *t);

ssize_t TRANSPORT__recv_all(transport_t *t, uint8_t *buf, size_t len);
ssize_t TRANSPORT__send_all(transport_t *t, const uint8_t *buf, size_t len);

#endif /* #ifndef GANON_TRANSPORT_H */
