/*  Copyright (C) 2022 CZ.NIC, z.s.p.o. <knot-dns@labs.nic.cz>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*!
 * \file
 *
 * \brief TCP buffer helpers.
 *
 * \addtogroup xdp
 * @{
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/uio.h>

typedef struct knot_tcp_outbuf {
	struct knot_tcp_outbuf *next;
	uint32_t len;
	uint32_t seqno;
	bool sent;
	uint8_t bytes[];
} knot_tcp_outbuf_t;

typedef enum {
	KNOT_SWEEP_CTR_TIMEOUT    = 0,
	KNOT_SWEEP_CTR_LIMIT_CONN = 1,
	KNOT_SWEEP_CTR_LIMIT_IBUF = 2,
	KNOT_SWEEP_CTR_LIMIT_OBUF = 3,
} knot_sweep_counter_t;

typedef struct knot_sweep_stats {
	uint64_t last_log; // in seconds
	uint32_t total;
	uint32_t counters[4];
} knot_sweep_stats_t;

inline static void knot_sweep_stats_incr(knot_sweep_stats_t *stats, knot_sweep_counter_t counter)
{
	(stats->counters[counter])++;
	(stats->total)++;
}

inline static void knot_sweep_stats_reset(knot_sweep_stats_t *stats)
{
	memset(stats, 0, sizeof(*stats));
}

/*!
 * \brief Handle DNS-over-TCP payloads in buffer and message.
 *
 * \param buffer         In/out: persistent buffer to store incomplete DNS payloads between receiving packets.
 * \param data           In: momental DNS payloads in incoming packet.
 * \param inbufs         Out: list of incoming DNS messages.
 * \param inbufs_count   Out: number of inbufs.
 * \param buffers_total  In/Out: total size of buffers (will be increased or decreased).
 *
 * \return KNOT_EOK, KNOT_ENOMEM
 */
int knot_tcp_inbuf_update(struct iovec *buffer, struct iovec data,
                          struct iovec **inbufs, size_t *inbufs_count,
                          size_t *buffers_total);

/*!
 * \brief Add payload to be sent by TCP, to output buffers.
 *
 * \param bufs             Output buffers to be updated.
 * \param data             Payload to be sent.
 * \param len              Payload length.
 * \param ignore_lastbyte  Evil mode: drop last byte of the payload.
 * \param mss              Connection outgoing MSS.
 * \param outbufs_total    In/out: total outbuf statistic to be updated.
 *
 * \return KNOT_E*
 */
int knot_tcp_outbufs_add(knot_tcp_outbuf_t **bufs, uint8_t *data, size_t len,
                         bool ignore_lastbyte, uint32_t mss, size_t *outbufs_total);

/*!
 * \brief Remove+free acked data from output buffers.
 *
 * \param bufs             Output buffers to be updated.
 * \param ackno            Ackno of received ACK.
 * \param outbufs_total    In/out: total outbuf statistic to be updated.
 */
void knot_tcp_outbufs_ack(knot_tcp_outbuf_t **bufs, uint32_t ackno, size_t *outbufs_total);

/*!
 * \brief Prepare output buffers to be sent now.
 *
 * \param bufs          Output buffers to be updated.
 * \param window_size   Connection outgoing window size.
 * \param resend        Send also possibly already sent data.
 * \param send_start    Out: first output buffer to be sent.
 * \param send_count    Out: number of output buffers to be sent.
 */
void knot_tcp_outbufs_can_send(knot_tcp_outbuf_t *bufs, ssize_t window_size, bool resend,
                               knot_tcp_outbuf_t **send_start, size_t *send_count);

/*!
 * \brief Compute allocated size of output buffers.
 */
size_t knot_tcp_outbufs_usage(knot_tcp_outbuf_t *bufs);

/*! @} */
