/*
 * Copyright (c) 2019, CZ.NIC, z.s.p.o.
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

#include "output/dnssim.h"
#include "core/assert.h"
#include "core/object/ip.h"
#include "core/object/ip6.h"
#include "core/object/payload.h"
#include "core/object/dns.h"

typedef struct _output_dnssim_source _output_dnssim_source_t;
struct _output_dnssim_source {
    _output_dnssim_source_t* next;
    struct sockaddr_storage addr;
};

typedef struct _output_dnssim {
    output_dnssim_t pub;

    output_dnssim_transport_t transport;
    uv_loop_t loop;
    struct sockaddr_storage target;
    _output_dnssim_source_t* source;

    uv_timer_t stat_timer;

    void (*create_request)(output_dnssim_t*, output_dnssim_client_t*,
        core_object_payload_t*);
} _output_dnssim_t;

typedef struct _output_dnssim_query _output_dnssim_query_t;
struct _output_dnssim_query {
    _output_dnssim_query_t* qry_prev;
    output_dnssim_transport_t transport;
};

typedef struct _output_dnssim_query_udp {
    _output_dnssim_query_t qry;
    uv_udp_t* handle;
    uv_buf_t buf;
    //uv_timer_t* qry_retransmit;
} _output_dnssim_query_udp_t;

typedef struct _output_dnssim_request {
    _output_dnssim_query_t* qry;
    output_dnssim_client_t* client;
    core_object_payload_t* payload;
    core_object_dns_t* dns_q;
    uv_timer_t* timeout;
    uint8_t timeout_closing;
    output_dnssim_t* dnssim;
} _output_dnssim_request_t;

static core_log_t _log = LOG_T_INIT("output.dnssim");
static output_dnssim_t _defaults = {
    LOG_T_INIT_OBJ("output.dnssim"),
    0, 0, 0,
    NULL, NULL, NULL,
    0, 0, 0,
    2000
};
static output_dnssim_client_t _client_defaults = {
    0, 0, 0,
    0.0, 0.0, 0.0
};
static output_dnssim_stats_t _stats_defaults = {
    NULL, NULL,
    0, 0, 0
};

// forward declarations
static void _close_query_udp(_output_dnssim_query_udp_t* qry);
static void _close_request_timeout_cb(uv_handle_t* handle);
static void _close_request_timeout(uv_timer_t* handle);

core_log_t* output_dnssim_log()
{
    return &_log;
}

#define _self ((_output_dnssim_t*)self)
#define _ERR_MALFORMED -2
#define _ERR_MSGID -3
#define _ERR_TC -4


/*** request/query ***/
static void _maybe_free_request(_output_dnssim_request_t* req)
{
    if (req->qry == NULL && req->timeout == NULL) {
        if (req->dnssim->free_after_use) {
            core_object_payload_free(req->payload);
            mldebug("payload freed");
        }
        core_object_dns_free(req->dns_q);
        free(req);
        mldebug("req freed");
    }
}

static void _close_query(_output_dnssim_query_t* qry)
{
    switch(qry->transport) {
    case OUTPUT_DNSSIM_TRANSPORT_UDP:
        _close_query_udp((_output_dnssim_query_udp_t*)qry);
        break;
    default:
        mlnotice("failed to close query: unsupported transport");
        break;
    }
}

static void _close_request(_output_dnssim_request_t* req)
{
    if (req == NULL) {
        return;
    }
    if (req->timeout != NULL) {
        _close_request_timeout(req->timeout);
    }
    // finish any ongoing queries
    _output_dnssim_query_t* qry = req->qry;
    while (qry != NULL) {
        _close_query(qry);
        qry = qry->qry_prev;
    }
    _maybe_free_request(req);
}

static void _close_request_timeout_cb(uv_handle_t* handle)
{
    _output_dnssim_request_t* req = (_output_dnssim_request_t*)handle->data;
    free(handle);
    mldebug("req timer freed");
    req->timeout = NULL;
    _close_request(req);
}

static void _close_request_timeout(uv_timer_t* handle)
{
    _output_dnssim_request_t* req = (_output_dnssim_request_t*)handle->data;

    if (!req->timeout_closing) {
        req->timeout_closing = 1;
        uv_timer_stop(handle);
        uv_close((uv_handle_t*)handle, _close_request_timeout_cb);
    }
}


/*** UDP dnssim ***/
static int _process_udp_response(uv_udp_t* handle, ssize_t nread, const uv_buf_t* buf)
{
    _output_dnssim_request_t* req = (_output_dnssim_request_t*)handle->data;
    core_object_payload_t payload = CORE_OBJECT_PAYLOAD_INIT(NULL);
    core_object_dns_t dns_a = CORE_OBJECT_DNS_INIT(&payload);

    payload.payload = buf->base;
    payload.len = nread;

    dns_a.obj_prev = (core_object_t*)&payload;
    int ret = core_object_dns_parse_header(&dns_a);
    if (ret != 0) {
        mldebug("udp response malformed");
        return _ERR_MALFORMED;
    }
    if (dns_a.id != req->dns_q->id) {
        mldebug("udp response msgid mismatch %x(q) != %x(a)", req->dns_q->id, dns_a.id);
        return _ERR_MSGID;
    }
    if (dns_a.tc == 1) {
        mldebug("udp response has TC=1");
        return _ERR_TC;
    }

    req->client->req_answered++;
    req->dnssim->stats_sum->answered++;
    req->dnssim->stats_current->answered++;
    if (dns_a.rcode == CORE_OBJECT_DNS_RCODE_NOERROR) {
        req->client->req_noerror++;
        req->dnssim->stats_sum->noerror++;
        req->dnssim->stats_current->noerror++;
    }

    _close_request(req);
    return 0;
}

static void _query_udp_alloc_cb(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf)
{
    mlfatal_oom(buf->base = malloc(suggested_size));
    buf->len = suggested_size;
}

static void _query_udp_recv_cb(uv_udp_t* handle, ssize_t nread, const uv_buf_t* buf,
    const struct sockaddr* addr, unsigned flags)
{
    if (nread > 0) {
        mldebug("udp recv: %d", nread);

        // TODO handle TC=1
        _process_udp_response(handle, nread, buf);
    }

    if (buf->base != NULL) {
        free(buf->base);
    }
}

static void _close_query_udp_cb(uv_handle_t* handle)
{
    _output_dnssim_request_t* req = (_output_dnssim_request_t*)handle->data;
    _output_dnssim_query_t* qry = req->qry;
    _output_dnssim_query_t* parent_qry = req->qry;
    _output_dnssim_query_udp_t* udp_qry;

    req->dnssim->ongoing--;
    for (;;) {  // find the query the handle belongs to
        if (qry->transport == OUTPUT_DNSSIM_TRANSPORT_UDP) {
            udp_qry = (_output_dnssim_query_udp_t*)qry;
            if (udp_qry->handle == (uv_udp_t*)handle) {
                free(udp_qry->handle);

                // free and remove from query list
                if (req->qry == qry) {
                    req->qry = qry->qry_prev;
                    _maybe_free_request(req);
                } else {
                    parent_qry->qry_prev = qry->qry_prev;
                }
                free(qry);
                mldebug("freed udp query %p", qry);
                return;
            }
        }
        if (qry->qry_prev == NULL) {
            mlwarning("failed to free udp_query memory");
            return;
        }
        parent_qry = qry;
        qry = qry->qry_prev;
    }
}

static void _close_query_udp(_output_dnssim_query_udp_t* qry)
{
    int ret;

    ret = uv_udp_recv_stop(qry->handle);
    if (ret < 0) {
        mldebug("failed uv_udp_recv_stop(): %s", uv_strerror(ret));
    }

    uv_close((uv_handle_t*)qry->handle, _close_query_udp_cb);
}

static int _create_query_udp(output_dnssim_t* self, _output_dnssim_request_t* req)
{
    mlassert_self();

    int ret;
    _output_dnssim_query_udp_t* qry;
    core_object_payload_t* payload = (core_object_payload_t*)req->dns_q->obj_prev;

    lfatal_oom(qry = malloc(sizeof(_output_dnssim_query_udp_t)));
    lfatal_oom(qry->handle = malloc(sizeof(uv_udp_t)));

    qry->qry.transport = OUTPUT_DNSSIM_TRANSPORT_UDP;
    qry->qry.qry_prev = req->qry;
    qry->buf = uv_buf_init((char*)payload->payload, payload->len);
    ret = uv_udp_init(&_self->loop, qry->handle);
    if (ret < 0) {
        lwarning("failed to init uv_udp_t");
        goto failure;
    }
    qry->handle->data = (void*)req;
    req->qry = (_output_dnssim_query_t*)qry;

    // bind to IP address
    if (_self->source != NULL) {
        ret = uv_udp_bind(qry->handle, (struct sockaddr*)&_self->source->addr, 0);
        if (ret < 0) {
            lwarning("failed to bind to address: %s", uv_strerror(ret));
            return ret;
        }
        _self->source = _self->source->next;
    }

    ret = uv_udp_try_send(qry->handle, &qry->buf, 1, (struct sockaddr*)&_self->target);
    if (ret < 0) {
        lwarning("failed to send udp packet: %s", uv_strerror(ret));
        return ret;
    }

    // TODO IPv4
    struct sockaddr_in6 src;
    int addr_len = sizeof(src);
    uv_udp_getsockname(qry->handle, (struct sockaddr*)&src, &addr_len);
    ldebug("sent udp from port: %d", ntohs(src.sin6_port));

    // listen for reply
    ret = uv_udp_recv_start(qry->handle, _query_udp_alloc_cb, _query_udp_recv_cb);
    if (ret < 0) {
        lwarning("failed uv_udp_recv_start(): %s", uv_strerror(ret));
        return ret;
    }

    req->dnssim->ongoing++;

    return 0;
failure:
    free(qry->handle);
    free(qry);
    return ret;
}

static void _create_request_udp(output_dnssim_t* self, output_dnssim_client_t* client,
    core_object_payload_t* payload)
{
    mlassert_self();

    int ret;
    _output_dnssim_request_t* req;

    lfatal_oom(req = malloc(sizeof(_output_dnssim_request_t)));
    memset(req, 0, sizeof(_output_dnssim_request_t));
    req->dnssim = self;
    req->client = client;
    req->payload = payload;
    req->dns_q = core_object_dns_new();
    req->dns_q->obj_prev = (core_object_t*)req->payload;

    ret = core_object_dns_parse_header(req->dns_q);
    if (ret != 0) {
        ldebug("discarded malformed dns query: couldn't parse header");
        goto failure;
    }

    req->client->req_total++;
    req->dnssim->stats_sum->total++;
    req->dnssim->stats_current->total++;

    ret = _create_query_udp(self, req);
    if (ret < 0) {
        goto failure;
    }

    lfatal_oom(req->timeout = malloc(sizeof(uv_timer_t)));
    ret = uv_timer_init(&_self->loop, req->timeout);
    req->timeout->data = req;
    if (ret < 0) {
        ldebug("failed uv_timer_init(): %s", uv_strerror(ret));
        free(req->timeout);
        req->timeout = NULL;
        goto failure;
    }
    ret = uv_timer_start(req->timeout, _close_request_timeout, self->timeout_ms, 0);
    if (ret < 0) {
        ldebug("failed uv_timer_start(): %s", uv_strerror(ret));
        goto failure;
    }

    return;
failure:
    self->discarded++;
    _close_request(req);
    return;
}

/*** dnssim functions ***/
output_dnssim_t* output_dnssim_new(size_t max_clients)
{
    output_dnssim_t* self;
    int ret;

    mlfatal_oom(self = malloc(sizeof(_output_dnssim_t)));
    *self = _defaults;

    lfatal_oom(self->stats_sum = malloc(sizeof(output_dnssim_stats_t)));
    lfatal_oom(self->stats_current = malloc(sizeof(output_dnssim_stats_t)));
    self->stats_first = self->stats_current;

    _self->source = NULL;
    _self->transport = OUTPUT_DNSSIM_TRANSPORT_UDP_ONLY;
    _self->create_request = _create_request_udp;

    lfatal_oom(self->client_arr = calloc(
        max_clients, sizeof(output_dnssim_client_t)));
    for (int i = 0; i < self->max_clients; i++) {
        *self->client_arr = _client_defaults;
    }
    self->max_clients = max_clients;

    ret = uv_loop_init(&_self->loop);
    if (ret < 0) {
        lfatal("failed to initialize uv_loop (%s)", uv_strerror(ret));
    }
    ldebug("initialized uv_loop");

    return self;
}

void output_dnssim_free(output_dnssim_t* self)
{
    mlassert_self();
    int ret;
    _output_dnssim_source_t* source;
    _output_dnssim_source_t* first = _self->source;
    output_dnssim_stats_t* stats_prev;

    free(self->stats_sum);
    do {
        stats_prev = self->stats_current->prev;
        free(self->stats_current);
        self->stats_current = stats_prev;
    } while (self->stats_current != NULL);

    if (_self->source != NULL) {
        // free cilcular linked list
        do {
            source = _self->source->next;
            free(_self->source);
            _self->source = source;
        } while (_self->source != first);
    }

    free(self->client_arr);

    ret = uv_loop_close(&_self->loop);
    if (ret < 0) {
        lcritical("failed to close uv_loop (%s)", uv_strerror(ret));
    } else {
        ldebug("closed uv_loop");
    }

    free(self);
}

uint32_t _extract_client(const core_object_t* obj) {
    uint32_t client;
    uint8_t* ip;

    switch (obj->obj_type) {
    case CORE_OBJECT_IP:
        ip = ((core_object_ip_t*)obj)->dst;
        break;
    case CORE_OBJECT_IP6:
        ip = ((core_object_ip6_t*)obj)->dst;
        break;
    default:
        return -1;
    }

    memcpy(&client, ip, sizeof(client));
    return client;
}

static void _receive(output_dnssim_t* self, const core_object_t* obj)
{
    mlassert_self();
    core_object_t* current = (core_object_t*)obj;
    core_object_payload_t* payload;
    uint32_t client;

    self->processed++;

    /* get payload from packet */
    for (;;) {
        if (current->obj_type == CORE_OBJECT_PAYLOAD) {
            payload = (core_object_payload_t*)current;
            break;
        }
        if (current->obj_prev == NULL) {
            self->discarded++;
            lwarning("packet discarded (missing payload object)");
            return;
        }
        current = (core_object_t*)current->obj_prev;
    }

    /* extract client information from IP/IP6 layer */
    for (;;) {
        if (current->obj_type == CORE_OBJECT_IP || current->obj_type == CORE_OBJECT_IP6) {
            client = _extract_client(current);
            break;
        }
        if (current->obj_prev == NULL) {
            self->discarded++;
            lwarning("packet discarded (missing ip/ip6 object)");
            return;
        }
        current = (core_object_t*)current->obj_prev;
    }

    if (self->free_after_use) {
        /* free all objects except payload */
        current = (core_object_t*)obj;
        core_object_t* parent = current;
        while (current != NULL) {
            parent = current;
            current = (core_object_t*)current->obj_prev;
            if (parent->obj_type != CORE_OBJECT_PAYLOAD) {
                core_object_free(parent);
            }
        }
    }

    if (client >= self->max_clients) {
        self->discarded++;
        lwarning("packet discarded (client exceeded max_clients)");
        return;
    }

    ldebug("client(c): %d", client);
    _self->create_request(self, &self->client_arr[client], payload);
}

core_receiver_t output_dnssim_receiver()
{
    return (core_receiver_t)_receive;
}

void output_dnssim_set_transport(output_dnssim_t* self, output_dnssim_transport_t tr) {
    mlassert_self();

    switch(tr) {
    case OUTPUT_DNSSIM_TRANSPORT_UDP_ONLY:
        _self->create_request = _create_request_udp;
        lnotice("transport set to UDP (no TCP fallback)");
        break;
    case OUTPUT_DNSSIM_TRANSPORT_UDP:
    case OUTPUT_DNSSIM_TRANSPORT_TCP:
    case OUTPUT_DNSSIM_TRANSPORT_TLS:
    default:
        lfatal("unknown or unsupported transport");
        break;
    }

    _self->transport = tr;
}

int output_dnssim_target(output_dnssim_t* self, const char* ip, uint16_t port) {
    int ret;
    mlassert_self();
    lassert(ip, "ip is nil");
    lassert(port, "port is nil");

    ret = uv_ip6_addr(ip, port, (struct sockaddr_in6*)&_self->target);
    if (ret != 0) {
        lcritical("failed to parse IPv6 from \"%s\"", ip);
        return -1;
        // TODO IPv4 support
        //ret = uv_ip4_addr(ip, port, (struct sockaddr_in*)&_self->target);
        //if (ret != 0) {
        //    lcritical("failed to parse IP/IP6 from \"%s\"", ip);
        //    return -1;
        //}
    }

    lnotice("set target to %s port %d", ip, port);
    return 0;
}

int output_dnssim_bind(output_dnssim_t* self, const char* ip) {
    int ret;
    mlassert_self();
    lassert(ip, "ip is nil");

    _output_dnssim_source_t* source;
    lfatal_oom(source = malloc(sizeof(_output_dnssim_source_t)));

    ret = uv_ip6_addr(ip, 0, (struct sockaddr_in6*)&source->addr);
    if (ret != 0) {
        lfatal("failed to parse IPv6 from \"%s\"", ip);
        return -1;
        // TODO IPv4 support
    }

    if (_self->source == NULL) {
        source->next = source;
        _self->source = source;
    } else {
        source->next = _self->source->next;
        _self->source->next = source;
    }

    lnotice("bind to source address %s", ip);
    return 0;
}

int output_dnssim_run_nowait(output_dnssim_t* self)
{
    mlassert_self();

    return uv_run(&_self->loop, UV_RUN_NOWAIT);
}

static void _stat_timer_cb(uv_timer_t* handle)
{
    output_dnssim_t* self = (output_dnssim_t*)handle->data;
    lnotice("processed:%10ld; answered:%10ld; discarded:%10ld; ongoing:%10ld",
        self->processed, self->stats_sum->answered, self->discarded,
        self->ongoing);

    output_dnssim_stats_t* stats_next;
    lfatal_oom(stats_next = malloc(sizeof(output_dnssim_stats_t)));
    *stats_next = _stats_defaults;

    stats_next->prev = self->stats_current;
    self->stats_current->next = stats_next;
    self->stats_current = stats_next;
}

void output_dnssim_stat_collect(output_dnssim_t* self, uint64_t interval_ms)
{
    int ret;
    mlassert_self();

    _self->stat_timer.data = (void*)self;
    ret = uv_timer_init(&_self->loop, &_self->stat_timer);
    if (ret < 0) {
        lcritical("failed to init stat_timer: %s", uv_strerror(ret));
        return;
    }
    ret = uv_timer_start(&_self->stat_timer, _stat_timer_cb, interval_ms, interval_ms);
    if (ret < 0) {
        lcritical("failed to start stat_timer: %s", uv_strerror(ret));
        return;
    }
}

void output_dnssim_stat_finish(output_dnssim_t* self)
{
    int ret;
    mlassert_self();

    ret = uv_timer_stop(&_self->stat_timer);
    if (ret < 0) {
        lcritical("failed to stop stat_timer: %s", uv_strerror(ret));
        return;
    }
    uv_close((uv_handle_t*)&_self->stat_timer, NULL);
}
