/*
 * Copyright (c) 2018, OARC, Inc.
 * All rights reserved.
 *
 * This file is part of dnsjit.
 *
 * dnsjit is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * dnsjit is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with dnsjit.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "output/udpcli.h"
#include "core/assert.h"
#include "core/object/dns.h"
#include "core/object/payload.h"

#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

static core_log_t      _log      = LOG_T_INIT("output.udpcli");
static output_udpcli_t _defaults = {
    LOG_T_INIT_OBJ("output.udpcli"),
    0, 0, -1,
    { 0 }, 0,
    { 0 }, CORE_OBJECT_PAYLOAD_INIT(0)
};

core_log_t* output_udpcli_log()
{
    return &_log;
}

void output_udpcli_init(output_udpcli_t* self)
{
    mlassert_self();

    *self             = _defaults;
    self->pkt.payload = self->recvbuf;
}

void output_udpcli_destroy(output_udpcli_t* self)
{
    mlassert_self();

    if (self->fd > -1) {
        shutdown(self->fd, SHUT_RDWR);
        close(self->fd);
    }
}

int output_udpcli_connect(output_udpcli_t* self, const char* host, const char* port)
{
    struct addrinfo* addr;
    int              err;
    mlassert_self();
    lassert(host, "host is nil");
    lassert(port, "port is nil");

    if (self->fd > -1) {
        lfatal("already connected");
    }

    if ((err = getaddrinfo(host, port, 0, &addr))) {
        lcritical("getaddrinfo(%s, %s) error %s", host, port, gai_strerror(err));
        return -1;
    }
    if (!addr) {
        lcritical("getaddrinfo failed, no address returned");
        return -1;
    }

    memcpy(&self->addr, addr->ai_addr, addr->ai_addrlen);
    self->addr_len = addr->ai_addrlen;
    freeaddrinfo(addr);

    if ((self->fd = socket(((struct sockaddr*)&self->addr)->sa_family, SOCK_DGRAM, 0)) < 0) {
        lcritical("socket() error %s", core_log_errstr(errno));
        return -2;
    }

    return 0;
}

int output_udpcli_nonblocking(output_udpcli_t* self)
{
    int flags;
    mlassert_self();

    if (self->fd < 0) {
        lfatal("not connected");
    }

    flags = fcntl(self->fd, F_GETFL);
    if (flags != -1) {
        flags = flags & O_NONBLOCK ? 1 : 0;
    }

    return flags;
}

int output_udpcli_set_nonblocking(output_udpcli_t* self, int nonblocking)
{
    int flags;
    mlassert_self();

    if (self->fd < 0) {
        lfatal("not connected");
    }

    if ((flags = fcntl(self->fd, F_GETFL)) == -1) {
        lcritical("fcntl(FL_GETFL) error %s", core_log_errstr(errno));
        return -1;
    }

    if (nonblocking) {
        flags |= O_NONBLOCK;
    } else {
        flags &= ~O_NONBLOCK;
    }

    if (fcntl(self->fd, F_SETFL, flags | O_NONBLOCK)) {
        lcritical("fcntl(FL_SETFL, %x) error %s", flags, core_log_errstr(errno));
        return -1;
    }

    return 0;
}

static void _receive(void* ctx, const core_object_t* obj)
{
    output_udpcli_t* self = (output_udpcli_t*)ctx;
    const uint8_t*   payload;
    size_t           len, sent;
    mlassert_self();

    for (; obj;) {
        switch (obj->obj_type) {
        case CORE_OBJECT_DNS:
            obj = obj->obj_prev;
            continue;
        case CORE_OBJECT_PAYLOAD:
            payload = ((core_object_payload_t*)obj)->payload;
            len     = ((core_object_payload_t*)obj)->len;
            break;
        default:
            return;
        }

        if (len < 3 || payload[2] & 0x80) {
            return;
        }

        sent = 0;
        self->pkts++;
        for (;;) {
            ssize_t ret = sendto(self->fd, payload + sent, len - sent, 0, (struct sockaddr*)&self->addr, self->addr_len);
            if (ret > -1) {
                sent += ret;
                if (sent < len)
                    continue;
                return;
            }
            switch (errno) {
            case EAGAIN:
#if EAGAIN != EWOULDBLOCK
            case EWOULDBLOCK:
#endif
                continue;
            default:
                break;
            }
            self->errs++;
            break;
        }
        break;
    }
}

core_receiver_t output_udpcli_receiver(output_udpcli_t* self)
{
    mlassert_self();

    if (self->fd < 0) {
        lfatal("not connected");
    }

    return _receive;
}

static const core_object_t* _produce(void* ctx)
{
    output_udpcli_t* self = (output_udpcli_t*)ctx;
    ssize_t          n;
    mlassert_self();

    for (;;) {
        n = recvfrom(self->fd, self->recvbuf, sizeof(self->recvbuf), 0, 0, 0);
        if (n > -1) {
            break;
        }
        switch (errno) {
        case EAGAIN:
#if EAGAIN != EWOULDBLOCK
        case EWOULDBLOCK:
#endif
            n = 0;
            break;
        default:
            break;
        }
        break;
    }

    if (n < 1) {
        return 0;
    }

    self->pkt.len = n;
    return (core_object_t*)&self->pkt;
}

core_producer_t output_udpcli_producer(output_udpcli_t* self)
{
    mlassert_self();

    if (self->fd < 0) {
        lfatal("not connected");
    }

    return _produce;
}
