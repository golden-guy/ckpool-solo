/*
 * Copyright 2014 Con Kolivas
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 */

#ifndef CKPOOL_H
#define CKPOOL_H

#include "config.h"

#include <sys/file.h>

#include "libckpool.h"
#include "uthash.h"

struct ckpool_instance;
typedef struct ckpool_instance ckpool_t;

struct ckmsg {
	struct ckmsg *next;
	struct ckmsg *prev;
	void *data;
};

typedef struct ckmsg ckmsg_t;

struct ckmsgq {
	ckpool_t *ckp;
	char name[16];
	pthread_t pth;
	pthread_mutex_t lock;
	pthread_cond_t cond;
	ckmsg_t *msgs;
	void (*func)(ckpool_t *, void *);
};

typedef struct ckmsgq ckmsgq_t;

struct proc_instance {
	ckpool_t *ckp;
	unixsock_t us;
	char *processname;
	char *sockname;
	int pid;
	int (*process)(proc_instance_t *);
};

struct connsock {
	int fd;
	char *url;
	char *port;
	char *auth;
	char *buf;
	int bufofs;
	int buflen;
};

typedef struct connsock connsock_t;

struct server_instance {
	/* Hash table data */
	UT_hash_handle hh;
	int id;

	char *url;
	char *auth;
	char *pass;
	connsock_t cs;

	void *data; // Private data
};

typedef struct server_instance server_instance_t;

struct ckpool_instance {
	/* Filename of config file */
	char *config;
	/* Kill old instance with same name */
	bool killold;
	/* Logging level */
	int loglevel;
	/* Main process name */
	char *name;
	/* Directory where sockets are created */
	char *socket_dir;
	/* Directory where ckdb sockets are */
	char *ckdb_sockdir;
	/* Name of the ckdb process */
	char *ckdb_name;
	char *ckdb_sockname;
	/* Group ID for unix sockets */
	char *grpnam;
	gid_t gr_gid;
	/* Directory where logs are written */
	char *logdir;
	/* Logfile */
	FILE *logfp;
	int logfd;

	/* Logger message queue NOTE: Unique per process */
	ckmsgq_t *logger;
	/* Process instance data of parent/child processes */
	proc_instance_t main;

	int proc_instances;
	proc_instance_t **children;

	proc_instance_t *generator;
	proc_instance_t *stratifier;
	proc_instance_t *connector;

	/* Threads of main process */
	pthread_t pth_listener;
	pthread_t pth_watchdog;

	/* Are we running as a proxy */
	bool proxy;

	/* Are we running without ckdb */
	bool standalone;

	/* Bitcoind data */
	int btcds;
	char **btcdurl;
	char **btcdauth;
	char **btcdpass;
	int blockpoll; // How frequently in ms to poll bitcoind for block updates

	/* Difficulty settings */
	int mindiff; // Default 1
	int startdiff; // Default 42

	/* Coinbase data */
	char *btcaddress; // Address to mine to
	char *btcsig; // Optional signature to add to coinbase

	/* Stratum options */
	server_instance_t **servers;
	char *serverurl; // URL to bind our server/proxy to
	int update_interval; // Seconds between stratum updates
	int chosen_server; // Chosen server for next connection

	/* Proxy options */
	int proxies;
	char **proxyurl;
	char **proxyauth;
	char **proxypass;
};

ckmsgq_t *create_ckmsgq(ckpool_t *ckp, const char *name, const void *func);
void ckmsgq_add(ckmsgq_t *ckmsgq, void *data);

ckpool_t *global_ckp;

bool ping_main(ckpool_t *ckp);
int read_socket_line(connsock_t *cs, int timeout);
bool _send_proc(proc_instance_t *pi, const char *msg, const char *file, const char *func, const int line);
#define send_proc(pi, msg) _send_proc(pi, msg, __FILE__, __func__, __LINE__)
char *_send_recv_proc(proc_instance_t *pi, const char *msg, const char *file, const char *func, const int line);
#define send_recv_proc(pi, msg) _send_recv_proc(pi, msg, __FILE__, __func__, __LINE__)
char *_send_recv_ckdb(const ckpool_t *ckp, const char *msg, const char *file, const char *func, const int line);
#define send_recv_ckdb(ckp, msg) _send_recv_ckdb(ckp, msg, __FILE__, __func__, __LINE__)
char *_json_ckdb_call(const ckpool_t *ckp, const char *idmsg, json_t *val, bool logged,
		      const char *file, const char *func, const int line);
#define json_ckdb_call(ckp, idmsg, val, logged) _json_ckdb_call(ckp, idmsg, val, logged, __FILE__, __func__, __LINE__)

json_t *json_rpc_call(connsock_t *cs, const char *rpc_req);

int process_exit(ckpool_t *ckp, proc_instance_t *pi, int ret);

#endif /* CKPOOL_H */
