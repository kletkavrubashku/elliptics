/*
 * 2008+ Copyright (c) Evgeniy Polyakov <zbr@ioremap.net>
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/syscall.h>

#include <ctype.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <netinet/in.h>

#include "elliptics/cppdef.h"

#include "backends.h"
#include "common.h"

using namespace ioremap::elliptics;

#ifndef __unused
#define __unused	__attribute__ ((unused))
#endif

static void dnet_usage(char *p)
{
	fprintf(stderr, "Usage: %s\n"
			" -r addr:port:family  - adds a route to the given node\n"
			" -W file              - write given file to the network storage\n"
			" -s                   - request IO counter stats from node\n"
			" -z                   - request VFS IO stats from node\n"
			" -a                   - request stats from all connected nodes\n"
			" -U status            - update server status: 1 - elliptics exits, 2 - goes RO\n"
			" -R file              - read given file from the network into the local storage\n"
			" -I id                - transaction id (used to read data)\n"
			" -g groups            - group IDs to connect\n"
			" -c cmd-event         - execute command with given event on the remote node\n"
			" -L file              - lookup a storage which hosts given file\n"
			" -l log               - log file. Default: disabled\n"
			" -w timeout           - wait timeout in seconds used to wait for content sync.\n"
			" ...                  - parameters can be repeated multiple times\n"
			"                        each time they correspond to the last added node\n"
			" -m level             - log level\n"
			" -M level             - set new log level\n"
			" -F flags             - change node flags (see @cfg->flags comments in include/elliptics/interface.h)\n"
			" -O offset            - read/write offset in the file\n"
			" -S size              - read/write transaction size\n"
			" -u file              - unlink file\n"
			" -N namespace         - use this namespace for operations\n"
			" -D object            - read latest data for given object, if -I id is specified, this field is unused\n"
			" -C flags             - command flags\n"
			" -t column            - column ID to read or write\n"
			" -d                   - start defragmentation\n"
			" -i flags             - IO flags (see DNET_IO_FLAGS_* in include/elliptics/packet.h\n"
			, p);
}

key create_id(unsigned char *id, const char *file_name, int type)
{
	if (id) {
		struct dnet_id raw;

		dnet_setup_id(&raw, 0, id);
		raw.type = type;

		return raw;
	} else {
		return key(file_name, type);
	}
}

int main(int argc, char *argv[])
{
	int ch, err, i, have_remote = 0;
	int io_counter_stat = 0, vfs_stat = 0, single_node_stat = 1;
	struct dnet_node_status node_status;
	int update_status = 0;
	struct dnet_config cfg, rem, *remotes = NULL;
	const char *logfile = "/dev/stderr", *readf = NULL, *writef = NULL, *cmd = NULL, *lookup = NULL;
	const char *read_data = NULL;
	char *removef = NULL;
	unsigned char trans_id[DNET_ID_SIZE], *id = NULL;
	uint64_t offset, size;
	std::vector<int> groups;
	int type = EBLOB_TYPE_DATA;
	uint64_t cflags = 0;
	uint64_t ioflags = 0;
	int defrag = 0;
	sigset_t mask;

	memset(&node_status, 0, sizeof(struct dnet_node_status));
	memset(&cfg, 0, sizeof(struct dnet_config));

	node_status.nflags = -1;
	node_status.status_flags = -1;
	node_status.log_level = ~0U;

	size = offset = 0;

	cfg.sock_type = SOCK_STREAM;
	cfg.proto = IPPROTO_TCP;
	cfg.wait_timeout = 60;
	int log_level = DNET_LOG_ERROR;

	memcpy(&rem, &cfg, sizeof(struct dnet_config));

	while ((ch = getopt(argc, argv, "i:dC:t:A:F:M:N:g:u:O:S:m:zsU:aL:w:l:c:I:r:W:R:D:h")) != -1) {
		switch (ch) {
			case 'i':
				ioflags = strtoull(optarg, NULL, 0);
				break;
			case 'd':
				defrag = 1;
				break;
			case 'C':
				cflags = strtoull(optarg, NULL, 0);
				break;
			case 't':
				type = atoi(optarg);
				break;
			case 'F':
				node_status.nflags = strtol(optarg, NULL, 0);
				update_status = 1;
				break;
			case 'M':
				node_status.log_level = atoi(optarg);
				update_status = 1;
				break;
			case 'N':
				cfg.ns = optarg;
				cfg.nsize = strlen(optarg);
				break;
			case 'u':
				removef = optarg;
				break;
			case 'O':
				offset = strtoull(optarg, NULL, 0);
				break;
			case 'S':
				size = strtoull(optarg, NULL, 0);
				break;
			case 'm':
				log_level = atoi(optarg);
				break;
			case 's':
				io_counter_stat = 1;
				break;
			case 'U':
				node_status.status_flags = strtol(optarg, NULL, 0);
				update_status = 1;
				break;
			case 'z':
				vfs_stat = 1;
				break;
			case 'a':
				single_node_stat = 0;
				break;
			case 'L':
				lookup = optarg;
				break;
			case 'w':
				cfg.check_timeout = cfg.wait_timeout = atoi(optarg);
				break;
			case 'l':
				logfile = optarg;
				break;
			case 'c':
				cmd = optarg;
				break;
			case 'I':
				err = dnet_parse_numeric_id(optarg, trans_id);
				if (err)
					return err;
				id = trans_id;
				break;
			case 'g': {
				int *groups_tmp = NULL, group_num = 0;
				group_num = dnet_parse_groups(optarg, &groups_tmp);
				if (group_num <= 0)
					return -1;
				groups.assign(groups_tmp, groups_tmp + group_num);
				free(groups_tmp);
				break;
			}
			case 'r':
				err = dnet_parse_addr(optarg, &rem);
				if (err)
					return err;
				have_remote++;
				remotes = reinterpret_cast<dnet_config*>(
					realloc(remotes, sizeof(rem) * have_remote));
				if (!remotes)
					return -ENOMEM;
				memcpy(&remotes[have_remote - 1], &rem, sizeof(rem));
				break;
			case 'W':
				writef = optarg;
				break;
			case 'R':
				readf = optarg;
				break;
			case 'D':
				read_data = optarg;
				break;
			case 'h':
			default:
				dnet_usage(argv[0]);
				return -1;
		}
	}

	try {
		file_logger log(logfile, log_level);

		node n(log, cfg);
		session s(n);

		s.set_cflags(cflags);
		s.set_ioflags(ioflags);

		sigemptyset(&mask);
		sigaddset(&mask, SIGTERM);
		sigaddset(&mask, SIGINT);
		sigaddset(&mask, SIGHUP);
		sigaddset(&mask, SIGCHLD);
		pthread_sigmask(SIG_UNBLOCK, &mask, NULL);
		sigprocmask(SIG_UNBLOCK, &mask, NULL);

		if (have_remote) {
			int error = -ECONNRESET;
			for (i = 0; i < have_remote; ++i) {
				if (single_node_stat && (vfs_stat || io_counter_stat))
					remotes[i].flags = DNET_CFG_NO_ROUTE_LIST;
				err = dnet_add_state(n.get_native(), &remotes[i]);
				if (!err)
					error = 0;
			}

			if (error)
				return error;
		}

		s.set_groups(groups);

		if (defrag)
			return dnet_start_defrag(s.get_native(), cflags);

		if (writef)
			s.write_file(create_id(id, writef, type), writef, offset, offset, size);

		if (readf)
			s.read_file(create_id(id, readf, type), readf, offset, size);

		if (read_data) {
			read_result result = s.read_latest(create_id(id, read_data, type), offset, 0);

			data_pointer file = result->file();

			while (!file.empty()) {
				err = write(1, file.data(), file.size());
				if (err <= 0) {
					err = -errno;
					throw_error(err, "%s: can not write data to stdout", read_data);
					return err;
				}
				file.skip(err);
			}
		}

		if (removef)
			s.remove(create_id(id, removef, type));

		if (cmd) {
			struct dnet_id __did, *did = NULL;
			struct sph *sph;
			int len = strlen(cmd);
			int event_size = len;
			char *ret = NULL;
			const char *tmp;

			tmp = strchr(cmd, ' ');
			if (tmp) {
				event_size = tmp - cmd;
			}

			if (id) {
				did = &__did;

				dnet_setup_id(did, 0, id);
				did->type = type;
			}

			sph = reinterpret_cast<struct sph*>(malloc(sizeof(struct sph) + len + 1));
			if (!sph)
				return -ENOMEM;

			memset(sph, 0, sizeof(struct sph));

			sph->flags = DNET_SPH_FLAGS_SRC_BLOCK;
			sph->key = -1;
			sph->binary_size = 0;
			sph->data_size = len - event_size;
			sph->event_size = event_size;

			sprintf(sph->data, "%s", cmd);

			err = dnet_send_cmd(s.get_native(), did, sph, (void **)&ret);
			if (err < 0)
				return err;

			free(sph);

			if (err > 0) {
				printf("%.*s\n", err, ret);
				free(ret);
			}
		}

		if (lookup)
			s.lookup(std::string(lookup));

		if (vfs_stat)
			s.stat_log();

		if (io_counter_stat)
			s.stat_log_count();

		if (update_status) {
			struct dnet_addr addr;

			for (i=0; i<have_remote; ++i) {
				memset(&addr, 0, sizeof(addr));
				addr.addr_len = sizeof(addr.addr);

				err = dnet_fill_addr(&addr, remotes[i].addr, remotes[i].port,
							remotes[i].family, remotes[i].sock_type, remotes[i].proto);
				if (err) {
					dnet_log_raw(n.get_native(), DNET_LOG_ERROR, "ioclient: dnet_fill_addr: %s:%s:%d, sock_type: %d, proto: %d: %s %d\n",
							remotes[i].addr, remotes[i].port,
							remotes[i].family, remotes[i].sock_type, remotes[i].proto,
							strerror(-err), err);
				}

				err = dnet_update_status(s.get_native(), &addr, NULL, &node_status);
				if (err) {
					dnet_log_raw(n.get_native(), DNET_LOG_ERROR, "ioclient: dnet_update_status: %s:%s:%d, sock_type: %d, proto: %d: update: %d: "
							"%s %d\n",
							remotes[i].addr, remotes[i].port,
							remotes[i].family, remotes[i].sock_type, remotes[i].proto, update_status,
							strerror(-err), err);
				}
			}

		}
	} catch (const std::exception &e) {
		std::cerr << e.what() << std::endl;
	}

	return 0;
}

