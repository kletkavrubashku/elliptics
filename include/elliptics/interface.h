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

#ifndef __DNET_INTERFACE_H
#define __DNET_INTERFACE_H

#include <elliptics/core.h>

#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/time.h>

#include <arpa/inet.h>
#include <netinet/in.h>

#include <elliptics/packet.h>

#include "logger.hpp"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef O_CLOEXEC
#define O_CLOEXEC	02000000
#endif

#ifndef FD_CLOEXEC
#define FD_CLOEXEC	1
#endif

struct dnet_net_state;
struct dnet_config_data;
struct dnet_node;
struct dnet_session;
struct n2_request_info;

int dnet_need_exit(struct dnet_node *n);
void dnet_set_need_exit(struct dnet_node *n);

/*
 * Callback data structures.
 *
 * [dnet_cmd]
 * [attributes]
 *
 * [dnet_cmd] header when present shows number of attached bytes.
 * It should be equal to the al_attr structure at least in the
 * correct message, otherwise it should be discarded.
 * One can also check cmd->flags if it has DNET_FLAGS_MORE or
 * DNET_FLAGS_DESTROY bit set. The former means that callback
 * will be invoked again in the future and transaction is not
 * yet completed. The latter means that transaction is about
 * to be destroyed.
 */

/*
 * IO helpers.
 *
 * dnet_node is a node pointer returned by calling dnet_node_create()
 * dnet_io_attr contains IO details (size, offset and the checksum)
 * completion callback (if present) will be invoked when IO transaction is finished
 * private data will be stored in the appropriate transaction and can be obtained
 * when transaction completion callback is invoked. It will be automatically
 * freed when transaction is completed.
 */

typedef int (* transaction_callback)(struct dnet_addr *addr,
				    struct dnet_cmd *cmd,
				    void *priv);

struct dnet_io_control {
	/* Used as cmd->id/group_id - 'address' of the remote node */
	struct dnet_id			id;

	/*
	 * IO control structure - it is copied into resulted transaction as is.
	 * During write origin will be replaced with data transformation, and
	 * id will be replaced with the object name transformation.
	 */
	struct dnet_io_attr		io;

	/*
	 * If present, will be invoked when transaction is completed.
	 * Can be invoked multiple times, the last one will be when
	 * cmd->flags does not have DNET_FLAGS_MORE flag.
	 *
	 * All parameters are releated to the received transaction reply.
	 */
	transaction_callback		complete;

	/*
	 * Transaction completion private data. Will be accessible in the
	 * above completion callback.
	 */
	void				*priv;

	/*
	 * Data to be sent.
	 */
	const void			*data;

	/*
	 * File descriptor to read data from (for the write transaction).
	 */
	int				fd;

	/*
	 * This offset represent local data shift, when local and remote offsets differ.
	 * For example when we want to put local object into transaction but place it
	 * after some bytes in the remote object.
	 */
	uint64_t			local_offset;

	/*
	 * IO command.
	 */
	unsigned int			cmd;

	/*
	 * Command flags (DNET_FLAGS_*)
	 */
	uint64_t			cflags;

	/* Data transaction timestamp */
	struct timespec			ts;
};

int dnet_search_range(struct dnet_node *n, struct dnet_id *id,
		struct dnet_raw_id *start, struct dnet_raw_id *next);


/*
 * Operations to perform on request's data when request is about to be destroyed
 */
#define DNET_IO_REQ_FLAGS_CLOSE			(1<<0)	/* close fd */
#define DNET_IO_REQ_FLAGS_CACHE_FORGET		(1<<1)	/* try to remove read data from page cache using fadvice */

int __attribute__((weak)) dnet_send_read_data(void *state, struct dnet_cmd *cmd, struct dnet_io_attr *io,
		void *data, int fd, uint64_t offset, int on_exit);

#define DNET_MAX_ADDRLEN		256
#define DNET_MAX_PORTLEN		8

/*
 * cfg->flags
 *
 * IT IS ALSO PROVIDED IN PYTHON BINDING so if you want to add new flag
 * please also add it to elliptics_config_flags and to BOOST_PYTHON_MODULE(core) in elliptics_python.cpp
 */
#define DNET_CFG_JOIN_NETWORK		(1<<0)		/* given node joins network and becomes part of the storage */
#define DNET_CFG_NO_ROUTE_LIST		(1<<1)		/* do not request route table from remote nodes */
#define DNET_CFG_MIX_STATES		(1<<2)		/* mix states according to their weights before reading data */
#define DNET_CFG_NO_CSUM		(1<<3)		/* globally disable checksum verification and update */
#define DNET_CFG_RANDOMIZE_STATES	(1<<5)		/* randomize states for read requests */
#define DNET_CFG_KEEPS_IDS_IN_CLUSTER	(1<<6)		/* keeps ids in elliptics cluster */

static inline const char *dnet_flags_dump_cfgflags(uint64_t flags)
{
	static __thread char buffer[256];
	static struct flag_info infos[] = {
		{ DNET_CFG_JOIN_NETWORK, "join" },
		{ DNET_CFG_NO_ROUTE_LIST, "no_route_list" },
		{ DNET_CFG_MIX_STATES, "mix_states" },
		{ DNET_CFG_NO_CSUM, "no_csum" },
		{ DNET_CFG_RANDOMIZE_STATES, "randomize_states" },
		{ DNET_CFG_KEEPS_IDS_IN_CLUSTER, "keeps_ids_in_cluster" },
	};

	dnet_flags_dump_raw(buffer, sizeof(buffer), flags, infos, sizeof(infos) / sizeof(infos[0]));

	return buffer;
}

/*
 * New-style iterator control
 */
struct dnet_iterator_ctl {
	void				*iterate_private;
	void				*callback_private;
	int				(* callback)(void *priv, struct dnet_raw_id *key, uint64_t flags,
			int fd, uint64_t data_offset, uint64_t dsize, struct dnet_ext_list *elist);
};

/*
 * Iterator result container routines
 */
int dnet_iterator_response_container_sort(int fd, size_t size);
int dnet_iterator_response_container_append(const struct dnet_iterator_response
		*response, int fd, uint64_t pos);
int dnet_iterator_response_container_read(int fd, uint64_t pos,
		struct dnet_iterator_response *response);
int64_t dnet_iterator_response_container_diff(int diff_fd, int left_fd, uint64_t left_size,
		int right_fd, uint64_t right_size);

/*
 * This structure is used to get information about given key position
 * in the underlying backend. Backend will not send anything to client,
 * there is no @dnet_net_state for this.
 */
struct dnet_io_local
{
	uint8_t			key[DNET_ID_SIZE];

	/*
	 * @timestamp and @user_flags is used in extended headers in metadata writes.
	 */
	struct dnet_time	timestamp;
	uint64_t		user_flags;

	/*
	 * Total size of the object.
	 */
	uint64_t		total_size;

	/*
	 * Combination of DNET_RECORD_FLAGS_*
	 */
	uint64_t		record_flags;

	/*
	 * If object can be read from file, backend may put appropriate fd here.
	 */
	int			fd;
	uint64_t		fd_offset;

	/*
	 * This structure is not supposed to be transferred over the network,
	 * so reserved fields are not needed - all backends live in elliptics source tree.
	 *
	 * But if there will be any external backend which can not be recompiled with
	 * elliptics update, these fields may be useful.
	 */
	uint64_t		reserved[8];
};

struct dnet_access_context;

struct dnet_backend_callbacks {
	/* command handler processes DNET_CMD_* commands */
	int			(* command_handler)(void *state,
	                                            void *priv,
	                                            struct dnet_cmd *cmd,
	                                            void *data,
	                                            void *cmd_stats,
	                                            struct dnet_access_context *context);

	/* this must be provided as @priv argument to all above and below callbacks*/
	void			*command_private;

	/* fills storage statistics in json format */
	int			(* storage_stat_json)(void *priv, char **json_stat, size_t *size);

	/* returns total elements at backend */
	uint64_t		(* total_elements)(void *priv);

	/* cleanups backend at exit */
	void			(* backend_cleanup)(void *command_private);

	/*
	 * calculates checksum and writes (no more than *@csize bytes) it
	 * into @csum,
	 * @csize must be set to actual @csum size
	 */
	int			(* checksum)(struct dnet_node *n, void *priv, struct dnet_id *id, void *csum, int *csize);

	/*
	 * Iterator.
	 * Invokes callback on each record's data and metadata.
	 */
	int			(* iterator)(struct dnet_iterator_ctl *ictl,
			struct dnet_iterator_request *ireq, struct dnet_iterator_range *irange);

	int			(* defrag_status)(void *priv);
	int			(* defrag_start)(void *priv, enum dnet_backend_defrag_level level, const char* chunks_dir);
	int			(* defrag_stop)(void *priv);

	int			(* inspect_start)(void *priv);
	int			(* inspect_stop)(void *priv);
	int			(* inspect_status)(void *priv);

	/*
	 * Returns dir used by backend
	 */
	char *			(* dir)(void);

	int			(* lookup)(struct dnet_node *n, void *priv, struct dnet_io_local *io);

	/* command handler processes DNET_CMD_* commands protocol-undependently */
	int			(* n2_command_handler)(void *state,
	                                               void *priv,
	                                               struct n2_request_info *req_info,
	                                               void *cmd_stats,
	                                               struct dnet_access_context *context);
};

/*
 * Node configuration interface.
 */
struct dnet_config
{
	/*
	 * Family (AF_INET, AF_INET6) of the appropriate socket.
	 * These parameters are sent in the lookup replies so that remote nodes
	 * could know how to connect to this one.
	 */
	int			family;

	/*
	 * Socket port.
	 */
	int			port;

	/*
	 * Wait timeout in seconds used for example to wait
	 * for remote content sync.
	 */
	long			wait_timeout;

	/*
	 * Specifies whether given node will join the network,
	 * or it is a client node and its ID should not be checked
	 * against collision with others.
	 *
	 * Also has a bit to forbid route list download.
	 */
	int			flags;

	/* Private logger */
	dnet_logger		*log;

	/* Notify hash table size */
	unsigned int		hash_size;

	/*
	 * Wait until transaction acknowledge is received.
	 */
	long			check_timeout;

	/*
	 * Destroy state if stall_count transactions stalled.
	 */
	long			stall_count;

	/*
	 * Number of IO threads in processing pool
	 */
	int			io_thread_num;

	/*
	 * Number of IO threads in processing pool dedicated to non-blocking operations
	 * Those operations are started from recursive commands like from DNET_CMD_EXEC handler
	 */
	int			nonblocking_io_thread_num;

	/*
	 * Number of threads in network processing pool
	 */
	int			net_thread_num;

	/* IO nice parameters for background operations */
	int			bg_ionice_class;
	int			bg_ionice_prio;
	int			removal_delay;

	char			cookie[DNET_AUTH_COOKIE_SIZE];

	/* man 7 socket for IP_PRIORITY - priorities are set for joined (server) and others (client) connections */
	int			server_prio;
	int			client_prio;

	uint8_t			reconnect_batch_size;
	uint8_t			reserved_for_future_use_3[3];

	dnet_logger		*access_log;

	int			send_limit;

	int			reserved_for_future_use_2[4];

	/* Config file name for handystats library */
	const char 	*handystats_config;

	/* Unused. Will be removed at next major release */
	unsigned int	__unused_1;
	long		__unused_2;

	/* so that we do not change major version frequently */
	int			reserved_for_future_use[8];
};

struct dnet_node *dnet_get_node_from_state(void *state);

int __attribute__((weak)) dnet_session_set_groups(struct dnet_session *s, const int *groups, int group_num);
int *dnet_session_get_groups(struct dnet_session *s, int *count);

void dnet_session_set_trace_id(struct dnet_session *s, trace_id_t trace_id);
trace_id_t dnet_session_get_trace_id(struct dnet_session *s);

void dnet_session_set_trace_bit(struct dnet_session *s, int trace);
int dnet_session_get_trace_bit(struct dnet_session *s);

void dnet_session_set_ioflags(struct dnet_session *s, uint32_t ioflags);
uint32_t dnet_session_get_ioflags(struct dnet_session *s);

void dnet_session_set_cache_lifetime(struct dnet_session *s, uint64_t lifetime);
uint64_t dnet_session_get_cache_lifetime(struct dnet_session *s);

void dnet_session_set_cflags(struct dnet_session *s, uint64_t cflags);
uint64_t dnet_session_get_cflags(struct dnet_session *s);

void dnet_session_set_timestamp(struct dnet_session *s, const struct dnet_time *ts);
void dnet_session_get_timestamp(struct dnet_session *s, struct dnet_time *ts);

void dnet_session_set_json_timestamp(struct dnet_session *s, const struct dnet_time *ts);
void dnet_session_get_json_timestamp(struct dnet_session *s, struct dnet_time *ts);

struct dnet_id *dnet_session_get_direct_id(struct dnet_session *s);
void dnet_session_set_direct_id(struct dnet_session *s, const struct dnet_id *id);

const struct dnet_addr *dnet_session_get_direct_addr(struct dnet_session *s);
void dnet_session_set_direct_addr(struct dnet_session *s, const struct dnet_addr *addr);

uint32_t dnet_session_get_direct_backend(struct dnet_session *s);
void dnet_session_set_direct_backend(struct dnet_session *s, uint32_t backend_id);

void dnet_session_set_forward(struct dnet_session *s, const struct dnet_addr *addr);
const struct dnet_addr *dnet_session_get_forward(const struct dnet_session *s);

void dnet_session_set_user_flags(struct dnet_session *s, uint64_t user_flags);
uint64_t dnet_session_get_user_flags(struct dnet_session *s);

void dnet_session_set_timeout(struct dnet_session *s, long wait_timeout);
struct timespec *dnet_session_get_timeout(struct dnet_session *s);

void dnet_set_keepalive(struct dnet_node *n, int idle, int cnt, int interval);

int dnet_session_set_ns(struct dnet_session *s, const char *ns, int nsize);

struct dnet_node *dnet_session_get_node(struct dnet_session *s);

/*
 * Logging helpers.
 */

/*
 * Logging helpers used for the fine-printed address representation.
 */
static inline int dnet_addr_port(const struct dnet_addr *addr)
{
	if (addr->addr_len == sizeof(struct sockaddr_in) && addr->family == AF_INET) {
		struct sockaddr_in *in = (struct sockaddr_in *)addr->addr;
		return ntohs(in->sin_port);
	} else if (addr->addr_len == sizeof(struct sockaddr_in6) && addr->family == AF_INET6) {
		struct sockaddr_in6 *in = (struct sockaddr_in6 *)addr->addr;
		return ntohs(in->sin6_port);
	}
	return 0;
}

static inline const char *dnet_addr_host_string(const struct dnet_addr *addr)
{
	static __thread char __dnet_addr_host_string[128];
	int err;
	memset(__dnet_addr_host_string, 0, sizeof(__dnet_addr_host_string));

	err = getnameinfo((struct sockaddr *)addr->addr, addr->addr_len,
			__dnet_addr_host_string, sizeof(__dnet_addr_host_string),
			NULL, 0, NI_NUMERICHOST | NI_NUMERICSERV);
	if (err) {
		return "invalid address";
	}

	return __dnet_addr_host_string;
}

static inline const char *dnet_addr_string_raw(const struct dnet_addr *addr, char *inet_addr, int inet_size)
{
	memset(inet_addr, 0, inet_size);
	snprintf(inet_addr, inet_size, "%s:%d", dnet_addr_host_string(addr), dnet_addr_port(addr));
	return inet_addr;
}

static inline const char *dnet_addr_string(const struct dnet_addr *sa)
{
	static __thread char ___inet_addr[128];
	return dnet_addr_string_raw(sa, ___inet_addr, sizeof(___inet_addr));
}

struct dnet_addr *dnet_state_addr(struct dnet_net_state *st);
static inline const char *dnet_state_dump_addr(struct dnet_net_state *st)
{
	return dnet_addr_string(dnet_state_addr(st));
}

static inline const char *dnet_print_time(const struct dnet_time *t)
{
	char str[64];
	struct tm tm;

	static __thread char __dnet_print_time[128];

	localtime_r((time_t *)&t->tsec, &tm);
	strftime(str, sizeof(str), "%F %R:%S", &tm);

	snprintf(__dnet_print_time, sizeof(__dnet_print_time), "%s.%06llu", str, (long long unsigned) t->tnsec / 1000);
	return __dnet_print_time;
}

/*
 * Node creation/destruction callbacks. Node is a building block of the storage
 * and it is needed for every operation one may want to do with the network.
 */
struct dnet_node *dnet_node_create(struct dnet_config *);
void dnet_node_destroy(struct dnet_node *n);

/*
 * Create a session from node.
 * Session is not thread safe!
 */
struct dnet_session *dnet_session_create(struct dnet_node *n);
struct dnet_session *dnet_session_copy(struct dnet_session *s);
void dnet_session_destroy(struct dnet_session *s);

/* Server node creation/destruction.
 */
struct dnet_node *dnet_server_node_create(struct dnet_config_data *cfg_data);
void dnet_server_node_destroy(struct dnet_node *s);

/*
 * dnet_add_state() is used to add remote addresses into the route list, the more
 * routes are added the less network lookups will be performed to send/receive
 * data requests.
 */
int dnet_add_state(struct dnet_node *n, const struct dnet_addr *addr, int num, int flags);

/*
 * Converts address string into dnet_addr structure.
 * This optionally resolves DNS name.
 */
int dnet_create_addr(struct dnet_addr *addr, const char *addr_str, int port, int family);

/*
 * The same as @dnet_create_addr(), but parses @addr_str first.
 */
int dnet_create_addr_str(struct dnet_addr *addr, const char *addr_str, int addr_len);

/*
 * Returns number of states we are connected to.
 * It does not check whether they are alive though.
 */

int dnet_state_num(struct dnet_session *s);
int dnet_node_state_num(struct dnet_node *n);
struct dnet_net_state *dnet_state_search_by_addr(struct dnet_node *n, const struct dnet_addr *addr);
struct dnet_net_state *dnet_state_get_first(struct dnet_node *n, const struct dnet_id *id);
struct dnet_net_state *dnet_state_get_first_with_backend(struct dnet_node *n, const struct dnet_id *id, int *backend_id);
void dnet_state_put(struct dnet_net_state *st);

#define DNET_DUMP_NUM	6
#define DNET_DUMP_ID_LEN(name, id_struct, data_length) \
	char name[2 * DNET_ID_SIZE + 16 + 3]; \
	do { \
		char tmp[2 * DNET_ID_SIZE + 1]; \
		snprintf(name, sizeof(name), "%d:%s", (id_struct)->group_id, \
			dnet_dump_id_len_raw((id_struct)->id, (data_length), tmp)); \
	} while (0)
#define DNET_DUMP_ID(name, id_struct) DNET_DUMP_ID_LEN(name, id_struct, DNET_DUMP_NUM)
/*
 * Logging helper used to print ID (DNET_ID_SIZE bytes) as a hex string.
 */
static inline char *dnet_dump_id_len_raw(const unsigned char *id, unsigned int len, char *dst)
{
	static const char hex[] = "0123456789abcdef";

	unsigned int i;

	if (len > DNET_ID_SIZE)
		len = DNET_ID_SIZE;

	for (i=0; i<len; ++i) {
		dst[2*i  ] = hex[id[i] >>  4];
		dst[2*i+1] = hex[id[i] & 0xf];
	}
	dst[2*len] = '\0';
	return dst;
}

static inline char *dnet_dump_id_len(const struct dnet_id *id, unsigned int len)
{
	static __thread char __dnet_dump_str[2 * DNET_ID_SIZE + 16 + 3];
	char tmp[2*DNET_ID_SIZE + 1];
	char tmp2[2*DNET_ID_SIZE + 1];

	unsigned int len2 = (DNET_ID_SIZE - len) < len ? (DNET_ID_SIZE - len) : len;

	if (len < DNET_ID_SIZE)
		snprintf(__dnet_dump_str, sizeof(__dnet_dump_str),
		         "%d:%s...%s",
		         id->group_id,
		         dnet_dump_id_len_raw(id->id, len, tmp),
		         dnet_dump_id_len_raw(id->id + DNET_ID_SIZE - len2, len2, tmp2));
	else
		snprintf(__dnet_dump_str, sizeof(__dnet_dump_str),
		         "%d:%s",
		         id->group_id,
		         dnet_dump_id_len_raw(id->id, len, tmp));
	return __dnet_dump_str;
}

static inline char *dnet_dump_id(const struct dnet_id *id)
{
	return dnet_dump_id_len(id, DNET_DUMP_NUM);
}

static inline char *dnet_dump_id_str(const unsigned char *id)
{
	static __thread char __dnet_dump_id_str[2 * DNET_ID_SIZE + 1];
	return dnet_dump_id_len_raw(id, DNET_DUMP_NUM, __dnet_dump_id_str);
}

static inline char *dnet_dump_id_str_full(const unsigned char *id)
{
	static __thread char __dnet_dump_id_str_full[2 * DNET_ID_SIZE + 1];
	return dnet_dump_id_len_raw(id, DNET_ID_SIZE, __dnet_dump_id_str_full);
}

/*
 * Returns 0 if versions are equal.
 * negative of positive value if @st->version is smaller or bigger than @version one.
 */
int dnet_version_compare(struct dnet_net_state *st, int *version);

/*
 * Returns 0 if version are compatible.
 * Currently this means the 2 highest version fields are equal.
 *
 * Otherwise negative error code is returned.
 */
int dnet_version_check(struct dnet_net_state *st, int *version);

/*
 * Returns 4-ints version array.
 */
int *dnet_version(struct dnet_net_state *state);

/*!
 * Compares two dnet_time structs
 * Returns
 *	< 0 if t1 < t2
 *	> 0 if t1 > t2
 *	= 0 if t1 == t2
 */
static inline int dnet_time_cmp(const struct dnet_time *t1, const struct dnet_time *t2)
{
	if (t1->tsec < t2->tsec)
		return -1;
	else if (t1->tsec > t2->tsec)
		return 1;

	if (t1->tnsec < t2->tnsec)
		return -1;
	else if (t1->tnsec > t2->tnsec)
		return 1;

	return 0;
}

/*
 * Compare two IDs.
 * Returns  1 when id1 > id2
 *         -1 when id1 < id2
 *          0 when id1 = id2
 */
static inline int dnet_id_cmp_str(const unsigned char *id1, const unsigned char *id2)
{
	unsigned int i = 0;

	for (i*=sizeof(unsigned long); i<DNET_ID_SIZE; ++i) {
		if (id1[i] < id2[i])
			return -1;
		if (id1[i] > id2[i])
			return 1;
	}

	return 0;
}
static inline int dnet_id_cmp(const struct dnet_id *id1, const struct dnet_id *id2)
{
	if (id1->group_id < id2->group_id)
		return -1;
	if (id1->group_id > id2->group_id)
		return 1;

	return dnet_id_cmp_str(id1->id, id2->id);
}

/*
 * Request notifications when given ID is modified.
 * Notifications are sent after update was stored in the IO backend.
 * @id and @complete are not allowed to be NULL.
 *
 * @complete will be invoked each time object with given @id is modified.
 */
int dnet_request_notification(struct dnet_session *s, struct dnet_id *id,
	transaction_callback complete,
	void *priv);

/*
 * Drop notifications for given ID.
 */
int dnet_drop_notification(struct dnet_session *s, struct dnet_id *id);

/*
 * Low-level transaction allocation and sending function.
 */
struct dnet_trans_control
{
	struct dnet_id		id;

	unsigned int		cmd;
	uint64_t		cflags;

	void			*data;
	unsigned int		size;

	int			(* complete)(struct dnet_addr *addr, struct dnet_cmd *cmd, void *priv);
	void			*priv;
};

/*
 * Allocate and send transaction according to above control structure.
 */
int dnet_trans_alloc_send(struct dnet_session *s, struct dnet_trans_control *ctl);
void dnet_io_trans_alloc_send(struct dnet_session *s, struct dnet_io_control *ctl);
int dnet_trans_alloc_send_state(struct dnet_session *s, struct dnet_net_state *st, struct dnet_trans_control *ctl);
int dnet_trans_create_send_all(struct dnet_session *s, struct dnet_io_control *ctl);

int dnet_request_cmd(struct dnet_session *s, struct dnet_trans_control *ctl);

int dnet_fill_addr(struct dnet_addr *addr, const char *saddr, const int port, const int sock_type, const int proto);

/*
 * Transformation helper, which uses *ppos as an index for transformation function.
 * @src and @size correspond to to be transformed source data.
 * @dst and @dsize specify destination buffer.
 */
int dnet_transform(struct dnet_session *s, const void *src, uint64_t size, struct dnet_id *id);
int __attribute__((weak)) dnet_transform_node(struct dnet_node *n, const void *src, uint64_t size,
		unsigned char *csum, int csize);
int dnet_transform_raw(struct dnet_session *s, const void *src, uint64_t size, char *csum, unsigned int csize);
int dnet_transform_file(struct dnet_node *n, int fd, uint64_t offset, uint64_t size, char *csum, unsigned int csize);

/*
 * Transformation implementation, currently it's sha512 hash.
 * It calculates checksum for @src of @size and writes it to @id.
 */
int dnet_digest_transform(const void *src, uint64_t size, struct dnet_id *id);
/*
 * @dnet_digest_transform overload.
 * Writes most of @csum_size bytes to @csum.
 */
int dnet_digest_transform_raw(const void *src, uint64_t size, void *csum, int csum_size);

/*
 * Calculates message autherization code based on digest_transformation.
 * Uses data from @src of @size and @key of size @key_size. Result is written to @id.
 */
int dnet_digest_auth_transform(const void *src, uint64_t size, const void *key, uint64_t key_size, struct dnet_id *id);
/*
 * @dnet_digest_auth_transform overload.
 * Writes most of @csum_size bytes to @csum.
 */
int dnet_digest_auth_transform_raw(const void *src, uint64_t size, const void *key, uint64_t key_size, void *csum, int csum_size);

int dnet_lookup_addr(struct dnet_session *s, const void *remote, int len, const struct dnet_id *id, int group_id,
		struct dnet_addr *addr, int *backend);

struct dnet_id_param {
	unsigned int		group_id;
	uint64_t		param;
	uint64_t		param_reserved;
} __attribute__ ((packed));

enum id_params {
	DNET_ID_PARAM_LA = 1,
	DNET_ID_PARAM_FREE_SPACE,
};

struct dnet_check_reply {
	int			total;
	int			completed;
	int			errors;
	int			reserved[5];
};

static inline void dnet_convert_check_reply(struct dnet_check_reply *r)
{
	r->total = dnet_bswap32(r->total);
	r->completed = dnet_bswap32(r->completed);
	r->errors = dnet_bswap32(r->errors);
}

/* Set by dnet_check when we only want to merge transaction
 * and do not check copies in other groups
 */
#define DNET_CHECK_MERGE			(1<<0)
/* Check not only history presence but also try to read part of the data object */
#define DNET_CHECK_FULL				(1<<1)
/* Do not actually perform any action, just update counters */
#define DNET_CHECK_DRY_RUN			(1<<2)
/* Physically delete files marked as REMOVED in history */
#define DNET_CHECK_DELETE			(1<<3)

struct dnet_check_request {
	uint32_t		flags;
	uint32_t		thread_num;
	uint64_t		timestamp;
	uint64_t		updatestamp_start;
	uint64_t		updatestamp_stop;
	uint32_t		obj_num;
	uint32_t		group_num;
	int			blob_start;
	int			blob_num;
	uint64_t		reserved;
} __attribute__ ((packed));

static inline void dnet_convert_check_request(struct dnet_check_request *r)
{
	r->flags = dnet_bswap32(r->flags);
	r->thread_num = dnet_bswap32(r->thread_num);
	r->timestamp = dnet_bswap64(r->timestamp);
	r->updatestamp_start = dnet_bswap64(r->updatestamp_start);
	r->updatestamp_stop = dnet_bswap64(r->updatestamp_stop);
	r->obj_num = dnet_bswap32(r->obj_num);
	r->group_num = dnet_bswap32(r->group_num);
	r->blob_start = dnet_bswap32(r->blob_start);
	r->blob_num = dnet_bswap32(r->blob_num);
}

int dnet_request_check(struct dnet_session *s, struct dnet_check_request *r);

long __attribute__((weak)) dnet_get_id(void);

static inline int is_trans_destroyed(struct dnet_cmd *cmd)
{
	int ret = 0;

	if (!cmd || (cmd->flags & DNET_FLAGS_DESTROY)) {
		ret = 1;
		if (cmd && cmd->status)
			ret = cmd->status;
	}

	return ret;
}

int dnet_mix_states(struct dnet_session *s, struct dnet_id *id, uint32_t ioflags, int **groupsp);

char * __attribute__((weak)) dnet_cmd_string(int cmd);
const char *dnet_backend_command_string(uint32_t cmd);
const char *dnet_backend_state_string(uint32_t state);
const char *dnet_backend_defrag_state_string(uint32_t state);
const char *dnet_backend_inspect_state_string(uint32_t state);

int dnet_checksum_file(struct dnet_node *n, const char *file, uint64_t offset, uint64_t size, void *csum, int csize);
int dnet_checksum_fd(struct dnet_node *n, int fd, uint64_t offset, uint64_t size, void *csum, int csize);
int dnet_checksum_data(struct dnet_node *n, const void *data, uint64_t size, unsigned char *csum, int csize);

int dnet_fd_readlink(int fd, char **datap);

int dnet_send_file_info(void *state, struct dnet_cmd *cmd, int fd, uint64_t offset, int64_t size);
int dnet_send_file_info_without_fd(void *state, struct dnet_cmd *cmd, const void *data, int64_t size);
int dnet_send_file_info_ts(void *state, struct dnet_cmd *cmd, int fd,
                           uint64_t offset, int64_t size, struct dnet_time *timestamp, uint64_t record_flags);
int dnet_send_file_info_ts_without_fd(void *state, struct dnet_cmd *cmd, const void *data, int64_t size, const struct dnet_time *timestamp);


/*
 * Send given number of bytes as reply command.
 * It will fill transaction, command and ID from the original command and copy given data.
 * It will set DNET_FLAGS_MORE if original command requested acknowledge or @more is set.
 */
int dnet_send_reply(void *state,
                    struct dnet_cmd *cmd,
                    const void *odata,
                    unsigned int size,
                    int more,
                    struct dnet_access_context  *context);
int dnet_send_reply_threshold(void *state, struct dnet_cmd *cmd,
		const void *odata, unsigned int size, int more);

int dnet_send_fd_threshold(struct dnet_net_state *st, void *header, uint64_t hsize,
                           int fd, uint64_t offset, uint64_t dsize);

int n2_send_error_response(struct dnet_net_state *st,
                           struct n2_request_info *req_info,
                           int errc);

struct dnet_route_entry
{
	struct dnet_raw_id id;
	struct dnet_addr addr;
	int group_id;
	uint32_t backend_id;
};

int dnet_get_routes(struct dnet_session *s, struct dnet_route_entry **entries);

int dnet_flags(struct dnet_node *n);
void dnet_set_timeouts(struct dnet_node *n, long wait_timeout, long check_timeout);

#define DNET_CONF_ADDR_DELIM	':'
int dnet_parse_addr(char *addr, int *portp, int *familyp);

int dnet_parse_numeric_id(const char *value, unsigned char *id);

struct dnet_vm_stat {
	uint16_t	la[3];
	uint64_t	vm_active;
	uint64_t	vm_inactive;
	uint64_t	vm_total;
	uint64_t	vm_free;
	uint64_t	vm_cached;
	uint64_t	vm_buffers;
};

int dnet_get_vm_stat(dnet_logger *l, struct dnet_vm_stat *st);

struct dnet_server_send_ctl;

// all other fields of @dnet_server_send_ctl must be set manually, no need to increase number of parameters
// groups are here since they will be allocated as one chunk with control structure itself
struct dnet_server_send_ctl *dnet_server_send_alloc(void *state, struct dnet_cmd *cmd, int *groups, int group_num);
struct dnet_server_send_ctl *dnet_server_send_get(struct dnet_server_send_ctl *ctl);
int dnet_server_send_put(struct dnet_server_send_ctl *ctl);
int dnet_server_send_write(struct dnet_server_send_ctl *send,
		struct dnet_iterator_response *re, uint64_t dsize,
		int fd, uint64_t data_offset);

static inline const char *dnet_print_io(const struct dnet_io_attr *io) {
	static __thread char __dnet_print_io[256];
	snprintf(__dnet_print_io, sizeof(__dnet_print_io),
	         "io-flags: %s, "
	         "io-offset: %llu, "
	         "io-size: %llu/%llu, "
	         "io-user-flags: 0x%llx, "
	         "io-num: %llu, "
	         "ts: '%s'",
	         dnet_flags_dump_ioflags(io->flags),
	         (unsigned long long)io->offset,
	         (unsigned long long)io->size, (unsigned long long)io->total_size,
	         (unsigned long long)io->user_flags,
	         (unsigned long long)io->num,
	         dnet_print_time(&io->timestamp));
	return __dnet_print_io;
}

static inline const char *dnet_print_error(int err) {
	static __thread char __dnet_print_error[256];
	snprintf(__dnet_print_error, sizeof(__dnet_print_error),
	         "%s [%d]",
	         strerror(-err), err);
	return __dnet_print_error;
}

#ifdef __cplusplus
}
#endif

#endif /* __DNET_INTERFACE_H */
