/*
 * Copyright 2014 Con Kolivas
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 */

#include "config.h"

#include <sys/socket.h>
#include <jansson.h>
#include <string.h>
#include <unistd.h>

#include "ckpool.h"
#include "libckpool.h"
#include "generator.h"
#include "bitcoin.h"
#include "uthash.h"
#include "utlist.h"

struct notify_instance {
	/* Hash table data */
	UT_hash_handle hh;
	int id;

	char prevhash[68];
	char *jobid;
	char *coinbase1;
	char *coinbase2;
	int merkles;
	char merklehash[16][68];
	char nbit[12];
	char ntime[12];
	char bbversion[12];
	bool clean;

	time_t notify_time;
};

typedef struct notify_instance notify_instance_t;

struct share_msg {
	UT_hash_handle hh;
	int id; // Our own id for submitting upstream

	int client_id;
	int msg_id; // Stratum message id from client
	time_t submit_time;
};

typedef struct share_msg share_msg_t;

struct stratum_msg {
	struct stratum_msg *next;
	struct stratum_msg *prev;

	json_t *json_msg;
	int client_id;
};

typedef struct stratum_msg stratum_msg_t;

/* Per proxied pool instance data */
struct proxy_instance {
	ckpool_t *ckp;
	connsock_t *cs;
	server_instance_t *si;

	const char *auth;
	const char *pass;

	char *enonce1;
	char *enonce1bin;
	int nonce1len;
	char *sessionid;
	int nonce2len;

	tv_t last_message;

	double diff;
	int absolute_shares;
	int diff_shares;
	tv_t last_share;

	int id; /* Message id for sending stratum messages */
	bool no_sessionid; /* Doesn't support session id resume on subscribe */
	bool no_params; /* Doesn't want any parameters on subscribe */

	bool notified; /* Received new template for work */
	bool diffed; /* Received new diff */
	bool reconnect; /* We need to drop and reconnect */

	pthread_mutex_t notify_lock;
	notify_instance_t *notify_instances;
	notify_instance_t *current_notify;
	int notify_id;

	pthread_t pth_precv;
	pthread_t pth_psend;
	pthread_mutex_t psend_lock;
	pthread_cond_t psend_cond;

	stratum_msg_t *psends;

	pthread_mutex_t share_lock;
	share_msg_t *shares;
	int share_id;
};

typedef struct proxy_instance proxy_instance_t;

static server_instance_t *live_server(ckpool_t *ckp)
{
	server_instance_t *alive = NULL;
	connsock_t *cs;
	int i;

	LOGDEBUG("Attempting to connect to bitcoind");
retry:
	if (!ping_main(ckp))
		goto out;

	for (i = 0; i < ckp->btcds; i++) {
		server_instance_t *si;
		char *userpass = NULL;
		gbtbase_t *gbt;

		si = ckp->servers[i];
		cs = &si->cs;
		if (!extract_sockaddr(si->url, &cs->url, &cs->port)) {
			LOGWARNING("Failed to extract address from %s", si->url);
			continue;
		}
		userpass = strdup(si->auth);
		realloc_strcat(&userpass, ":");
		realloc_strcat(&userpass, si->pass);
		cs->auth = http_base64(userpass);
		dealloc(userpass);
		if (!cs->auth) {
			LOGWARNING("Failed to create base64 auth from %s", userpass);
			continue;
		}

		cs->fd = connect_socket(cs->url, cs->port);
		if (cs->fd < 0) {
			LOGWARNING("Failed to connect socket to %s:%s !", cs->url, cs->port);
			continue;
		}

		keep_sockalive(cs->fd);

		/* Test we can connect, authorise and get a block template */
		gbt = si->data;
		if (!gen_gbtbase(cs, gbt)) {
			LOGINFO("Failed to get test block template from %s:%s auth %s !",
				cs->url, cs->port, userpass);
			continue;
		}
		clear_gbtbase(gbt);
		if (!validate_address(cs, ckp->btcaddress)) {
			LOGWARNING("Invalid btcaddress: %s !", ckp->btcaddress);
			continue;
		}
		alive = si;
		break;
	}
	if (!alive) {
		LOGWARNING("CRITICAL: No bitcoinds active!");
		sleep(5);
		goto retry;
	}
	cs = &alive->cs;
	LOGINFO("Connected to live server %s:%s", cs->url, cs->port);
out:
	send_proc(ckp->connector, alive ? "accept" : "reject");
	return alive;
}

static void kill_server(server_instance_t *si)
{
	if (!si)
		return;
	close(si->cs.fd);
	si->cs.fd = -1;
}

static int gen_loop(proc_instance_t *pi)
{
	int sockd = -1, ret = 0, selret;
	server_instance_t *si = NULL;
	unixsock_t *us = &pi->us;
	ckpool_t *ckp = pi->ckp;
	char *buf = NULL;
	connsock_t *cs;
	gbtbase_t *gbt;
	char hash[68];

reconnect:
	if (si)
		kill_server(si);
	si = live_server(ckp);
	if (!si)
		goto out;

	gbt = si->data;
	cs = &si->cs;

retry:
	do {
		selret = wait_read_select(us->sockd, 5);
		if (!selret && !ping_main(ckp)) {
			LOGEMERG("Generator failed to ping main process, exiting");
			ret = 1;
			goto out;
		}
	} while (selret < 1);

	sockd = accept(us->sockd, NULL, NULL);
	if (sockd < 0) {
		LOGEMERG("Failed to accept on generator socket");
		ret = 1;
		goto out;
	}

	dealloc(buf);
	buf = recv_unix_msg(sockd);
	if (!buf) {
		LOGWARNING("Failed to get message in gen_loop");
		close(sockd);
		goto retry;
	}
	LOGDEBUG("Generator received request: %s", buf);
	if (cmdmatch(buf, "shutdown")) {
		ret = 0;
		goto out;
	}
	if (cmdmatch(buf, "getbase")) {
		if (!gen_gbtbase(cs, gbt)) {
			LOGWARNING("Failed to get block template from %s:%s",
				   cs->url, cs->port);
			send_unix_msg(sockd, "Failed");
			goto reconnect;
		} else {
			char *s = json_dumps(gbt->json, 0);

			send_unix_msg(sockd, s);
			free(s);
			clear_gbtbase(gbt);
		}
	} else if (cmdmatch(buf, "getbest")) {
		if (!get_bestblockhash(cs, hash)) {
			LOGINFO("No best block hash support from %s:%s",
				cs->url, cs->port);
			send_unix_msg(sockd, "Failed");
		} else
			send_unix_msg(sockd, hash);
	} else if (cmdmatch(buf, "getlast")) {
		int height = get_blockcount(cs);

		if (height == -1) {
			send_unix_msg(sockd,  "Failed");
			goto reconnect;
		} else {
			LOGDEBUG("Height: %d", height);
			if (!get_blockhash(cs, height, hash)) {
				send_unix_msg(sockd, "Failed");
				goto reconnect;
			} else {
				send_unix_msg(sockd, hash);
				LOGDEBUG("Hash: %s", hash);
			}
		}
	} else if (cmdmatch(buf, "submitblock:")) {
		LOGNOTICE("Submitting block data!");
		if (submit_block(cs, buf + 12))
			send_proc(ckp->stratifier, "block");
	} else if (cmdmatch(buf, "loglevel")) {
		sscanf(buf, "loglevel=%d", &ckp->loglevel);
	} else if (cmdmatch(buf, "ping")) {
		LOGDEBUG("Generator received ping request");
		send_unix_msg(sockd, "pong");
	}
	close(sockd);
	goto retry;

out:
	kill_server(si);
	dealloc(buf);
	return ret;
}

static bool send_json_msg(connsock_t *cs, json_t *json_msg)
{
	int len, sent;
	char *s;

	s = json_dumps(json_msg, JSON_ESCAPE_SLASH);
	LOGDEBUG("Sending json msg: %s", s);
	len = strlen(s);
	sent = write_socket(cs->fd, s, len);
	len += 1;
	dealloc(s);
	sent += write_socket(cs->fd, "\n", 1);
	if (sent != len) {
		LOGWARNING("Failed to send %d bytes sent %d in send_json_msg", len, sent);
		return false;
	}
	return true;
}

static bool connect_proxy(connsock_t *cs)
{
	cs->fd = connect_socket(cs->url, cs->port);
	if (cs->fd < 0) {
		LOGWARNING("Failed to connect socket to %s:%s in connect_proxy",
			   cs->url, cs->port);
		return false;
	}
	keep_sockalive(cs->fd);
	return true;
}

/* Decode a string that should have a json message and return just the contents
 * of the result key or NULL. */
static json_t *json_result(json_t *val)
{
	json_t *res_val = NULL, *err_val;

	res_val = json_object_get(val, "result");
	if (json_is_null(res_val))
		res_val = NULL;
	if (!res_val) {
		char *ss;

		err_val = json_object_get(val, "error");
		if (err_val)
			ss = json_dumps(err_val, 0);
		else
			ss = strdup("(unknown reason)");

		LOGWARNING("JSON-RPC decode failed: %s", ss);
		free(ss);
	}
	return res_val;
}

/* Parse a string and return the json value it contains, if any, and the
 * result in res_val. Return NULL if no result key is found. */
static json_t *json_msg_result(char *msg, json_t **res_val)
{
	json_error_t err;
	json_t *val;

	*res_val = NULL;
	val = json_loads(msg, 0, &err);
	if (!val) {
		LOGWARNING("Json decode failed(%d): %s", err.line, err.text);
		goto out;
	}
	*res_val = json_result(val);
	if (!*res_val) {
		LOGWARNING("No json result found");
		json_decref(val);
		val = NULL;
	}

out:
	return val;
}

/* For some reason notify is buried at various different array depths so use
 * a reentrant function to try and find it. */
static json_t *find_notify(json_t *val)
{
	int arr_size, i;
	json_t *ret = NULL;
	const char *entry;

	if (!json_is_array(val))
		return NULL;
	arr_size = json_array_size(val);
	entry = json_string_value(json_array_get(val, 0));
	if (cmdmatch(entry, "mining.notify"))
		return val;
	for (i = 0; i < arr_size; i++) {
		json_t *arr_val;

		arr_val = json_array_get(val, i);
		ret = find_notify(arr_val);
		if (ret)
			break;
	}
	return ret;
}

static bool parse_subscribe(connsock_t *cs, proxy_instance_t *proxi)
{
	json_t *val = NULL, *res_val, *notify_val, *tmp;
	const char *string;
	bool ret = false;
	char *old;
	int size;

	size = read_socket_line(cs, 5);
	if (size < 1) {
		LOGWARNING("Failed to receive line in parse_subscribe");
		goto out;
	}
	val = json_msg_result(cs->buf, &res_val);
	if (!val) {
		LOGWARNING("Failed to get a json result in parse_subscribe, got: %s", cs->buf);
		goto out;
	}
	if (!json_is_array(res_val)) {
		LOGWARNING("Result in parse_subscribe not an array");
		goto out;
	}
	size = json_array_size(res_val);
	if (size < 3) {
		LOGWARNING("Result in parse_subscribe array too small");
		goto out;
	}
	notify_val = find_notify(res_val);
	if (!notify_val) {
		LOGWARNING("Failed to find notify in parse_subscribe");
		goto out;
	}
	/* Free up old data in place if we are re-subscribing */
	old = proxi->sessionid;
	proxi->sessionid = NULL;
	if (!proxi->no_params && !proxi->no_sessionid && json_array_size(notify_val) > 1) {
		/* Copy the session id if one exists. */
		string = json_string_value(json_array_get(notify_val, 1));
		if (string)
			proxi->sessionid = strdup(string);
	}
	free(old);
	tmp = json_array_get(res_val, 1);
	if (!tmp || !json_is_string(tmp)) {
		LOGWARNING("Failed to parse enonce1 in parse_subscribe");
		goto out;
	}
	string = json_string_value(tmp);
	if (strlen(string) < 1) {
		LOGWARNING("Invalid string length for enonce1 in parse_subscribe");
		goto out;
	}
	old = proxi->enonce1;
	proxi->enonce1 = strdup(string);
	free(old);
	proxi->nonce1len = strlen(proxi->enonce1) / 2;
	if (proxi->nonce1len > 15) {
		LOGWARNING("Nonce1 too long at %d", proxi->nonce1len);
		goto out;
	}
	old = proxi->enonce1bin;
	proxi->enonce1bin = ckalloc(proxi->nonce1len);
	free(old);
	hex2bin(proxi->enonce1bin, proxi->enonce1, proxi->nonce1len);
	tmp = json_array_get(res_val, 2);
	if (!tmp || !json_is_integer(tmp)) {
		LOGWARNING("Failed to parse nonce2len in parse_subscribe");
		goto out;
	}
	size = json_integer_value(tmp);
	if (size < 1 || size > 8) {
		LOGWARNING("Invalid nonce2len %d in parse_subscribe", size);
		goto out;
	}
	if (size < 4) {
		LOGWARNING("Nonce2 length %d too small to be able to proxy", size);
		goto out;
	}
	proxi->nonce2len = size;

	LOGINFO("Found notify with enonce %s nonce2len %d", proxi->enonce1,
		proxi->nonce2len);
	ret = true;

out:
	if (val)
		json_decref(val);
	return ret;
}

static bool subscribe_stratum(connsock_t *cs, proxy_instance_t *proxi)
{
	json_t *req;
	bool ret = false;

retry:
	/* Attempt to reconnect if the pool supports resuming */
	if (proxi->sessionid) {
		req = json_pack("{s:i,s:s,s:[s,s]}",
				"id", proxi->id++,
				"method", "mining.subscribe",
				"params", PACKAGE"/"VERSION, proxi->sessionid);
	/* Then attempt to connect with just the client description */
	} else if (!proxi->no_params) {
		req = json_pack("{s:i,s:s,s:[s]}",
				"id", proxi->id++,
				"method", "mining.subscribe",
				"params", PACKAGE"/"VERSION);
	/* Then try without any parameters */
	} else {
		req = json_pack("{s:i,s:s,s:[]}",
				"id", proxi->id++,
				"method", "mining.subscribe",
				"params");
	}
	ret = send_json_msg(cs, req);
	json_decref(req);
	if (!ret) {
		LOGWARNING("Failed to send message in subscribe_stratum");
		close(cs->fd);
		goto out;
	}
	ret = parse_subscribe(cs, proxi);
	if (ret)
		goto out;

	close(cs->fd);
	if (proxi->no_params) {
		LOGWARNING("Failed all subscription options in subscribe_stratum");
		goto out;
	}
	if (proxi->sessionid) {
		LOGNOTICE("Failed sessionid reconnect in subscribe_stratum, retrying without");
		proxi->no_sessionid = true;
		dealloc(proxi->sessionid);
	} else {
		LOGNOTICE("Failed connecting with parameters in subscribe_stratum, retrying without");
		proxi->no_params = true;
	}
	ret = connect_proxy(cs);
	if (!ret) {
		LOGWARNING("Failed to reconnect in subscribe_stratum");
		goto out;
	}
	goto retry;

out:
	return ret;
}

static bool parse_notify(proxy_instance_t *proxi, json_t *val)
{
	const char *prev_hash, *bbversion, *nbit, *ntime;
	char *job_id, *coinbase1, *coinbase2;
	bool clean, ret = false;
	notify_instance_t *ni;
	int merkles, i;
	json_t *arr;

	arr = json_array_get(val, 4);
	if (!arr || !json_is_array(arr))
		goto out;

	merkles = json_array_size(arr);
	job_id = json_array_string(val, 0);
	prev_hash = __json_array_string(val, 1);
	coinbase1 = json_array_string(val, 2);
	coinbase2 = json_array_string(val, 3);
	bbversion = __json_array_string(val, 5);
	nbit = __json_array_string(val, 6);
	ntime = __json_array_string(val, 7);
	clean = json_is_true(json_array_get(val, 8));
	if (!job_id || !prev_hash || !coinbase1 || !coinbase2 || !bbversion || !nbit || !ntime) {
		if (job_id)
			free(job_id);
		if (coinbase1)
			free(coinbase1);
		if (coinbase2)
			free(coinbase2);
		goto out;
	}

	LOGDEBUG("New notify");
	ni = ckzalloc(sizeof(notify_instance_t));
	ni->jobid = job_id;
	LOGDEBUG("Job ID %s", job_id);
	ni->coinbase1 = coinbase1;
	LOGDEBUG("Coinbase1 %s", coinbase1);
	ni->coinbase2 = coinbase2;
	LOGDEBUG("Coinbase2 %s", coinbase2);
	memcpy(ni->prevhash, prev_hash, 65);
	LOGDEBUG("Prevhash %s", prev_hash);
	memcpy(ni->bbversion, bbversion, 9);
	LOGDEBUG("BBVersion %s", bbversion);
	memcpy(ni->nbit, nbit, 9);
	LOGDEBUG("Nbit %s", nbit);
	memcpy(ni->ntime, ntime, 9);
	LOGDEBUG("Ntime %s", ntime);
	ni->clean = clean;
	LOGDEBUG("Clean %s", clean ? "true" : "false");
	LOGDEBUG("Merkles %d", merkles);
	for (i = 0; i < merkles; i++) {
		const char *merkle = __json_array_string(arr, i);

		LOGDEBUG("Merkle %d %s", i, merkle);
		memcpy(&ni->merklehash[i][0], merkle, 65);
	}
	ni->merkles = merkles;
	ret = true;
	ni->notify_time = time(NULL);

	mutex_lock(&proxi->notify_lock);
	ni->id = proxi->notify_id++;
	HASH_ADD_INT(proxi->notify_instances, id, ni);
	proxi->current_notify = ni;
	mutex_unlock(&proxi->notify_lock);

out:
	return ret;
}

static bool parse_diff(proxy_instance_t *proxi, json_t *val)
{
	double diff = json_number_value(json_array_get(val, 0));

	if (diff == 0 || diff == proxi->diff)
		return true;
	proxi->diff = diff;
	proxi->diffed = true;
	return true;
}

static bool send_version(proxy_instance_t *proxi, json_t *val)
{
	json_t *json_msg, *id_val = json_object_dup(val, "id");
	connsock_t *cs = proxi->cs;
	bool ret;

	json_msg = json_pack("{sossso}", "id", id_val, "result", PACKAGE"/"VERSION,
			     "error", json_null());
	ret = send_json_msg(cs, json_msg);
	json_decref(json_msg);
	return ret;
}

static bool show_message(json_t *val)
{
	const char *msg;

	if (!json_is_array(val))
		return false;
	msg = json_string_value(json_array_get(val, 0));
	if (!msg)
		return false;
	LOGNOTICE("Pool message: %s", msg);
	return true;
}

static bool parse_reconnect(proxy_instance_t *proxi, json_t *val)
{
	server_instance_t *newsi, *si = proxi->si;
	ckpool_t *ckp = proxi->ckp;
	const char *new_url;
	bool ret = false;
	int new_port;
	char *url;

	new_url = json_string_value(json_array_get(val, 0));
	new_port = json_integer_value(json_array_get(val, 1));
	if (new_url && strlen(new_url) && new_port) {
		char *dot_pool, *dot_reconnect;
		int len;

		dot_pool = strchr(si->url, '.');
		if (!dot_pool) {
			LOGWARNING("Denied stratum reconnect request from server without domain %s",
				   si->url);
			goto out;
		}
		dot_reconnect = strchr(new_url, '.');
		if (!dot_reconnect) {
			LOGWARNING("Denied stratum reconnect request to url without domain %s",
				   new_url);
			goto out;
		}
		len = strlen(dot_reconnect);
		if (strncmp(dot_pool, dot_reconnect, len)) {
			LOGWARNING("Denied stratum reconnect request from %s to non-matching domain %s",
				   si->url, new_url);
			goto out;
		}
		ASPRINTF(&url, "%s:%d", new_url, new_port);
	} else
		url = strdup(si->url);
	LOGINFO("Processing reconnect request to %s", url);

	ret = true;
	newsi = ckzalloc(sizeof(server_instance_t));
	newsi->id = ckp->proxies++;
	ckp->servers = realloc(ckp->servers, sizeof(server_instance_t *) * ckp->proxies);
	ckp->servers[newsi->id] = newsi;
	ckp->chosen_server = newsi->id;
	newsi->url = url;
	newsi->auth = strdup(si->auth);
	newsi->pass = strdup(si->pass);
	proxi->reconnect = true;

	proxi = ckzalloc(sizeof(proxy_instance_t));
	newsi->data = proxi;
	proxi->auth = newsi->auth;
	proxi->pass = newsi->pass;
	proxi->si = newsi;
	proxi->ckp = ckp;
	proxi->cs = &newsi->cs;
out:
	return ret;
}

static bool parse_method(proxy_instance_t *proxi, const char *msg)
{
	json_t *val = NULL, *method, *err_val, *params;
	json_error_t err;
	bool ret = false;
	const char *buf;

	memset(&err, 0, sizeof(err));
	val = json_loads(msg, 0, &err);
	if (!val) {
		LOGWARNING("JSON decode failed(%d): %s", err.line, err.text);
		goto out;
	}

	method = json_object_get(val, "method");
	if (!method) {
		LOGDEBUG("Failed to find method in json for parse_method");
		goto out;
	}
	err_val = json_object_get(val, "error");
	params = json_object_get(val, "params");

	if (err_val && !json_is_null(err_val)) {
		char *ss;

		if (err_val)
			ss = json_dumps(err_val, 0);
		else
			ss = strdup("(unknown reason)");

		LOGINFO("JSON-RPC method decode failed: %s", ss);
		free(ss);
		goto out;
	}

	if (!json_is_string(method)) {
		LOGINFO("Method is not string in parse_method");
		goto out;
	}
	buf = json_string_value(method);
	if (!buf || strlen(buf) < 1) {
		LOGINFO("Invalid string for method in parse_method");
		goto out;
	}

	if (cmdmatch(buf, "mining.notify")) {
		if (parse_notify(proxi, params))
			proxi->notified = ret = true;
		else
			proxi->notified = ret = false;
		goto out;
	}

	if (cmdmatch(buf, "mining.set_difficulty")) {
		ret = parse_diff(proxi, params);
		goto out;
	}

	if (cmdmatch(buf, "client.reconnect")) {
		ret = parse_reconnect(proxi, params);
		goto out;
	}

	if (cmdmatch(buf, "client.get_version")) {
		ret =  send_version(proxi, val);
		goto out;
	}

	if (cmdmatch(buf, "client.show_message")) {
		ret = show_message(params);
		goto out;
	}
out:
	if (val)
		json_decref(val);
	return ret;
}

static bool auth_stratum(connsock_t *cs, proxy_instance_t *proxi)
{
	json_t *val = NULL, *res_val, *req;
	bool ret;

	req = json_pack("{s:i,s:s,s:[s,s]}",
			"id", proxi->id++,
			"method", "mining.authorize",
			"params", proxi->auth, proxi->pass);
	ret = send_json_msg(cs, req);
	json_decref(req);
	if (!ret) {
		LOGWARNING("Failed to send message in auth_stratum");
		close(cs->fd);
		goto out;
	}

	/* Read and parse any extra methods sent. Anything left in the buffer
	 * should be the response to our auth request. */
	do {
		int size;

		size = read_socket_line(cs, 5);
		if (size < 1) {
			LOGWARNING("Failed to receive line in auth_stratum");
			ret = false;
			goto out;
		}
		ret = parse_method(proxi, cs->buf);
	} while (ret);

	val = json_msg_result(cs->buf, &res_val);
	if (!val) {
		LOGWARNING("Failed to get a json result in auth_stratum, got: %s", cs->buf);
		goto out;
	}

	ret = json_is_true(res_val);
	if (!ret) {
		LOGWARNING("Failed to authorise in auth_stratum");
		goto out;
	}
	LOGINFO("Auth success in auth_stratum");
out:
	if (val)
		json_decref(val);
	return ret;
}

static void send_subscribe(proxy_instance_t *proxi, int sockd)
{
	json_t *json_msg;
	char *msg;

	json_msg = json_pack("{sssi}", "enonce1", proxi->enonce1,
			     "nonce2len", proxi->nonce2len);
	msg = json_dumps(json_msg, 0);
	json_decref(json_msg);
	send_unix_msg(sockd, msg);
	free(msg);
	close(sockd);
}

static void send_notify(proxy_instance_t *proxi, int sockd)
{
	json_t *json_msg, *merkle_arr;
	notify_instance_t *ni;
	char *msg;
	int i;

	merkle_arr = json_array();

	mutex_lock(&proxi->notify_lock);
	ni = proxi->current_notify;
	for (i = 0; i < ni->merkles; i++)
		json_array_append_new(merkle_arr, json_string(&ni->merklehash[i][0]));
	/* Use our own jobid instead of the server's one for easy lookup */
	json_msg = json_pack("{sisssssssosssssssb}",
			     "jobid", ni->id, "prevhash", ni->prevhash,
			     "coinbase1", ni->coinbase1, "coinbase2", ni->coinbase2,
			     "merklehash", merkle_arr, "bbversion", ni->bbversion,
			     "nbit", ni->nbit, "ntime", ni->ntime,
			     "clean", ni->clean);
	mutex_unlock(&proxi->notify_lock);

	msg = json_dumps(json_msg, 0);
	json_decref(json_msg);
	send_unix_msg(sockd, msg);
	free(msg);
	close(sockd);
}

static void send_diff(proxy_instance_t *proxi, int sockd)
{
	json_t *json_msg;
	char *msg;

	json_msg = json_pack("{sf}", "diff", proxi->diff);
	msg = json_dumps(json_msg, 0);
	json_decref(json_msg);
	send_unix_msg(sockd, msg);
	free(msg);
	close(sockd);
}

static void submit_share(proxy_instance_t *proxi, json_t *val)
{
	stratum_msg_t *msg;
	share_msg_t *share;

	msg = ckzalloc(sizeof(stratum_msg_t));
	share = ckzalloc(sizeof(share_msg_t));
	share->submit_time = time(NULL);
	share->client_id = json_integer_value(json_object_get(val, "client_id"));
	share->msg_id = json_integer_value(json_object_get(val, "msg_id"));
	json_object_del(val, "client_id");
	json_object_del(val, "msg_id");
	msg->json_msg = val;

	/* Add new share entry to the share hashtable */
	mutex_lock(&proxi->share_lock);
	share->id = proxi->share_id++;
	HASH_ADD_INT(proxi->shares, id, share);
	mutex_unlock(&proxi->share_lock);

	json_object_set_nocheck(val, "id", json_integer(share->id));

	/* Add the new message to the psend list */
	mutex_lock(&proxi->psend_lock);
	DL_APPEND(proxi->psends, msg);
	pthread_cond_signal(&proxi->psend_cond);
	mutex_unlock(&proxi->psend_lock);
}

static void clear_notify(notify_instance_t *ni)
{
	free(ni->jobid);
	free(ni->coinbase1);
	free(ni->coinbase2);
	free(ni);
}

/* FIXME: Return something useful to the stratifier based on this result */
static bool parse_share(proxy_instance_t *proxi, const char *buf)
{
	json_t *val = NULL, *idval;
	share_msg_t *share;
	bool ret = false;
	int id;

	val = json_loads(buf, 0, NULL);
	if (!val) {
		LOGINFO("Failed to parse json msg: %s", buf);
		goto out;
	}
	idval = json_object_get(val, "id");
	if (!idval) {
		LOGINFO("Failed to find id in json msg: %s", buf);
		goto out;
	}
	id = json_integer_value(idval);

	mutex_lock(&proxi->share_lock);
	HASH_FIND_INT(proxi->shares, &id, share);
	if (share)
		HASH_DEL(proxi->shares, share);
	mutex_unlock(&proxi->share_lock);

	if (!share) {
		LOGINFO("Failed to find matching share to result: %s", buf);
		goto out;
	}
	ret = true;
	LOGDEBUG("Found share from client %d with msg_id %d", share->client_id,
		 share->msg_id);
	free(share);
out:
	if (val)
		json_decref(val);
	return ret;
}

static void *proxy_recv(void *arg)
{
	proxy_instance_t *proxi = (proxy_instance_t *)arg;
	connsock_t *cs = proxi->cs;
	ckpool_t *ckp = proxi->ckp;

	rename_proc("proxyrecv");

	while (42) {
		notify_instance_t *ni, *tmp;
		share_msg_t *share, *tmpshare;
		int retries = 0, ret;
		time_t now;

		now = time(NULL);

		/* Age old notifications older than 10 mins old */
		mutex_lock(&proxi->notify_lock);
		HASH_ITER(hh, proxi->notify_instances, ni, tmp) {
			if (HASH_COUNT(proxi->notify_instances) < 3)
				break;
			if (ni->notify_time < now - 600) {
				HASH_DEL(proxi->notify_instances, ni);
				clear_notify(ni);
			}
		}
		mutex_unlock(&proxi->notify_lock);

		/* Similary with shares older than 2 mins without response */
		mutex_lock(&proxi->share_lock);
		HASH_ITER(hh, proxi->shares, share, tmpshare) {
			if (share->submit_time < now - 120) {
				HASH_DEL(proxi->shares, share);
			}
		}
		mutex_unlock(&proxi->share_lock);

		/* If we don't get an update within 2 minutes the upstream pool
		 * has likely stopped responding. */
		do {
			if (cs->fd == -1) {
				ret = -1;
				break;
			}
			ret = read_socket_line(cs, 5);
		} while (ret == 0 && ++retries < 24);

		if (ret < 1) {
			/* Send ourselves a reconnect message */
			LOGWARNING("Failed to read_socket_line in proxy_recv, attempting reconnect");
			send_proc(ckp->generator, "reconnect");
			break;
		}
		if (parse_method(proxi, cs->buf)) {
			if (proxi->notified) {
				send_proc(ckp->stratifier, "notify");
				proxi->notified = false;
			}
			if (proxi->diffed) {
				send_proc(ckp->stratifier, "diff");
				proxi->diffed = false;
			}
			if (proxi->reconnect) {
				proxi->reconnect = false;
				LOGWARNING("Reconnect issue, dropping existing connection");
				send_proc(ckp->generator, "reconnect");
				break;
			}
			continue;
		}
		if (parse_share(proxi, cs->buf)) {
			continue;
		}
		/* If it's not a method it should be a share result */
		LOGWARNING("Unhandled stratum message: %s", cs->buf);
	}
	return NULL;
}

/* For processing and sending shares */
static void *proxy_send(void *arg)
{
	proxy_instance_t *proxi = (proxy_instance_t *)arg;
	connsock_t *cs = proxi->cs;

	rename_proc("proxysend");

	while (42) {
		notify_instance_t *ni;
		stratum_msg_t *msg;
		char *jobid = NULL;
		bool ret = true;
		json_t *val;
		uint32_t id;

		mutex_lock(&proxi->psend_lock);
		if (!proxi->psends)
			pthread_cond_wait(&proxi->psend_cond, &proxi->psend_lock);
		msg = proxi->psends;
		if (likely(msg))
			DL_DELETE(proxi->psends, msg);
		mutex_unlock(&proxi->psend_lock);

		if (unlikely(!msg))
			continue;

		json_uintcpy(&id, msg->json_msg, "jobid");

		mutex_lock(&proxi->notify_lock);
		HASH_FIND_INT(proxi->notify_instances, &id, ni);
		if (ni)
			jobid = strdup(ni->jobid);
		mutex_unlock(&proxi->notify_lock);

		if (jobid) {
			val = json_pack("{s[ssooo]soss}", "params", proxi->auth, jobid,
					json_object_dup(msg->json_msg, "nonce2"),
					json_object_dup(msg->json_msg, "ntime"),
					json_object_dup(msg->json_msg, "nonce"),
					"id", json_object_dup(msg->json_msg, "id"),
					"method", "mining.submit");
			free(jobid);
			ret = send_json_msg(cs, val);
			json_decref(val);
		} else
			LOGWARNING("Failed to find matching jobid in proxysend");
		json_decref(msg->json_msg);
		free(msg);
		if (!ret && cs->fd > 0) {
			LOGWARNING("Failed to send msg in proxy_send, dropping to reconnect");
			close(cs->fd);
			cs->fd = -1;
		}
	}
	return NULL;
}

/* Cycle through the available proxies and find the first alive one */
static proxy_instance_t *live_proxy(ckpool_t *ckp)
{
	proxy_instance_t *alive = NULL;
	connsock_t *cs;
	int i;

	LOGDEBUG("Attempting to connect to proxy");
retry:
	if (!ping_main(ckp))
		goto out;

	for (i = ckp->chosen_server; i < ckp->proxies; i++) {
		proxy_instance_t *proxi;
		server_instance_t *si;

		si = ckp->servers[i];
		proxi = si->data;
		cs = proxi->cs;
		if (!extract_sockaddr(si->url, &cs->url, &cs->port)) {
			LOGWARNING("Failed to extract address from %s", si->url);
			continue;
		}
		if (!connect_proxy(cs)) {
			LOGINFO("Failed to connect to %s:%s in proxy_mode!",
				cs->url, cs->port);
			continue;
		}
		/* Test we can connect, authorise and get stratum information */
		if (!subscribe_stratum(cs, proxi)) {
			LOGINFO("Failed initial subscribe to %s:%s !",
				cs->url, cs->port);
			continue;
		}
		if (!auth_stratum(cs, proxi)) {
			LOGWARNING("Failed initial authorise to %s:%s with %s:%s !",
				   cs->url, cs->port, si->auth, si->pass);
			continue;
		}
		alive = proxi;
		break;
	}
	if (!alive) {
		if (!ckp->chosen_server) {
			LOGWARNING("Failed to connect to any servers as proxy, retrying in 5s!");
			sleep(5);
		}
		goto retry;
	}
	ckp->chosen_server = 0;
	cs = alive->cs;
	LOGNOTICE("Connected to upstream server %s:%s as proxy", cs->url, cs->port);
	mutex_init(&alive->notify_lock);
	create_pthread(&alive->pth_precv, proxy_recv, alive);
	mutex_init(&alive->psend_lock);
	cond_init(&alive->psend_cond);
	create_pthread(&alive->pth_psend, proxy_send, alive);
out:
	send_proc(ckp->connector, alive ? "accept" : "reject");
	return alive;
}

static void kill_proxy(proxy_instance_t *proxi)
{
	notify_instance_t *ni, *tmp;
	connsock_t *cs;

	if (!proxi)
		return;
	cs = proxi->cs;
	close(cs->fd);

	/* All our notify data is invalid if we reconnect so discard them */
	mutex_lock(&proxi->notify_lock);
	HASH_ITER(hh, proxi->notify_instances, ni, tmp) {
		HASH_DEL(proxi->notify_instances, ni);
		clear_notify(ni);
	}
	mutex_unlock(&proxi->notify_lock);

	pthread_cancel(proxi->pth_precv);
	pthread_cancel(proxi->pth_psend);
}

static int proxy_loop(proc_instance_t *pi)
{
	int sockd = -1, ret = 0, selret;
	unixsock_t *us = &pi->us;
	ckpool_t *ckp = pi->ckp;
	proxy_instance_t *proxi;
	char *buf = NULL;

reconnect:
	proxi = live_proxy(ckp);
	if (!proxi)
		goto out;

	/* We've just subscribed and authorised so tell the stratifier to
	 * retrieve the first subscription. */
	send_proc(ckp->stratifier, "subscribe");
	send_proc(ckp->stratifier, "notify");
	proxi->notified = false;

	do {
		selret = wait_read_select(us->sockd, 5);
		if (!selret && !ping_main(ckp)) {
			LOGEMERG("Generator failed to ping main process, exiting");
			ret = 1;
			goto out;
		}
	} while (selret < 1);
retry:
	sockd = accept(us->sockd, NULL, NULL);
	if (sockd < 0) {
		LOGEMERG("Failed to accept on proxy socket");
		ret = 1;
		goto out;
	}
	dealloc(buf);
	buf = recv_unix_msg(sockd);
	if (!buf) {
		LOGWARNING("Failed to get message in proxy_loop");
		close(sockd);
		goto retry;
	}
	LOGDEBUG("Proxy received request: %s", buf);
	if (cmdmatch(buf, "shutdown")) {
		ret = 0;
		goto out;
	} else if (cmdmatch(buf, "getsubscribe")) {
		send_subscribe(proxi, sockd);
	} else if (cmdmatch(buf, "getnotify")) {
		send_notify(proxi, sockd);
	} else if (cmdmatch(buf, "getdiff")) {
		send_diff(proxi, sockd);
	} else if (cmdmatch(buf, "reconnect")) {
		kill_proxy(proxi);
		pthread_cancel(proxi->pth_precv);
		pthread_cancel(proxi->pth_psend);
		goto reconnect;
	} else if (cmdmatch(buf, "loglevel")) {
		sscanf(buf, "loglevel=%d", &ckp->loglevel);
	} else if (cmdmatch(buf, "ping")) {
		LOGDEBUG("Proxy received ping request");
		send_unix_msg(sockd, "pong");
	} else {
		/* Anything remaining should be share submissions */
		json_t *val = json_loads(buf, 0, NULL);

		if (!val)
			LOGWARNING("Received unrecognised message: %s", buf);
		else
			submit_share(proxi, val);
	}
	close(sockd);
	goto retry;
out:
	kill_proxy(proxi);
	close(sockd);
	return ret;
}

static int server_mode(ckpool_t *ckp, proc_instance_t *pi)
{
	server_instance_t *si;
	int i, ret;

	ckp->servers = ckalloc(sizeof(server_instance_t *) * ckp->btcds);
	for (i = 0; i < ckp->btcds; i++) {
		gbtbase_t *gbt;

		ckp->servers[i] = ckzalloc(sizeof(server_instance_t));
		si = ckp->servers[i];
		si->url = ckp->btcdurl[i];
		si->auth = ckp->btcdauth[i];
		si->pass = ckp->btcdpass[i];
		gbt = ckzalloc(sizeof(gbtbase_t));
		si->data = gbt;
	}

	ret = gen_loop(pi);

	for (i = 0; i < ckp->btcds; si = ckp->servers[i], i++) {
		connsock_t *cs = &si->cs;

		dealloc(cs->url);
		dealloc(cs->port);
		dealloc(cs->auth);
		dealloc(si->data);
		dealloc(si);
	}
	dealloc(ckp->servers);
	return ret;
}

static int proxy_mode(ckpool_t *ckp, proc_instance_t *pi)
{
	proxy_instance_t *proxi;
	server_instance_t *si;
	int i, ret;

	/* Create all our proxy structures and pointers */
	ckp->servers = ckalloc(sizeof(server_instance_t *) * ckp->proxies);
	for (i = 0; i < ckp->proxies; i++) {
		ckp->servers[i] = ckzalloc(sizeof(server_instance_t));
		si = ckp->servers[i];
		si->id = i;
		si->url = strdup(ckp->proxyurl[i]);
		si->auth = strdup(ckp->proxyauth[i]);
		si->pass = strdup(ckp->proxypass[i]);
		proxi = ckzalloc(sizeof(proxy_instance_t));
		si->data = proxi;
		proxi->auth = si->auth;
		proxi->pass = si->pass;
		proxi->si = si;
		proxi->ckp = ckp;
		proxi->cs = &si->cs;
	}

	ret = proxy_loop(pi);

	for (i = 0; i < ckp->proxies; i++) {
		si = ckp->servers[i];
		close(si->cs.fd);
		proxi = si->data;
		free(proxi->enonce1);
		free(proxi->enonce1bin);
		free(proxi->sessionid);
		pthread_cancel(proxi->pth_psend);
		pthread_cancel(proxi->pth_precv);
		join_pthread(proxi->pth_psend);
		join_pthread(proxi->pth_precv);
		dealloc(si->data);
		dealloc(si->url);
		dealloc(si->auth);
		dealloc(si->pass);
		dealloc(si);
	}
	dealloc(ckp->servers);
	return ret;
}

int generator(proc_instance_t *pi)
{
	ckpool_t *ckp = pi->ckp;
	int ret;

	if (ckp->proxy)
		ret = proxy_mode(ckp, pi);
	else
		ret = server_mode(ckp, pi);

	return process_exit(ckp, pi, ret);
}
