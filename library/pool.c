/*
 * Copyright 2008+ Evgeniy Polyakov <zbr@ioremap.net>
 *
 * This file is part of Elliptics.
 *
 * Elliptics is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Elliptics is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Elliptics.  If not, see <http://www.gnu.org/licenses/>.
 */

#define __STDC_FORMAT_MACROS
#include <inttypes.h>

#include <sys/stat.h>
#include <netinet/in.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>

#include "access_context.h"
#include "elliptics.h"
#include "backend.h"
#include "elliptics/interface.h"
#include "monitor/monitor.h"
#include "monitor/measure_points.h"
#include "request_queue.h"
#include "library/logger.hpp"
#include "library/backend.h"
#include "library/n2_protocol.h"
#include "library/native_protocol/native_protocol.h"

static char *dnet_work_io_mode_string[] = {
	[DNET_WORK_IO_MODE_BLOCKING] = "BLOCKING",
	[DNET_WORK_IO_MODE_NONBLOCKING] = "NONBLOCKING",
	[DNET_WORK_IO_MODE_LIFO] = "LIFO",
};

static char *dnet_work_io_mode_str(int mode)
{
	if (mode < 0 || mode >= (int)ARRAY_SIZE(dnet_work_io_mode_string))
		return NULL;

	return dnet_work_io_mode_string[mode];
}

void dnet_work_pool_stop(struct dnet_work_pool_place *place) {
	int i;
	struct dnet_work_io *wio;

	pthread_mutex_lock(&place->lock);

	for (i = 0; i < place->pool->num; ++i) {
		wio = &place->pool->wio_list[i];
		if (!wio->joined) {
			pthread_join(wio->tid, NULL);
			wio->joined = 1;
		}
	}

	pthread_mutex_unlock(&place->lock);
}

static void dnet_work_pool_cleanup(struct dnet_work_pool_place *place)
{
	int i;
	struct dnet_io_req *r, *tmp;
	struct dnet_work_io *wio;

	pthread_mutex_lock(&place->lock);

	for (i = 0; i < place->pool->num; ++i) {
		wio = &place->pool->wio_list[i];

		list_for_each_entry_safe(r, tmp, &wio->reply_list, req_entry) {
			list_del(&r->req_entry);
			dnet_io_req_free(r);
		}

		list_for_each_entry_safe(r, tmp, &wio->request_list, req_entry) {
			list_del(&r->req_entry);
			dnet_io_req_free(r);
		}
	}

	pthread_mutex_destroy(&place->pool->lock);

	dnet_request_queue_destroy(place->pool);

	free(place->pool->wio_list);
	free(place->pool);

	place->pool = NULL;

	pthread_mutex_unlock(&place->lock);
}

void dnet_work_pool_exit(struct dnet_work_pool_place *place)
{
	dnet_work_pool_stop(place);
	dnet_work_pool_cleanup(place);
}

static int dnet_work_pool_grow(struct dnet_node *n, struct dnet_work_pool *pool, int num, void *(* process)(void *))
{
	int i = 0, j, err;
	struct dnet_work_io *wio;

	pthread_mutex_lock(&pool->lock);

	pool->wio_list = malloc(num * sizeof(struct dnet_work_io));
	if (!pool->wio_list) {
		err = -ENOMEM;
		goto err_out_io_threads;
	}

	for (i = 0; i < num; ++i) {
		wio = &pool->wio_list[i];

		wio->thread_index = i;
		wio->pool = pool;
		wio->trans = ~0ULL;
		wio->joined = 0;
		INIT_LIST_HEAD(&wio->reply_list);
		INIT_LIST_HEAD(&wio->request_list);

		err = pthread_create(&wio->tid, NULL, process, wio);
		if (err) {
			err = -err;
			dnet_log(n, DNET_LOG_ERROR, "Failed to create IO thread: %d", err);
			goto err_out_io_threads;
		}
	}

	dnet_log(n, DNET_LOG_INFO, "Grew %s pool by: %d -> %d IO threads",
			dnet_work_io_mode_str(pool->mode), pool->num, pool->num + num);

	pool->num = num;
	pthread_mutex_unlock(&pool->lock);

	return 0;

err_out_io_threads:
	for (j = 0; j < i; ++j) {
		wio = &pool->wio_list[j];
		pthread_join(wio->tid, NULL);
	}

	free(pool->wio_list);

	pthread_mutex_unlock(&pool->lock);

	return err;
}

int dnet_work_pool_place_init(struct dnet_work_pool_place *pool)
{
	int err;
	memset(pool, 0, sizeof(struct dnet_work_pool_place));

	err = pthread_mutex_init(&pool->lock, NULL);
	if (err) {
		err = -err;
		goto err_out_exit;
	}

err_out_exit:
	return err;
}

void dnet_work_pool_place_cleanup(struct dnet_work_pool_place *pool)
{
	pthread_mutex_destroy(&pool->lock);
}

int dnet_work_pool_alloc(struct dnet_work_pool_place *place,
                         struct dnet_node *n,
                         int num,
                         int mode,
                         size_t queue_limit,
                         const char *pool_id,
                         void *(*process)(void *)) {
	int err;
	struct dnet_work_pool *pool;

	pthread_mutex_lock(&place->lock);

	place->pool = pool = calloc(1, sizeof(struct dnet_work_pool));
	if (!pool) {
		err = -ENOMEM;
		goto err_out_exit;
	}

	err = pthread_mutex_init(&pool->lock, NULL);
	if (err) {
		err = -err;
		goto err_out_free;
	}

	pool->num = 0;
	pool->mode = mode;
	pool->n = n;

	strncpy(pool->pool_id, pool_id, sizeof(pool->pool_id));

	pool->request_queue = dnet_request_queue_create(mode, queue_limit);
	if (!pool->request_queue) {
		err = -ENOMEM;
		goto err_out_mutex_destroy;
	}

	err = dnet_work_pool_grow(n, pool, num, process);
	if (err)
		goto err_out_mutex_destroy;

	pthread_mutex_unlock(&place->lock);

	return err;

err_out_mutex_destroy:
	pthread_mutex_destroy(&pool->lock);
err_out_free:
	free(pool);
err_out_exit:
	pthread_mutex_unlock(&place->lock);
	return err;
}

// Keep this enums in sync with enums from dnet_process_cmd_without_backend_raw
static int dnet_cmd_needs_backend(int command)
{
	// backend is not needed for:
	switch (command) {
	case DNET_CMD_AUTH:
	case DNET_CMD_STATUS:
	case DNET_CMD_REVERSE_LOOKUP:
	case DNET_CMD_JOIN:
	case DNET_CMD_ROUTE_LIST:
	case DNET_CMD_MONITOR_STAT:
	case DNET_CMD_BACKEND_CONTROL:
	case DNET_CMD_BACKEND_STATUS:
	case DNET_CMD_BULK_READ_NEW:
	case DNET_CMD_BULK_REMOVE_NEW:
		return 0;
	}
	return 1;
}

static inline void make_thread_stat_id(char *buffer, int size, struct dnet_work_pool *pool)
{
	/* Could have used dnet_work_io_mode_str() to get string name
	 for the pool's mode, but for statistic lowercase names works better and
	 dnet_work_io_mode_str() provides mode names in uppercase.
	*/
	const char *mode_marker;
	switch (pool->mode) {
	case DNET_WORK_IO_MODE_NONBLOCKING:
		mode_marker = "nonblocking";
		break;
	case DNET_WORK_IO_MODE_LIFO:
		mode_marker = "lifo";
		break;
	default:
		mode_marker = "blocking";
		break;
	}
	snprintf(buffer, size - 1, "%s.%s", pool->pool_id, mode_marker);
}

struct dnet_cmd *dnet_io_req_get_cmd(struct dnet_io_req *r) {
	if (r->io_req_type == DNET_IO_REQ_OLD_PROTOCOL)
		// dnet_io_req enqueued from old mechanic
		return r->header;
	else
		// dnet_io_req enqueued from protocol-independent mechanic
		return n2_io_req_get_cmd(r);
}

int dnet_io_req_set_request_backend_id(struct dnet_io_req *r, int backend_id) {
	if (r->io_req_type == DNET_IO_REQ_OLD_PROTOCOL) {
		// dnet_io_req enqueued from old mechanic
		struct dnet_cmd *cmd = r->header;
		cmd->backend_id = backend_id;
		return 0;
	} else {
		// dnet_io_req enqueued from protocol-independent mechanic
		return n2_io_req_set_request_backend_id(r, backend_id);
	}
}

static void dnet_update_trans_timestamp_network(struct dnet_io_req *r)
{
	struct dnet_net_state *st = r->st;
	struct dnet_cmd *cmd = dnet_io_req_get_cmd(r);

	if (cmd->flags & DNET_FLAGS_REPLY) {
		struct dnet_trans *t;

		pthread_mutex_lock(&st->trans_lock);
		t = dnet_trans_search(st, cmd->trans);
		if (t) {
			dnet_trans_update_timestamp(t);

			/*
			 * Always remove transaction from 'timer' tree,
			 * thus it will not be found by checker thread and
			 * its callback will not be called under us.
			 */
			dnet_trans_remove_timer_nolock(st, t);
		}
		pthread_mutex_unlock(&st->trans_lock);

		dnet_trans_put(t);
	}
}

void dnet_schedule_io(struct dnet_node *n, struct dnet_io_req *r)
{
	struct dnet_work_pool_place *place = NULL;
	struct dnet_work_pool *pool = NULL;
	struct dnet_cmd *cmd = dnet_io_req_get_cmd(r);
	int nonblocking = !!(cmd->flags & DNET_FLAGS_NOLOCK);
	ssize_t backend_id = -1;
	char thread_stat_id[255];
	int log_level = DNET_LOG_INFO;
	r->recv_time = DIFF_TIMESPEC(r->st->rcv_start_ts, r->st->rcv_finish_ts);

	if (cmd->cmd == DNET_CMD_ITERATOR || cmd->cmd == DNET_CMD_ITERATOR_NEW)
		log_level = DNET_LOG_DEBUG;

	if (cmd->size > 0) {
		dnet_log(r->st->n, log_level, "%s: %s: RECV cmd: %s, cmd-size: %" PRIu64
		                              ", nonblocking: %d, cflags: %s, trans: %" PRIu64 ", recv-time: %ld usecs",
		         dnet_state_dump_addr(r->st), dnet_dump_id(&cmd->id), dnet_cmd_string(cmd->cmd), cmd->size,
		         nonblocking, dnet_flags_dump_cflags(cmd->flags), cmd->trans, r->recv_time);
	} else if ((cmd->size == 0) && !(cmd->flags & DNET_FLAGS_MORE) && (cmd->flags & DNET_FLAGS_REPLY)) {
		dnet_log(r->st->n, log_level, "%s: %s: RECV ACK cmd: %s, nonblocking: %d, cflags: %s, trans: %" PRIu64
		                              ", recv-time: %ld usecs",
		         dnet_state_dump_addr(r->st), dnet_dump_id(&cmd->id), dnet_cmd_string(cmd->cmd), nonblocking,
		         dnet_flags_dump_cflags(cmd->flags), cmd->trans, r->recv_time);
	} else {
		int reply = !!(cmd->flags & DNET_FLAGS_REPLY);

		dnet_log(r->st->n, log_level, "%s: %s: RECV cmd: %s, cmd-size: %" PRIu64
		                              ", nonblocking: %d, cflags: %s, trans: %" PRIu64
		                              ", reply: %d, recv-time: %ld usecs",
		         dnet_state_dump_addr(r->st), dnet_dump_id(&cmd->id), dnet_cmd_string(cmd->cmd), cmd->size,
		         nonblocking, dnet_flags_dump_cflags(cmd->flags), cmd->trans, reply,  r->recv_time);
	}

	dnet_update_trans_timestamp_network(r);

	if (cmd->flags & DNET_FLAGS_DIRECT_BACKEND) {
		backend_id = cmd->backend_id;
	} else if (dnet_cmd_needs_backend(cmd->cmd)) {
		backend_id = dnet_state_search_backend(n, &cmd->id);
	}

	place = dnet_backend_get_place(n, backend_id, nonblocking);

	pool = place->pool;

	make_thread_stat_id(thread_stat_id, sizeof(thread_stat_id), pool);

	// If we are processing the command we should update cmd->backend_id to actual one
	if (!(cmd->flags & DNET_FLAGS_REPLY)) {
		int err = dnet_io_req_set_request_backend_id(r, backend_id >= 0 ? backend_id : -1);

		// TODO(sabramkin): error can occur only during unfinished refactoring, remove this error case
		if (err) {
			dnet_log(n, DNET_LOG_ERROR, "%s: %s: backend_id: %zd, place: %p, "
			                            "failed to set cmd->backend_id : %s %d",
	        	         dnet_state_dump_addr(r->st), dnet_dump_id(&cmd->id), backend_id, place,
	        	         strerror(-err), err);
			pthread_mutex_unlock(&place->lock);
			return;
		}
	}

	dnet_log(n, DNET_LOG_DEBUG, "%s: %s: backend_id: %zd, place: %p, cmd->backend_id: %d",
	         dnet_state_dump_addr(r->st), dnet_dump_id(&cmd->id), backend_id, place, cmd->backend_id);

	dnet_push_request(pool, r, thread_stat_id);

	pthread_mutex_unlock(&place->lock);

	FORMATTED(HANDY_TIMER_START, ("pool.%s.queue.wait_time", thread_stat_id), (uint64_t)&r->req_entry);
	FORMATTED(HANDY_COUNTER_INCREMENT, ("pool.%s.queue.size", thread_stat_id), 1);
	HANDY_COUNTER_INCREMENT("io.input.queue.size", 1);
}

void dnet_schedule_command(struct dnet_net_state *st)
{
	st->rcv_flags = DNET_IO_CMD;

	if (st->rcv_data) {
#if 0
		struct dnet_cmd *c = &st->rcv_cmd;
		unsigned long long tid = c->trans;
		dnet_log(st->n, DNET_LOG_DEBUG, "freed: size: %llu, trans: %llu, reply: %d, ptr: %p.",
						(unsigned long long)c->size, tid, tid != c->trans, st->rcv_data);
#endif
		if (!st->rcv_buffer_used)
			free(st->rcv_data);
		st->rcv_data = NULL;
	}

	st->rcv_end = sizeof(struct dnet_cmd);
	st->rcv_offset = 0;
}

static int dnet_process_recv_single(struct dnet_net_state *st)
{
	struct dnet_node *n = st->n;
	struct dnet_io_req *r;
	void *data;
	uint64_t size;
	int err;

	dnet_logger_set_trace_id(st->rcv_cmd.trace_id, st->rcv_cmd.flags & DNET_FLAGS_TRACE_BIT);
again:
	/*
	 * Reading command first.
	 */
	if (st->rcv_flags & DNET_IO_CMD)
		data = &st->rcv_cmd;
	else
		data = st->rcv_data;
	data += st->rcv_offset;
	size = st->rcv_end - st->rcv_offset;

	if (size) {
		err = recv(st->read_s, data, size, 0);
		if (err < 0) {
			err = -EAGAIN;
			if (errno != EAGAIN && errno != EINTR) {
				err = -errno;
				DNET_ERROR(n, "%s: failed to receive data, socket: %d/%d", dnet_state_dump_addr(st),
				           st->read_s, st->write_s);
				goto out;
			}

			goto out;
		}

		if (err == 0) {
			dnet_log(n, DNET_LOG_ERROR, "%s: peer has disconnected, socket: %d/%d",
				dnet_state_dump_addr(st), st->read_s, st->write_s);
			err = -ECONNRESET;
			goto out;
		}

		dnet_logger_unset_trace_id();
		dnet_logger_set_trace_id(st->rcv_cmd.trace_id, st->rcv_cmd.flags & DNET_FLAGS_TRACE_BIT);

		if ((st->rcv_flags & DNET_IO_CMD) && (st->rcv_offset == 0)) {
			clock_gettime(CLOCK_MONOTONIC_RAW, &st->rcv_start_ts);
		}

		st->rcv_offset += err;
	}

	if (st->rcv_offset != st->rcv_end)
		goto again;

	if (st->rcv_flags & DNET_IO_CMD) {
		unsigned long long tid;
		struct dnet_cmd *c = &st->rcv_cmd;

		dnet_convert_cmd(c);

		tid = c->trans;

		dnet_log(n, DNET_LOG_DEBUG, "%s: %s: received trans: %llu <- %s/%d: "
				"size: %llu, cflags: %s, status: %d",
				dnet_dump_id(&c->id), dnet_cmd_string(c->cmd), tid,
				dnet_state_dump_addr(st), c->backend_id,
				(unsigned long long)c->size, dnet_flags_dump_cflags(c->flags), c->status);

		st->rcv_flags &= ~DNET_IO_CMD;

		err = n2_native_protocol_prepare_message_buffer(st);
		if (err == 0) {
			if (c->size)
				goto again;
			else
				goto schedule;
		}
		if (err != -ENOTSUP)
			goto out;

		r = malloc(c->size + sizeof(struct dnet_cmd) + sizeof(struct dnet_io_req));
		if (!r) {
			err = -ENOMEM;
			goto out;
		}
		memset(r, 0, sizeof(struct dnet_io_req));

		r->header = r + 1;
		r->hsize = sizeof(struct dnet_cmd);
		memcpy(r->header, &st->rcv_cmd, sizeof(struct dnet_cmd));

		st->rcv_data = r;
		st->rcv_offset = sizeof(struct dnet_io_req) + sizeof(struct dnet_cmd);
		st->rcv_end = st->rcv_offset + c->size;

		if (c->size) {
			r->data = r->header + sizeof(struct dnet_cmd);
			r->dsize = c->size;

			/*
			 * We read the command header, now get the data.
			 */
			goto again;
		}
	}

schedule:
	clock_gettime(CLOCK_MONOTONIC_RAW, &st->rcv_finish_ts);

	err = n2_native_protocol_schedule_message(st);
	if (err != -ENOTSUP)
		goto out;

	r = st->rcv_data;
	st->rcv_data = NULL;

	dnet_schedule_command(st);

	r->st = dnet_state_get(st);

	dnet_schedule_io(n, r);
	dnet_logger_unset_trace_id();
	return 0;

out:
	if (err != -EAGAIN && err != -EINTR)
		dnet_schedule_command(st);

	dnet_logger_unset_trace_id();
	return err;
}

/*
 * Tries to unmap IPv4 from IPv6.
 * If it is succeeded addr will contain valid unmapped IPv4 address
 * otherwise it will contain original address.
 */
static void try_to_unmap_ipv4(struct dnet_addr *addr) {
	struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *) addr->addr;

	/*
	 * if address isn't IPv6 or it isn't mapped IPv4 then there is nothing to be unmapped
	 */
	if (addr->family != AF_INET6 || !IN6_IS_ADDR_V4MAPPED(&sin6->sin6_addr))
		return;

	struct sockaddr_in sin;
	memset(&sin, 0, sizeof(sin));

	sin.sin_family = AF_INET;
	sin.sin_port = sin6->sin6_port;
	// copies last 4 bytes from mapped IPv6 that represents original IPv4 address
	memcpy(&sin.sin_addr.s_addr, &sin6->sin6_addr.s6_addr[12], 4);

	memcpy(&addr->addr, &sin, sizeof(sin));
	addr->addr_len = sizeof(sin);
	addr->family = AF_INET;
}

int dnet_socket_local_addr(int s, struct dnet_addr *addr)
{
	int err;
	socklen_t len;

	len = addr->addr_len = sizeof(addr->addr);

	err = getsockname(s, (struct sockaddr *)addr->addr, &len);
	if (err < 0)
		return -errno;

	addr->addr_len = len;
	addr->family = ((struct sockaddr *)addr->addr)->sa_family;

	try_to_unmap_ipv4(addr);
	return 0;
}

int dnet_local_addr_index(struct dnet_node *n, struct dnet_addr *addr)
{
	int i;

	for (i = 0; i < n->addr_num; ++i) {
		if (dnet_addr_equal(addr, &n->addrs[i]))
			return i;
	}

	return -1;
}

int dnet_state_accept_process(struct dnet_net_state *orig, struct epoll_event *ev __unused)
{
	struct dnet_node *n = orig->n;
	int err, cs, idx;
	struct dnet_addr addr, saddr;
	struct dnet_net_state *st;
	socklen_t salen;
	char client_addr[128], server_addr[128];

	memset(&addr, 0, sizeof(addr));

	salen = addr.addr_len = sizeof(addr.addr);
	cs = accept(orig->accept_s, (struct sockaddr *)&addr.addr, &salen);
	if (cs < 0) {
		err = -errno;

		/* EAGAIN (or EWOULDBLOCK) is totally good here */
		if (err == -EAGAIN || err == -EWOULDBLOCK) {
			goto err_out_exit;
		}

		/* Some error conditions considered "recoverable" and treated the same way as EAGAIN */
		DNET_ERROR(n, "Failed to accept new client at %s", dnet_state_dump_addr(orig));
		if (err == -ECONNABORTED || err == -EMFILE || err == -ENOBUFS || err == -ENOMEM) {
			err = -EAGAIN;
			goto err_out_exit;
		}

		/* Others are too bad to live with */
		dnet_log(n, DNET_LOG_ERROR, "FATAL: Can't recover from this error: %d, exiting...", err);
		exit(err);
	}

	addr.family = orig->addr.family;
	addr.addr_len = salen;

	try_to_unmap_ipv4(&addr);

	dnet_set_sockopt(n, cs);

	err = dnet_socket_local_addr(cs, &saddr);
	if (err) {
		dnet_log(n, DNET_LOG_ERROR, "%s: failed to resolve server addr for connected client: %s [%d]",
				dnet_addr_string_raw(&addr, client_addr, sizeof(client_addr)), strerror(-err), -err);
		goto err_out_exit;
	}

	idx = dnet_local_addr_index(n, &saddr);

	st = dnet_state_create(n, NULL, 0, &addr, cs, &err, 0, 0, idx, 0, NULL, 0);
	if (!st) {
		dnet_log(n, DNET_LOG_ERROR, "%s: Failed to create state for accepted client: %s [%d]",
				dnet_addr_string_raw(&addr, client_addr, sizeof(client_addr)), strerror(-err), -err);
		err = -EAGAIN;

		/* We do not close socket, since it is closed in dnet_state_create() */
		goto err_out_exit;
	}

	// @dnet_net_state() returns state with 2 reference counters
	dnet_state_put(st);

	dnet_log(n, DNET_LOG_INFO, "Accepted client %s, socket: %d, server address: %s, idx: %d",
			dnet_addr_string_raw(&addr, client_addr, sizeof(client_addr)), cs,
			dnet_addr_string_raw(&saddr, server_addr, sizeof(server_addr)), idx);

	return 0;

err_out_exit:
	return err;
}

void dnet_unschedule_send(struct dnet_net_state *st)
{
	if (st->write_s >= 0)
		epoll_ctl(st->epoll_fd, EPOLL_CTL_DEL, st->write_s, NULL);
}

void dnet_unschedule_all(struct dnet_net_state *st)
{
	if (st->read_s >= 0)
		epoll_ctl(st->epoll_fd, EPOLL_CTL_DEL, st->read_s, NULL);
	if (st->write_s >= 0)
		epoll_ctl(st->epoll_fd, EPOLL_CTL_DEL, st->write_s, NULL);
	if (st->accept_s >= 0)
		epoll_ctl(st->epoll_fd, EPOLL_CTL_DEL, st->accept_s, NULL);
}

static int dnet_process_send_single(struct dnet_net_state *st)
{
	struct dnet_io_req *r = NULL;
	int err;
	uint32_t counter = 0;

	while (1) {
		r = NULL;

		pthread_mutex_lock(&st->send_lock);
		if (!list_empty(&st->send_list)) {
			r = list_first_entry(&st->send_list, struct dnet_io_req, req_entry);
		} else {
			dnet_unschedule_send(st);
		}
		pthread_mutex_unlock(&st->send_lock);

		if (!r) {
			err = -EAGAIN;
			goto err_out_exit;
		}

		err = r->serialized ? n2_send_request(st, r) : dnet_send_request(st, r);

		if (!err) {
			pthread_mutex_lock(&st->send_lock);
			list_del(&r->req_entry);
			pthread_mutex_unlock(&st->send_lock);

			pthread_mutex_lock(&st->n->io->full_lock);
			list_stat_size_decrease(&st->n->io->output_stats, 1);
			pthread_mutex_unlock(&st->n->io->full_lock);
			HANDY_COUNTER_DECREMENT("io.output.queue.size", 1);

			if (atomic_read(&st->send_queue_size) > 0)
				if (atomic_dec(&st->send_queue_size) == DNET_SEND_WATERMARK_LOW) {
					dnet_log(st->n, DNET_LOG_DEBUG,
							"State low_watermark reached: %s: %ld, waking up",
							dnet_addr_string(&st->addr),
							atomic_read(&st->send_queue_size));
					pthread_cond_broadcast(&st->send_wait);
				}

			dnet_io_req_free(r);
			st->send_offset = 0;
			/* exit the loop, if @send_limit was set and it has been reached, and switch net thread to
			 * another ready state.
			 */
			if (st->n->send_limit && ++counter >= st->n->send_limit) {
				dnet_log(st->n, DNET_LOG_NOTICE, "Limit on number of packet sent to one state in a row "
				                                 "has been reached: limit: %" PRIu32,
				         st->n->send_limit);
				break;
			}
		}

		if (err)
			goto err_out_exit;
	}

err_out_exit:
	if ((err < 0) && (atomic_read(&st->send_queue_size) > 0))
		pthread_cond_broadcast(&st->send_wait);

	return err;
}

static int dnet_schedule_network_io(struct dnet_net_state *st, int send)
{
	struct epoll_event ev;
	int err, fd;

	if (st->__need_exit) {
		DNET_ERROR(st->n, "%s: scheduling %s event on reset state: need-exit: %d", dnet_state_dump_addr(st),
		           send ? "SEND" : "RECV", st->__need_exit);
		return st->__need_exit;
	}

	if (send) {
		ev.events = EPOLLOUT;
		fd = st->write_s;

		ev.data.ptr = &st->write_data;
	} else {
		ev.events = EPOLLIN;
		fd = st->read_s;

		ev.data.ptr = &st->read_data;
	}

	if (fd >= 0) {
		err = epoll_ctl(st->epoll_fd, EPOLL_CTL_ADD, fd, &ev);
	} else {
		err = 0;
	}

	if (err < 0) {
		err = -errno;

		if (err == -EEXIST) {
			err = 0;
		} else {
			DNET_ERROR(st->n, "%s: failed to add %s event, fd: %d", dnet_state_dump_addr(st),
			           send ? "SEND" : "RECV", fd);
		}
	} else if (!send && st->accept_s >= 0) {
		ev.data.ptr = &st->accept_data;
		err = epoll_ctl(st->epoll_fd, EPOLL_CTL_ADD, st->accept_s, &ev);

		if (err < 0) {
			err = -errno;

			DNET_ERROR(st->n, "%s: failed to add %s event, fd: %d", dnet_state_dump_addr(st), "ACCEPT",
			           st->accept_s);
		}
	}

	if (send)
		pthread_cond_broadcast(&st->n->io->full_wait);

	return err;
}

int dnet_schedule_send(struct dnet_net_state *st)
{
	return dnet_schedule_network_io(st, 1);
}

int dnet_schedule_recv(struct dnet_net_state *st)
{
	return dnet_schedule_network_io(st, 0);
}

static int dnet_state_net_process(struct dnet_net_state *st, struct epoll_event *ev)
{
	int err = -ECONNRESET;

	if (ev->events & EPOLLIN) {
		err = dnet_process_recv_single(st);
		if (err && (err != -EAGAIN))
			goto err_out_exit;
	}
	if (ev->events & EPOLLOUT) {
		err = dnet_process_send_single(st);
		if (err && (err != -EAGAIN))
			goto err_out_exit;
	}

	if (ev->events & (EPOLLHUP | EPOLLERR)) {
		dnet_log(st->n, DNET_LOG_ERROR, "%s: received error event mask 0x%x, socket: %d",
				dnet_state_dump_addr(st), ev->events, ev->data.fd);
		err = -ECONNRESET;
	}
err_out_exit:
	return err;
}

static void dnet_check_work_pool_place(struct dnet_work_pool_place *place, uint64_t *queue_size, uint64_t *threads_count)
{
	struct dnet_work_pool *pool;

	pthread_mutex_lock(&place->lock);
	pool = place->pool;
	if (pool) {
		*queue_size += dnet_get_pool_queue_size(pool);

		pthread_mutex_lock(&pool->lock);
		*threads_count += pool->num;
		pthread_mutex_unlock(&pool->lock);
	}
	pthread_mutex_unlock(&place->lock);
}

void dnet_check_io_pool(struct dnet_io_pool *io, uint64_t *queue_size, uint64_t *threads_count) {
	dnet_check_work_pool_place(&io->recv_pool, queue_size, threads_count);
	dnet_check_work_pool_place(&io->recv_pool_nb, queue_size, threads_count);
}

static int dnet_check_io(struct dnet_io *io)
{
	uint64_t queue_size = 0;
	uint64_t threads_count = 0;

	dnet_check_io_pool(&io->pool, &queue_size, &threads_count);

	if (io->pools_manager)
		dnet_io_pools_check(io->pools_manager, &queue_size, &threads_count);

	if (queue_size <= threads_count * 1000)
		return 1;

	return 0;
}

static void dnet_shuffle_epoll_events(struct epoll_event *evs, int size) {
	int i = 0, j = 0;
	struct epoll_event tmp;

	if (size < 1)
		return;

	for (i = 0; i < size - 1; ++i) {
		j = i + rand() / (RAND_MAX / (size - i) + 1);

		// In case if j == i we can't use memcpy because of the overlap
		memcpy(&tmp, evs + j, sizeof(struct epoll_event));
		memmove(evs + j, evs + i, sizeof(struct epoll_event));
		memcpy(evs + i, &tmp, sizeof(struct epoll_event));
	}
}

static void *dnet_io_process_network(void *data_)
{
	struct dnet_net_io *nio = data_;
	struct dnet_node *n = nio->n;
	struct dnet_net_epoll_data *data;
	struct dnet_net_state *st;
	int evs_size = 100;
	struct epoll_event *evs = malloc(evs_size * sizeof(struct epoll_event));
	struct epoll_event *evs_tmp = NULL;
	struct timespec ts;
	int tmp = 0;
	int err = 0;
	int num_events = 0;
	int i = 0;
	struct timespec prev_ts, curr_ts;

	dnet_set_name(nio->name);
	dnet_logger_set_pool_id("net");

	dnet_log(n, DNET_LOG_NOTICE, "started %s pool", nio->name);

	if (evs == NULL) {
		dnet_log(n, DNET_LOG_ERROR, "Not enough memory to allocate epoll_events");
		goto err_out_exit;
	}

	// get current timestamp for future outputting "Net pool is suspended..." logging
	clock_gettime(CLOCK_MONOTONIC_RAW, &prev_ts);

	while (!n->need_exit) {
		// check if epoll possibly has more events to process then evs_size
		if (num_events >= evs_size) {
			tmp = 2 * evs_size; // tries to increase number of epoll_events
			evs_tmp = (struct epoll_event *)realloc(evs, sizeof(struct epoll_event) * tmp);
			if (evs_tmp) {
				evs = evs_tmp;
				evs_size = tmp;
			}
		}

		err = epoll_wait(nio->epoll_fd, evs, evs_size, 1000);
		if (err == 0)
			continue;

		if (err < 0) {
			err = -errno;

			if (err == -EAGAIN || err == -EINTR)
				continue;

			dnet_log(n, DNET_LOG_ERROR, "Failed to wait for IO fds: %s [%d]", strerror(-err), err);
			n->need_exit = err;
			break;
		}

		// tmp will counts number of send events
		tmp = 0;
		num_events = err;
		// shuffles available epoll_events
		dnet_shuffle_epoll_events(evs, num_events);
		for (i = 0; i < num_events; ++i) {
			data = evs[i].data.ptr;
			st = data->st;
			st->epoll_fd = nio->epoll_fd;

			if (data->fd == st->accept_s) {
				// We have to accept new connection
				++tmp;
				err = dnet_state_accept_process(st, &evs[i]);
			} else if ((evs[i].events & EPOLLOUT) || dnet_check_io(n->io)) {
				// if this is sending event or io pool queues are not full then process it
				++tmp;
				err = dnet_state_net_process(st, &evs[i]);
			} else {
				continue;
			}

			if (err == 0)
				continue;

			if (err < 0 && err != -EAGAIN) {
				char addr_str[128] = "<unknown>";
				if (n->addr_num) {
					dnet_addr_string_raw(&n->addrs[0], addr_str, sizeof(addr_str));
				}
				dnet_log(n, DNET_LOG_ERROR, "self: addr: %s, resetting state: %s (%p)",
				         addr_str, dnet_state_dump_addr(st), st);

				dnet_state_reset(st, err);

				pthread_mutex_lock(&st->send_lock);
				dnet_unschedule_all(st);
				pthread_mutex_unlock(&st->send_lock);

				dnet_add_reconnect_state(st->n, &st->addr, st->__join_state);

				// state still contains a fair number of transactions in its queue
				// they will not be cleaned up here - dnet_state_put() will only drop refctn by 1,
				// while every transaction holds a reference
				//
				// IO thread could remove transaction, it is the only place allowed to do it.
				// transactions may live in the tree and be accessed without locks in IO thread,
				// IO thread is kind of 'owner' of the transaction processing
				dnet_state_put(st);
				break;
			}
		}

		// wait condition variable if no data has been sent and io pool queues are still full
		if (tmp == 0 && !dnet_check_io(n->io)) {
			clock_gettime(CLOCK_MONOTONIC_RAW, &curr_ts);
			// print log only if previous log was written more then 1 seconds ago
			if ((curr_ts.tv_sec - prev_ts.tv_sec) > 1) {
				dnet_log(n, DNET_LOG_INFO, "Net pool is suspended because io pool queues is full");
				prev_ts = curr_ts;
			}
			// wait condition variable - io queues has a free slot or some socket has something to send
			pthread_mutex_lock(&n->io->full_lock);
			n->io->blocked = 1;
			while (!n->need_exit && !dnet_check_io(n->io)) {
				clock_gettime(CLOCK_REALTIME, &ts);
				ts.tv_sec += 1;
				if (pthread_cond_timedwait(&n->io->full_wait, &n->io->full_lock, &ts) == 0)
					break;
			}
			n->io->blocked = 0;
			pthread_mutex_unlock(&n->io->full_lock);
		}
	}

	free(evs);

err_out_exit:
	dnet_log(n, DNET_LOG_NOTICE, "finished net pool");
	dnet_logger_unset_pool_id();
	return &n->need_exit;
}

static void dnet_io_cleanup_states(struct dnet_node *n)
{
	struct dnet_net_state *st, *tmp;

	list_for_each_entry_safe(st, tmp, &n->storage_state_list, storage_state_entry) {
		dnet_unschedule_all(st);

		dnet_state_reset(st, -EUCLEAN);

		dnet_state_clean(st);
		dnet_state_put(st);
	}

	n->st = NULL;
}

void *dnet_io_process(void *data_) {
	struct dnet_work_io *wio = data_;
	struct dnet_work_pool *pool = wio->pool;
	struct dnet_node *n = pool->n;
	struct dnet_net_state *st;
	struct dnet_io_req *r;
	struct dnet_cmd *cmd = NULL;
	const int lifo = (pool->mode == DNET_WORK_IO_MODE_LIFO);
	const int nonblocking = (pool->mode == DNET_WORK_IO_MODE_NONBLOCKING || lifo);
	char thread_stat_id[255];

	dnet_set_name("dnet_%sio_%s", nonblocking ? "nb_" : "", pool->pool_id);

	make_thread_stat_id(thread_stat_id, sizeof(thread_stat_id), pool);

	dnet_logger_set_pool_id(pool->pool_id);

	dnet_log(n, DNET_LOG_NOTICE, "started io thread: #%d, nonblocking: %d, lifo: %d, pool: %s", wio->thread_index,
	         nonblocking, lifo, pool->pool_id);

	while (!n->need_exit && !pool->need_exit) {
		r = dnet_pop_request(wio, thread_stat_id);
		if (!r)
			continue;

		pthread_cond_broadcast(&n->io->full_wait);

		FORMATTED(HANDY_COUNTER_INCREMENT, ("pool.%s.active_threads", thread_stat_id), 1);

		st = r->st;
		cmd = dnet_io_req_get_cmd(r);

		dnet_logger_set_backend_id(cmd->backend_id);
		dnet_logger_set_trace_id(cmd->trace_id, cmd->flags & DNET_FLAGS_TRACE_BIT);

		dnet_log(n, DNET_LOG_DEBUG, "%s: %s: got IO event: %p: cmd: %s, hsize: %zu, dsize: %zu, mode: %s, "
		                            "backend_id: %d, queue_time: %lu usecs",
		         dnet_state_dump_addr(st), dnet_dump_id(&cmd->id), r, dnet_cmd_string(cmd->cmd), r->hsize,
		         r->dsize, dnet_work_io_mode_str(pool->mode), cmd->backend_id, r->queue_time);

		dnet_process_recv(st, r);

		dnet_log(n, DNET_LOG_DEBUG, "%s: %s: processed IO event: %p, cmd: %s", dnet_state_dump_addr(st),
		         dnet_dump_id(&cmd->id), r, dnet_cmd_string(cmd->cmd));

		dnet_release_request(wio, r);
		dnet_io_req_free(r);
		dnet_state_put(st);

		dnet_logger_unset_trace_id();
		dnet_logger_unset_backend_id();

		FORMATTED(HANDY_COUNTER_DECREMENT, ("pool.%s.active_threads", thread_stat_id), 1);
	}

	dnet_log(n, DNET_LOG_NOTICE, "finished io thread: #%d, nonblocking: %d, lifo: %d, pool: %s", wio->thread_index,
	         nonblocking, lifo, pool->pool_id);

	dnet_logger_unset_pool_id();

	return NULL;
}

static int dnet_net_io_init(struct dnet_node *n, struct dnet_net_io *nio, const char *name) {
	int err = 0;

	nio->n = n;
	nio->name = name;

	nio->epoll_fd = epoll_create(10000);
	if (nio->epoll_fd < 0) {
		err = -errno;
		dnet_log(n, DNET_LOG_ERROR, "Failed to create epoll fd: %s [%d]", strerror(-err), err);
		goto err_out;
	}

	fcntl(nio->epoll_fd, F_SETFD, FD_CLOEXEC);
	fcntl(nio->epoll_fd, F_SETFL, O_NONBLOCK);

	err = pthread_create(&nio->tid, NULL, dnet_io_process_network, nio);
	if (err) {
		err = -err;
		dnet_log(n, DNET_LOG_ERROR, "Failed to create network processing thread: %s [%d]", strerror(-err), err);
		goto err_out_close_epoll;
	}

	return 0;

err_out_close_epoll:
	close(nio->epoll_fd);
err_out:
	return err;
}

static void dnet_net_io_cleanup(struct dnet_net_io *nio) {
	pthread_join(nio->tid, NULL);
	close(nio->epoll_fd);
}

int dnet_io_init(struct dnet_node *n, struct dnet_config *cfg)
{
	int err, i;
	int io_size = sizeof(struct dnet_io) + sizeof(struct dnet_net_io) * cfg->net_thread_num;

	n->io = malloc(io_size);
	if (!n->io) {
		err = -ENOMEM;
		goto err_out_exit;
	}
	memset(n->io, 0, io_size);

	err = pthread_mutex_init(&n->io->full_lock, NULL);
	if (err) {
		err = -err;
		goto err_out_free;
	}

	err = pthread_cond_init(&n->io->full_wait, NULL);
	if (err) {
		err = -err;
		goto err_out_free_mutex;
	}

	list_stat_init(&n->io->output_stats);

	n->io->net_thread_num = cfg->net_thread_num;
	n->io->net_thread_pos = 0;
	n->io->net = (struct dnet_net_io *)(n->io + 1);

	err = dnet_work_pool_place_init(&n->io->pool.recv_pool);
	if (err) {
		goto err_out_free_cond;
	}

	err = dnet_work_pool_alloc(&n->io->pool.recv_pool, n, cfg->io_thread_num, DNET_WORK_IO_MODE_BLOCKING,
	                           /*queue_limit*/ 0, "sys", dnet_io_process);
	if (err) {
		goto err_out_cleanup_recv_place;
	}

	err = dnet_work_pool_place_init(&n->io->pool.recv_pool_nb);
	if (err) {
		goto err_out_free_recv_pool;
	}

	err = dnet_work_pool_alloc(&n->io->pool.recv_pool_nb, n, cfg->nonblocking_io_thread_num,
	                           DNET_WORK_IO_MODE_NONBLOCKING, /*queue_limit*/ 0, "sys", dnet_io_process);
	if (err) {
		goto err_out_cleanup_recv_place_nb;
	}

	err = n2_native_protocol_io_start(n);
	if (err) {
		goto err_out_free_recv_pool_nb;
	}

	if (cfg->flags & DNET_CFG_JOIN_NETWORK) {
		err = dnet_net_io_init(n, &n->io->acceptor, "dnet_acceptor");
		if (err) {
			goto err_out_protocol_io_stop;
		}
	}

	for (i = 0; i < n->io->net_thread_num; ++i) {
		err = dnet_net_io_init(n, &n->io->net[i], "dnet_net");
		if (err) {
			goto err_out_net_destroy;
		}
	}

	return 0;

err_out_net_destroy:
	n->need_exit = 1;
	while (--i >= 0) {
		dnet_net_io_cleanup(&n->io->net[i]);
	}

	if (n->flags & DNET_CFG_JOIN_NETWORK) {
		dnet_net_io_cleanup(&n->io->acceptor);
	}
err_out_protocol_io_stop:
	n2_native_protocol_io_stop(n);
err_out_free_recv_pool_nb:
	n->need_exit = 1;
	dnet_work_pool_exit(&n->io->pool.recv_pool_nb);
err_out_cleanup_recv_place_nb:
	dnet_work_pool_place_cleanup(&n->io->pool.recv_pool_nb);
err_out_free_recv_pool:
	n->need_exit = 1;
	dnet_work_pool_exit(&n->io->pool.recv_pool);
err_out_cleanup_recv_place:
	dnet_work_pool_place_cleanup(&n->io->pool.recv_pool);
err_out_free_cond:
	pthread_cond_destroy(&n->io->full_wait);
err_out_free_mutex:
	pthread_mutex_destroy(&n->io->full_lock);
err_out_free:
	free(n->io);
err_out_exit:
	n->io = NULL;
	return err;
}

void dnet_io_stop(struct dnet_node *n) {
	struct dnet_io *io = n->io;
	int i;

	dnet_set_need_exit(n);

	for (i = 0; i < io->net_thread_num; ++i) {
		dnet_net_io_cleanup(&io->net[i]);
	}

	if (n->flags & DNET_CFG_JOIN_NETWORK) {
		dnet_net_io_cleanup(&io->acceptor);
	}

	n2_native_protocol_io_stop(n);

	dnet_work_pool_stop(&io->pool.recv_pool_nb);
	dnet_work_pool_stop(&io->pool.recv_pool);
}

void dnet_io_cleanup(struct dnet_node *n)
{
	struct dnet_io *io = n->io;

	dnet_work_pool_cleanup(&io->pool.recv_pool_nb);
	dnet_work_pool_place_cleanup(&io->pool.recv_pool_nb);

	dnet_work_pool_cleanup(&io->pool.recv_pool);
	dnet_work_pool_place_cleanup(&io->pool.recv_pool);

	if (io->backends_manager || io->pools_manager)
		dnet_backends_destroy(n);

	dnet_io_cleanup_states(n);

	free(io);
	n->io = NULL;
}
