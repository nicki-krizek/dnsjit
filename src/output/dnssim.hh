/*
 * Copyright (c) 2018-2019, CZ.NIC, z.s.p.o.
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

//lua:require("dnsjit.core.log")
//lua:require("dnsjit.core.receiver_h")

typedef enum output_dnssim_transport {
    OUTPUT_DNSSIM_TRANSPORT_UDP_ONLY = 255,
    OUTPUT_DNSSIM_TRANSPORT_UDP= 254,
    OUTPUT_DNSSIM_TRANSPORT_TCP = 253,
    OUTPUT_DNSSIM_TRANSPORT_TLS = 252
} output_dnssim_transport_t;

typedef struct output_dnssim {
    core_log_t _log;
    output_dnssim_transport_t transport;
} output_dnssim_t;

core_log_t* output_dnssim_log();

output_dnssim_t* output_dnssim_new();
void output_dnssim_free(output_dnssim_t* self);

int output_dnssim_run_nowait(output_dnssim_t* self);

core_receiver_t output_dnssim_receiver();
