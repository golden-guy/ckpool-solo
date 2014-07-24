/*
 * Copyright 2014 Con Kolivas
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 */

#include "config.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <string.h>
#include <unistd.h>

#include "ckpool.h"
#include "libckpool.h"
#include "bitcoin.h"
#include "sha2.h"
#include "stratifier.h"
#include "uthash.h"
#include "utlist.h"

static const char *workpadding = "000000800000000000000000000000000000000000000000000000000000000000000000000000000000000080020000";

static const char *scriptsig_header = "01000000010000000000000000000000000000000000000000000000000000000000000000ffffffff";
static uchar scriptsig_header_bin[41];

static char pubkeytxnbin[25];
static char pubkeytxn[52];

/* Add unaccounted shares when they arrive, remove them with each update of
 * rolling stats. */
struct pool_stats {
	tv_t start_time;
	ts_t last_update;

	int workers;
	int users;

	/* Absolute shares stats */
	int64_t unaccounted_shares;
	int64_t accounted_shares;

	/* Cycle of 24 to determine which users to dump stats on */
	uint8_t userstats_cycle;

	/* Shares per second for 1/5/15/60 minute rolling averages */
	double sps1;
	double sps5;
	double sps15;
	double sps60;

	/* Diff shares stats */
	int64_t unaccounted_diff_shares;
	int64_t accounted_diff_shares;
	int64_t unaccounted_rejects;
	int64_t accounted_rejects;
	int64_t pprop_shares;
	int64_t round_shares;

	/* Diff shares per second for 1/5/15... minute rolling averages */
	double dsps1;
	double dsps5;
	double dsps15;
	double dsps60;
	double dsps360;
	double dsps1440;
};

typedef struct pool_stats pool_stats_t;

static pool_stats_t stats;

static pthread_mutex_t stats_lock;

static uint64_t enonce1_64;

struct workbase {
	/* Hash table data */
	UT_hash_handle hh;
	int64_t id;
	char idstring[20];

	ts_t gentime;

	/* GBT/shared variables */
	char target[68];
	double diff;
	double network_diff;
	uint32_t version;
	uint32_t curtime;
	char prevhash[68];
	char ntime[12];
	uint32_t ntime32;
	char bbversion[12];
	char nbit[12];
	uint64_t coinbasevalue;
	int height;
	char *flags;
	int transactions;
	char *txn_data;
	char *txn_hashes;
	int merkles;
	char merklehash[16][68];
	char merklebin[16][32];
	json_t *merkle_array;

	/* Template variables, lengths are binary lengths! */
	char coinb1[256]; // coinbase1
	uchar coinb1bin[128];
	int coinb1len; // length of above

	char enonce1const[32]; // extranonce1 section that is constant
	uchar enonce1constbin[16];
	int enonce1constlen; // length of above - usually zero unless proxying
	int enonce1varlen; // length of unique extranonce1 string for each worker - usually 8

	int enonce2varlen; // length of space left for extranonce2 - usually 8 unless proxying

	char *coinb2; // coinbase2
	uchar *coinb2bin;
	int coinb2len; // length of above

	/* Cached header binary */
	char headerbin[112];

	char *logdir;

	ckpool_t *ckp;
	bool proxy;
};

typedef struct workbase workbase_t;

/* For protecting the hashtable data */
static cklock_t workbase_lock;

/* For the hashtable of all workbases */
static workbase_t *workbases;
static workbase_t *current_workbase;

static struct {
	double diff;

	char enonce1[32];
	uchar enonce1bin[16];
	int enonce1constlen;
	int enonce1varlen;

	int nonce2len;
	int enonce2varlen;
} proxy_base;

static int64_t workbase_id;
static int64_t blockchange_id;
static char lasthash[68];

struct json_params {
	json_t *params;
	json_t *id_val;
	int client_id;
	char address[INET6_ADDRSTRLEN];
};

typedef struct json_params json_params_t;

struct ckdb_msg {
	json_t *val;
	int idtype;
};

typedef struct ckdb_msg ckdb_msg_t;

/* Stratum json messages with their associated client id */
struct smsg {
	json_t *json_msg;
	int client_id;
	char address[INET6_ADDRSTRLEN];
};

typedef struct smsg smsg_t;

static ckmsgq_t *ssends;	// Stratum sends
static ckmsgq_t *srecvs;	// Stratum receives
static ckmsgq_t *ckdbq;		// ckdb
static ckmsgq_t *sshareq;	// Stratum share sends
static ckmsgq_t *sauthq;	// Stratum authorisations

static int user_instance_id;

struct user_instance {
	UT_hash_handle hh;
	char username[128];
	int id;

	int64_t diff_accepted;
	int64_t diff_rejected;
	uint64_t pprop_shares;

	int workers;
};

typedef struct user_instance user_instance_t;

static user_instance_t *user_instances;

/* Per client stratum instance == workers */
struct stratum_instance {
	UT_hash_handle hh;
	int id;

	char enonce1[32];
	uchar enonce1bin[16];
	char enonce1var[12];
	uint64_t enonce1_64;

	int64_t diff; /* Current diff */
	int64_t old_diff; /* Previous diff */
	int64_t diff_change_job_id; /* Last job_id we changed diff */
	double dsps1; /* Diff shares per second, 1 minute rolling average */
	double dsps5; /* ... 5 minute ... */
	double dsps60;/* etc */
	double dsps1440;
	tv_t ldc; /* Last diff change */
	int ssdc; /* Shares since diff change */
	tv_t first_share;
	tv_t last_share;
	time_t start_time;

	int64_t absolute_shares;
	int64_t diff_shares;

	char address[INET6_ADDRSTRLEN];
	bool authorised;
	bool idle;
	bool notified_idle;

	user_instance_t *user_instance;
	char *useragent;
	char *workername;
	char *secondaryuserid;
	int user_id;

	ckpool_t *ckp;
};

typedef struct stratum_instance stratum_instance_t;

/* Stratum_instances hashlist is stored by id, whereas disconnected_instances
 * is sorted by enonce1_64. */
static stratum_instance_t *stratum_instances;
static stratum_instance_t *disconnected_instances;
static stratum_instance_t *dead_instances;

/* Protects both stratum and user instances */
static cklock_t instance_lock;

struct share {
	UT_hash_handle hh;
	uchar hash[32];
	int64_t workbase_id;
};

typedef struct share share_t;

static share_t *shares;

static cklock_t share_lock;

#define ID_AUTH 0
#define ID_WORKINFO 1
#define ID_AGEWORKINFO 2
#define ID_SHARES 3
#define ID_SHAREERR 4
#define ID_POOLSTATS 5
#define ID_USERSTATS 6
#define ID_BLOCK 7

static const char *ckdb_ids[] = {
	"authorise",
	"workinfo",
	"ageworkinfo",
	"shares",
	"shareerror",
	"poolstats",
	"userstats",
	"block"
};

static void generate_coinbase(ckpool_t *ckp, workbase_t *wb)
{
	char header[228];
	int len, ofs = 0;
	uint64_t *u64;
	ts_t now;

	/* Strings in wb should have been zero memset prior. Generate binary
	 * templates first, then convert to hex */
	memcpy(wb->coinb1bin, scriptsig_header_bin, 41);
	ofs += 41; // Fixed header length;

	ofs++; // Script length is filled in at the end @wb->coinb1bin[41];

	/* Put block height at start of template */
	len = ser_number(wb->coinb1bin + ofs, wb->height);
	ofs += len;

	/* Followed by flag */
	len = strlen(wb->flags) / 2;
	wb->coinb1bin[ofs++] = len;
	hex2bin(wb->coinb1bin + ofs, wb->flags, len);
	ofs += len;

	/* Followed by timestamp */
	ts_realtime(&now);
	len = ser_number(wb->coinb1bin + ofs, now.tv_sec);
	ofs += len;

	/* Followed by our unique randomiser based on the nsec timestamp */
	len = ser_number(wb->coinb1bin + ofs, now.tv_nsec);
	ofs += len;

	/* Leave enonce1/2varlen constant at 8 bytes for bitcoind sources */
	wb->enonce1varlen = 8;
	wb->enonce2varlen = 8;
	wb->coinb1bin[ofs++] = wb->enonce1varlen + wb->enonce2varlen;

	wb->coinb1len = ofs;

	len = wb->coinb1len - 41;

	len += wb->enonce1varlen;
	len += wb->enonce2varlen;

	wb->coinb2bin = ckalloc(128);
	memcpy(wb->coinb2bin, "\x0a\x63\x6b\x70\x6f\x6f\x6c", 7);
	wb->coinb2len = 7;
	if (ckp->btcsig) {
		int siglen = strlen(ckp->btcsig);

		LOGDEBUG("Len %d sig %s", siglen, ckp->btcsig);
		if (siglen) {
			wb->coinb2bin[wb->coinb2len++] = siglen;
			memcpy(wb->coinb2bin + wb->coinb2len, ckp->btcsig, siglen);
			wb->coinb2len += siglen;
		}
	}
	len += wb->coinb2len;

	wb->coinb1bin[41] = len - 1; /* Set the length now */
	__bin2hex(wb->coinb1, wb->coinb1bin, wb->coinb1len);
	LOGDEBUG("Coinb1: %s", wb->coinb1);
	/* Coinbase 1 complete */

	memcpy(wb->coinb2bin + wb->coinb2len, "\xff\xff\xff\xff", 4);
	wb->coinb2len += 4;

	wb->coinb2bin[wb->coinb2len++] = 1;
	u64 = (uint64_t *)&wb->coinb2bin[wb->coinb2len];
	*u64 = htole64(wb->coinbasevalue);
	wb->coinb2len += 8;

	wb->coinb2bin[wb->coinb2len++] = 25;

	memcpy(wb->coinb2bin + wb->coinb2len, pubkeytxnbin, 25);
	wb->coinb2len += 25;

	wb->coinb2len += 4; // Blank lock

	wb->coinb2 = bin2hex(wb->coinb2bin, wb->coinb2len);
	LOGDEBUG("Coinb2: %s", wb->coinb2);
	/* Coinbase 2 complete */

	snprintf(header, 225, "%s%s%s%s%s%s%s",
		 wb->bbversion, wb->prevhash,
		 "0000000000000000000000000000000000000000000000000000000000000000",
		 wb->ntime, wb->nbit,
		 "00000000", /* nonce */
		 workpadding);
	LOGDEBUG("Header: %s", header);
	hex2bin(wb->headerbin, header, 112);
}

static void stratum_broadcast_update(bool clean);

static void clear_workbase(workbase_t *wb)
{
	free(wb->flags);
	free(wb->txn_data);
	free(wb->txn_hashes);
	free(wb->logdir);
	free(wb->coinb2bin);
	free(wb->coinb2);
	json_decref(wb->merkle_array);
	free(wb);
}

static void purge_share_hashtable(int64_t wb_id)
{
	share_t *share, *tmp;
	int purged = 0;

	ck_wlock(&share_lock);
	HASH_ITER(hh, shares, share, tmp) {
		if (share->workbase_id < wb_id) {
			HASH_DEL(shares, share);
			free(share);
			purged++;
		}
	}
	ck_wunlock(&share_lock);

	if (purged)
		LOGINFO("Cleared %d shares from share hashtable", purged);
}

static void ckdbq_add(ckpool_t *ckp, const int idtype, json_t *val)
{
	ckdb_msg_t *msg;

	if (ckp->standalone)
		return json_decref(val);

	msg = ckalloc(sizeof(ckdb_msg_t));
	msg->val = val;
	msg->idtype = idtype;
	ckmsgq_add(ckdbq, msg);
}

static void send_workinfo(ckpool_t *ckp, workbase_t *wb)
{
	char cdfield[64];
	json_t *val;

	sprintf(cdfield, "%lu,%lu", wb->gentime.tv_sec, wb->gentime.tv_nsec);

	val = json_pack("{sI,ss,ss,ss,ss,ss,ss,ss,ss,sI,so,ss,ss,ss,ss}",
			"workinfoid", wb->id,
			"poolinstance", ckp->name,
			"transactiontree", wb->txn_hashes,
			"prevhash", wb->prevhash,
			"coinbase1", wb->coinb1,
			"coinbase2", wb->coinb2,
			"version", wb->bbversion,
			"ntime", wb->ntime,
			"bits", wb->nbit,
			"reward", wb->coinbasevalue,
			"merklehash", json_deep_copy(wb->merkle_array),
			"createdate", cdfield,
			"createby", "code",
			"createcode", __func__,
			"createinet", "127.0.0.1");
	ckdbq_add(ckp, ID_WORKINFO, val);
}

static void send_ageworkinfo(ckpool_t *ckp, int64_t id)
{
	char cdfield[64];
	ts_t ts_now;
	json_t *val;

	ts_realtime(&ts_now);
	sprintf(cdfield, "%lu,%lu", ts_now.tv_sec, ts_now.tv_nsec);

	val = json_pack("{sI,ss,ss,ss,ss,ss}",
			"workinfoid", id,
			"poolinstance", ckp->name,
			"createdate", cdfield,
			"createby", "code",
			"createcode", __func__,
			"createinet", "127.0.0.1");
	ckdbq_add(ckp, ID_AGEWORKINFO, val);
}

static void add_base(ckpool_t *ckp, workbase_t *wb, bool *new_block)
{
	workbase_t *tmp, *tmpa, *aged = NULL;
	int len, ret;

	ts_realtime(&wb->gentime);
	wb->network_diff = diff_from_nbits(wb->headerbin + 72);

	len = strlen(ckp->logdir) + 8 + 1 + 16 + 1;
	wb->logdir = ckalloc(len);

	ck_wlock(&workbase_lock);
	if (!ckp->proxy)
		wb->id = workbase_id++;

	if (strncmp(wb->prevhash, lasthash, 64)) {
		*new_block = true;
		memcpy(lasthash, wb->prevhash, 65);
		blockchange_id = wb->id;
	}
	if (*new_block) {
		sprintf(wb->logdir, "%s%08x/", ckp->logdir, wb->height);
		ret = mkdir(wb->logdir, 0750);
		if (unlikely(ret && errno != EEXIST))
			LOGERR("Failed to create log directory %s", wb->logdir);
	}
	sprintf(wb->idstring, "%016lx", wb->id);
	sprintf(wb->logdir, "%s%08x/%s", ckp->logdir, wb->height, wb->idstring);

	HASH_ITER(hh, workbases, tmp, tmpa) {
		if (HASH_COUNT(workbases) < 3)
			break;
		/*  Age old workbases older than 10 minutes old */
		if (tmp->gentime.tv_sec < wb->gentime.tv_sec - 600) {
			HASH_DEL(workbases, tmp);
			aged = tmp;
			break;
		}
	}
	HASH_ADD_INT(workbases, id, wb);
	current_workbase = wb;
	ck_wunlock(&workbase_lock);

	if (*new_block)
		purge_share_hashtable(wb->id);

	send_workinfo(ckp, wb);

	/* Send the aged work message to ckdb once we have dropped the workbase lock
	 * to prevent taking recursive locks */
	if (aged) {
		send_ageworkinfo(ckp, aged->id);
		clear_workbase(aged);
	}
}

/* This function assumes it will only receive a valid json gbt base template
 * since checking should have been done earlier, and creates the base template
 * for generating work templates. */
static void update_base(ckpool_t *ckp)
{
	bool new_block = false;
	workbase_t *wb;
	json_t *val;
	char *buf;

	buf = send_recv_proc(ckp->generator, "getbase");
	if (unlikely(!buf)) {
		LOGWARNING("Failed to get base from generator in update_base");
		return;
	}
	if (unlikely(!strncasecmp(buf, "failed", 6))) {
		LOGWARNING("Generator returned failure in update_base");
		return;
	}

	wb = ckzalloc(sizeof(workbase_t));
	wb->ckp = ckp;
	val = json_loads(buf, 0, NULL);
	dealloc(buf);

	json_strcpy(wb->target, val, "target");
	json_dblcpy(&wb->diff, val, "diff");
	json_uintcpy(&wb->version, val, "version");
	json_uintcpy(&wb->curtime, val, "curtime");
	json_strcpy(wb->prevhash, val, "prevhash");
	json_strcpy(wb->ntime, val, "ntime");
	sscanf(wb->ntime, "%x", &wb->ntime32);
	json_strcpy(wb->bbversion, val, "bbversion");
	json_strcpy(wb->nbit, val, "nbit");
	json_uint64cpy(&wb->coinbasevalue, val, "coinbasevalue");
	json_intcpy(&wb->height, val, "height");
	json_strdup(&wb->flags, val, "flags");
	json_intcpy(&wb->transactions, val, "transactions");
	if (wb->transactions) {
		json_strdup(&wb->txn_data, val, "txn_data");
		json_strdup(&wb->txn_hashes, val, "txn_hashes");
	} else
		wb->txn_hashes = ckzalloc(1);
	json_intcpy(&wb->merkles, val, "merkles");
	wb->merkle_array = json_array();
	if (wb->merkles) {
		json_t *arr;
		int i;

		arr = json_object_get(val, "merklehash");
		for (i = 0; i < wb->merkles; i++) {
			strcpy(&wb->merklehash[i][0], json_string_value(json_array_get(arr, i)));
			hex2bin(&wb->merklebin[i][0], &wb->merklehash[i][0], 32);
			json_array_append_new(wb->merkle_array, json_string(&wb->merklehash[i][0]));
		}
	}
	json_decref(val);
	generate_coinbase(ckp, wb);

	add_base(ckp, wb, &new_block);

	stratum_broadcast_update(new_block);
}

static void drop_allclients(ckpool_t *ckp)
{
	stratum_instance_t *client, *tmp;
	char buf[128];

	ck_wlock(&instance_lock);
	HASH_ITER(hh, stratum_instances, client, tmp) {
		HASH_DEL(stratum_instances, client);
		sprintf(buf, "dropclient=%d", client->id);
		send_proc(ckp->connector, buf);
	}
	HASH_ITER(hh, disconnected_instances, client, tmp)
		HASH_DEL(disconnected_instances, client);
	stats.users = stats.workers = 0;
	ck_wunlock(&instance_lock);
}

static bool update_subscribe(ckpool_t *ckp)
{
	json_t *val;
	char *buf;

	buf = send_recv_proc(ckp->generator, "getsubscribe");
	if (unlikely(!buf)) {
		LOGWARNING("Failed to get subscribe from generator in update_notify");
		return false;
	}
	LOGDEBUG("Update subscribe: %s", buf);
	val = json_loads(buf, 0, NULL);
	free(buf);

	ck_wlock(&workbase_lock);
	if (!proxy_base.diff)
		proxy_base.diff = 1;
	/* Length is checked by generator */
	strcpy(proxy_base.enonce1, json_string_value(json_object_get(val, "enonce1")));
	proxy_base.enonce1constlen = strlen(proxy_base.enonce1) / 2;
	hex2bin(proxy_base.enonce1bin, proxy_base.enonce1, proxy_base.enonce1constlen);
	proxy_base.nonce2len = json_integer_value(json_object_get(val, "nonce2len"));
	if (proxy_base.nonce2len > 5)
		proxy_base.enonce1varlen = 4;
	else
		proxy_base.enonce1varlen = 2;
	proxy_base.enonce2varlen = proxy_base.nonce2len - proxy_base.enonce1varlen;
	ck_wunlock(&workbase_lock);

	json_decref(val);
	drop_allclients(ckp);

	return true;
}

static void update_notify(ckpool_t *ckp)
{
	bool new_block = false, clean;
	char header[228];
	workbase_t *wb;
	json_t *val;
	char *buf;
	int i;

	buf = send_recv_proc(ckp->generator, "getnotify");
	if (unlikely(!buf)) {
		LOGWARNING("Failed to get notify from generator in update_notify");
		return;
	}

	LOGDEBUG("Update notify: %s", buf);
	wb = ckzalloc(sizeof(workbase_t));
	val = json_loads(buf, 0, NULL);
	dealloc(buf);
	wb->ckp = ckp;
	wb->proxy = true;

	json_int64cpy(&wb->id, val, "jobid");
	json_strcpy(wb->prevhash, val, "prevhash");
	json_strcpy(wb->coinb1, val, "coinbase1");
	wb->coinb1len = strlen(wb->coinb1) / 2;
	hex2bin(wb->coinb1bin, wb->coinb1, wb->coinb1len);
	wb->height = get_sernumber(wb->coinb1bin + 42);
	json_strdup(&wb->coinb2, val, "coinbase2");
	wb->coinb2len = strlen(wb->coinb2) / 2;
	wb->coinb2bin = ckalloc(wb->coinb2len);
	hex2bin(wb->coinb2bin, wb->coinb2, wb->coinb2len);
	wb->merkle_array = json_object_dup(val, "merklehash");
	wb->merkles = json_array_size(wb->merkle_array);
	for (i = 0; i < wb->merkles; i++) {
		strcpy(&wb->merklehash[i][0], json_string_value(json_array_get(wb->merkle_array, i)));
		hex2bin(&wb->merklebin[i][0], &wb->merklehash[i][0], 32);
	}
	json_strcpy(wb->bbversion, val, "bbversion");
	json_strcpy(wb->nbit, val, "nbit");
	json_strcpy(wb->ntime, val, "ntime");
	sscanf(wb->ntime, "%x", &wb->ntime32);
	clean = json_is_true(json_object_get(val, "clean"));
	json_decref(val);
	ts_realtime(&wb->gentime);
	snprintf(header, 225, "%s%s%s%s%s%s%s",
		 wb->bbversion, wb->prevhash,
		 "0000000000000000000000000000000000000000000000000000000000000000",
		 wb->ntime, wb->nbit,
		 "00000000", /* nonce */
		 workpadding);
	LOGDEBUG("Header: %s", header);
	hex2bin(wb->headerbin, header, 112);

	ck_rlock(&workbase_lock);
	strcpy(wb->enonce1const, proxy_base.enonce1);
	wb->enonce1constlen = proxy_base.enonce1constlen;
	memcpy(wb->enonce1constbin, proxy_base.enonce1bin, wb->enonce1constlen);
	wb->enonce1varlen = proxy_base.enonce1varlen;
	wb->enonce2varlen = proxy_base.enonce2varlen;
	wb->diff = proxy_base.diff;
	ck_runlock(&workbase_lock);

	add_base(ckp, wb, &new_block);

	stratum_broadcast_update(new_block | clean);
}

static void stratum_send_diff(stratum_instance_t *client);

static void update_diff(ckpool_t *ckp)
{
	stratum_instance_t *client;
	double old_diff, diff;
	json_t *val;
	char *buf;

	buf = send_recv_proc(ckp->generator, "getdiff");
	if (unlikely(!buf)) {
		LOGWARNING("Failed to get diff from generator in update_diff");
		return;
	}

	LOGDEBUG("Update diff: %s", buf);
	val = json_loads(buf, 0, NULL);
	dealloc(buf);
	json_dblcpy(&diff, val, "diff");
	json_decref(val);

	ck_wlock(&workbase_lock);
	old_diff = proxy_base.diff;
	current_workbase->diff = proxy_base.diff = diff;
	ck_wunlock(&workbase_lock);

	if (old_diff < diff)
		return;

	/* If the diff has dropped, iterated over all the clients and check
	 * they're at or below the new diff, and update it if not. */
	ck_rlock(&instance_lock);
	for (client = stratum_instances; client != NULL; client = client->hh.next) {
		if (client->diff > diff) {
			client->diff = diff;
			stratum_send_diff(client);
		}
	}
	ck_runlock(&instance_lock);
}

/* Enter with instance_lock held */
static stratum_instance_t *__instance_by_id(int id)
{
	stratum_instance_t *instance;

	HASH_FIND_INT(stratum_instances, &id, instance);
	return instance;
}

/* Enter with write instance_lock held */
static stratum_instance_t *__stratum_add_instance(ckpool_t *ckp, int id)
{
	stratum_instance_t *instance = ckzalloc(sizeof(stratum_instance_t));

	instance->id = id;
	instance->diff = instance->old_diff = ckp->startdiff;
	instance->ckp = ckp;
	tv_time(&instance->ldc);
	LOGINFO("Added instance %d", id);
	HASH_ADD_INT(stratum_instances, id, instance);
	return instance;
}

static bool disconnected_sessionid_exists(const char *sessionid, int id)
{
	bool connected_exists = false, ret = false;
	stratum_instance_t *instance, *tmp;
	uint64_t session64;

	if (!sessionid)
		goto out;
	if (strlen(sessionid) != 16)
		goto out;
	/* Number is in BE but we don't swap either of them */
	hex2bin(&session64, sessionid, 8);

	ck_rlock(&instance_lock);
	HASH_ITER(hh, stratum_instances, instance, tmp) {
		if (instance->id == id)
			continue;
		if (instance->enonce1_64 == session64) {
			/* Only allow one connected instance per enonce1 */
			connected_exists = true;
			break;
		}
	}
	if (connected_exists)
		goto out_unlock;
	instance = NULL;
	HASH_FIND(hh, disconnected_instances, &session64, sizeof(uint64_t), instance);
	if (instance)
		ret = true;
out_unlock:
	ck_runlock(&instance_lock);
out:
	return ret;
}

static void stratum_add_recvd(json_t *val)
{
	smsg_t *msg;

	msg = ckzalloc(sizeof(smsg_t));
	msg->json_msg = val;
	ckmsgq_add(srecvs, msg);
}

/* Write the share summary to a tmp file first and then move
 * it to the user file to prevent leaving us without a file
 * if we abort at just the wrong time. */
static void log_pprop(const char *logdir, user_instance_t *instance)
{
	char fnametmp[512] = {}, fname[512] = {};
	FILE *fp;

	snprintf(fnametmp, 511, "%s/%stmp.pprop", logdir, instance->username);
	fp = fopen(fnametmp, "w");
	if (unlikely(!fp)) {
		LOGERR("Failed to fopen %s", fnametmp);
		return;
	}
	fprintf(fp, "%lu,%ld,%ld", instance->pprop_shares, instance->diff_accepted, instance->diff_rejected);
	fclose(fp);
	snprintf(fname, 511, "%s/%s.pprop", logdir, instance->username);
	if (rename(fnametmp, fname))
		LOGERR("Failed to rename %s to %s", fnametmp, fname);
}

/* For creating a list of sends without locking that can then be concatenated
 * to the stratum_sends list. Minimises locking and avoids taking recursive
 * locks. */
static void stratum_broadcast(json_t *val)
{
	stratum_instance_t *instance, *tmp;
	ckmsg_t *bulk_send = NULL;

	if (unlikely(!val)) {
		LOGERR("Sent null json to stratum_broadcast");
		return;
	}

	ck_rlock(&instance_lock);
	HASH_ITER(hh, stratum_instances, instance, tmp) {
		ckmsg_t *client_msg;
		smsg_t *msg;

		if (!instance->authorised)
			continue;
		client_msg = ckalloc(sizeof(ckmsg_t));
		msg = ckzalloc(sizeof(smsg_t));
		msg->json_msg = json_deep_copy(val);
		msg->client_id = instance->id;
		client_msg->data = msg;
		DL_APPEND(bulk_send, client_msg);
	}
	ck_runlock(&instance_lock);

	json_decref(val);

	if (!bulk_send)
		return;

	mutex_lock(&ssends->lock);
	if (ssends->msgs)
		DL_CONCAT(ssends->msgs, bulk_send);
	else
		ssends->msgs = bulk_send;
	pthread_cond_signal(&ssends->cond);
	mutex_unlock(&ssends->lock);
}

static void stratum_add_send(json_t *val, int client_id)
{
	smsg_t *msg;

	msg = ckzalloc(sizeof(smsg_t));
	msg->json_msg = val;
	msg->client_id = client_id;
	ckmsgq_add(ssends, msg);
}

static void drop_client(int id)
{
	stratum_instance_t *client = NULL;

	ck_ilock(&instance_lock);
	client = __instance_by_id(id);
	if (client) {
		stratum_instance_t *old_client = NULL;

		ck_ulock(&instance_lock);
		if (client->authorised && !--stats.workers)
			stats.users--;
		HASH_DEL(stratum_instances, client);
		HASH_FIND(hh, disconnected_instances, &client->enonce1_64, sizeof(uint64_t), old_client);
		/* Only keep around one copy of the old client */
		if (!old_client)
			HASH_ADD(hh, disconnected_instances, enonce1_64, sizeof(uint64_t), client);
		else // Keep around instance so we don't get a dereference
			HASH_ADD(hh, dead_instances, enonce1_64, sizeof(uint64_t), client);
		ck_dwilock(&instance_lock);
	}
	ck_uilock(&instance_lock);
}

static void stratum_broadcast_message(const char *msg)
{
	json_t *json_msg;

	json_msg = json_pack("{sosss[s]}", "id", json_null(), "method", "client.show_message",
			     "params", msg);
	stratum_broadcast(json_msg);
}

static void block_solve(ckpool_t *ckp)
{
	double round, total = 0, retain = 0, window;
	user_instance_t *instance, *tmp;
	char cdfield[64];
	ts_t ts_now;
	json_t *val;
	char *msg;

	ts_realtime(&ts_now);
	sprintf(cdfield, "%lu,%lu", ts_now.tv_sec, ts_now.tv_nsec);

	ck_rlock(&workbase_lock);
	/* We send blank settings to ckdb with only the matching data from what we submitted
	 * to say the block has been confirmed. */
	val = json_pack("{si,ss,sI,ss,ss,si,ss,ss,ss,sI,ss,ss,ss,ss}",
			"height", current_workbase->height,
			"confirmed", "1",
			"workinfoid", current_workbase->id,
			"username", "",
			"workername", "",
			"clientid", 0,
			"enonce1", "",
			"nonce2", "",
			"nonce", "",
			"reward", current_workbase->coinbasevalue,
			"createdate", cdfield,
			"createby", "code",
			"createcode", __func__,
			"createinet", "127.0.0.1");
	ck_runlock(&workbase_lock);

	update_base(ckp);

	ck_rlock(&workbase_lock);
	json_set_string(val, "blockhash", current_workbase->prevhash);
	ck_runlock(&workbase_lock);

	ckdbq_add(ckp, ID_BLOCK, val);

	ck_rlock(&workbase_lock);
	window = current_workbase->network_diff;
	ck_runlock(&workbase_lock);

	LOGWARNING("Block solve user summary");

	mutex_lock(&stats_lock);
	total = stats.pprop_shares;
	round = stats.round_shares;
	mutex_unlock(&stats_lock);

	if (unlikely(total == 0.0))
		total = 1;

	ck_rlock(&instance_lock);
	/* What proportion of shares should each user retain */
	if (total > window)
		retain = (total - window) / total;
	HASH_ITER(hh, user_instances, instance, tmp) {
		double residual, shares, percentage;

		shares = instance->pprop_shares;
		if (!shares)
			continue;
		residual = shares * retain;
		percentage = shares / total * 100;
		LOGWARNING("User %s: Reward: %f %%  Credited: %.0f  Remaining: %.0f",
			   instance->username, percentage, shares, residual);
		instance->pprop_shares = residual;
		instance->diff_accepted = instance->diff_rejected = 0;
		log_pprop(ckp->logdir, instance);
	}
	ck_runlock(&instance_lock);

	LOGWARNING("Round shares: %.0f  Total pprop: %.0f  pprop window %.0f", round, total, window);

	ASPRINTF(&msg, "Block solved by %s after %.0f shares!", ckp->name, round);

	stratum_broadcast_message(msg);
	free(msg);

	mutex_lock(&stats_lock);
	stats.round_shares = 0;
	stats.pprop_shares *= retain;
	mutex_unlock(&stats_lock);
}

static int stratum_loop(ckpool_t *ckp, proc_instance_t *pi)
{
	int sockd, ret = 0, selret = 0;
	unixsock_t *us = &pi->us;
	char *buf = NULL;
	tv_t start_tv;

	tv_time(&start_tv);
retry:
	do {
		if (!ckp->proxy) {
			double tdiff;
			tv_t end_tv;

			tv_time(&end_tv);
			tdiff = tvdiff(&end_tv, &start_tv);
			if (tdiff > ckp->update_interval) {
				copy_tv(&start_tv, &end_tv);
				LOGDEBUG("%ds elapsed in strat_loop, updating gbt base",
					 ckp->update_interval);
				update_base(ckp);
				continue;
			}
		}
		selret = wait_read_select(us->sockd, 5);
		if (!selret && !ping_main(ckp)) {
			LOGEMERG("Generator failed to ping main process, exiting");
			ret = 1;
			goto out;
		}
	} while (selret < 1);

	sockd = accept(us->sockd, NULL, NULL);
	if (sockd < 0) {
		LOGERR("Failed to accept on stratifier socket, exiting");
		ret = 1;
		goto out;
	}

	dealloc(buf);
	buf = recv_unix_msg(sockd);
	if (!buf) {
		close(sockd);
		LOGWARNING("Failed to get message in stratum_loop");
		goto retry;
	}
	if (!strncasecmp(buf, "ping", 4)) {
		LOGDEBUG("Stratifier received ping request");
		send_unix_msg(sockd, "pong");
		close(sockd);
		goto retry;
	}

	close(sockd);
	LOGDEBUG("Stratifier received request: %s", buf);
	if (!strncasecmp(buf, "shutdown", 8)) {
		ret = 0;
		goto out;
	} else if (!strncasecmp(buf, "update", 6)) {
		update_base(ckp);
	} else if (!strncasecmp(buf, "subscribe", 9)) {
		/* Proxifier has a new subscription */
		if (!update_subscribe(ckp))
			goto out;
	} else if (!strncasecmp(buf, "notify", 6)) {
		/* Proxifier has a new notify ready */
		update_notify(ckp);
	} else if (!strncasecmp(buf, "diff", 4)) {
		update_diff(ckp);
	} else if (!strncasecmp(buf, "dropclient", 10)) {
		int client_id;

		ret = sscanf(buf, "dropclient=%d", &client_id);
		if (ret < 0)
			LOGDEBUG("Stratifier failed to parse dropclient command: %s", buf);
		else
			drop_client(client_id);
	} else if (!strncasecmp(buf, "block", 5)) {
		block_solve(ckp);
	} else if (!strncasecmp(buf, "loglevel", 8)) {
		sscanf(buf, "loglevel=%d", &ckp->loglevel);
	} else {
		json_t *val = json_loads(buf, 0, NULL);

		if (!val) {
			LOGWARNING("Received unrecognised message: %s", buf);
		}  else
			stratum_add_recvd(val);
	}
	goto retry;

out:
	dealloc(buf);
	return ret;
}

static void *blockupdate(void *arg)
{
	ckpool_t *ckp = (ckpool_t *)arg;
	char *buf = NULL, hash[68];
	char request[8];

	pthread_detach(pthread_self());
	rename_proc("blockupdate");
	buf = send_recv_proc(ckp->generator, "getbest");
	if (buf && strncasecmp(buf, "Failed", 6))
		sprintf(request, "getbest");
	else
		sprintf(request, "getlast");

	memset(hash, 0, 68);
	while (42) {
		dealloc(buf);
		buf = send_recv_proc(ckp->generator, request);
		if (buf && strcmp(buf, hash) && strncasecmp(buf, "Failed", 6)) {
			strcpy(hash, buf);
			LOGNOTICE("Block hash changed to %s", hash);
			send_proc(ckp->stratifier, "update");
		} else
			cksleep_ms(ckp->blockpoll);
	}
	return NULL;
}

static void new_enonce1(stratum_instance_t *client)
{
	workbase_t *wb;

	ck_wlock(&workbase_lock);
	client->enonce1_64 = enonce1_64;
	wb = current_workbase;
	if (wb->enonce1varlen == 8) {
		enonce1_64++;
	} else if (wb->enonce1varlen == 2) {
		uint16_t *enonce1_16 = (uint16_t *)&enonce1_64;

		++(*enonce1_16);
	} else {
		uint32_t *enonce1_32 = (uint32_t *)&enonce1_64;

		++(*enonce1_32);
	}
	if (wb->enonce1constlen)
		memcpy(client->enonce1bin, wb->enonce1constbin, wb->enonce1constlen);
	memcpy(client->enonce1bin + wb->enonce1constlen, &client->enonce1_64, wb->enonce1varlen);
	__bin2hex(client->enonce1var, &client->enonce1_64, wb->enonce1varlen);
	__bin2hex(client->enonce1, client->enonce1bin, wb->enonce1constlen + wb->enonce1varlen);
	ck_wunlock(&workbase_lock);
}

/* Extranonce1 must be set here */
static json_t *parse_subscribe(int client_id, json_t *params_val)
{
	stratum_instance_t *client = NULL;
	bool old_match = false;
	int arr_size;
	json_t *ret;
	int n2len;

	if (unlikely(!json_is_array(params_val)))
		return json_string("params not an array");

	ck_rlock(&instance_lock);
	client = __instance_by_id(client_id);
	ck_runlock(&instance_lock);

	if (unlikely(!client)) {
		LOGERR("Failed to find client id %d in hashtable!", client_id);
		return NULL;
	}

	if (unlikely(!current_workbase))
		return json_string("Initialising");

	arr_size = json_array_size(params_val);
	if (arr_size > 0) {
		const char *buf;

		buf = json_string_value(json_array_get(params_val, 0));
		if (buf && strlen(buf))
			client->useragent = strdup(buf);
		else
			client->useragent = ckzalloc(1); // Set to ""
		if (arr_size > 1) {
			/* This would be the session id for reconnect, it will
			 * not work for clients on a proxied connection. */
			buf = json_string_value(json_array_get(params_val, 1));
			LOGDEBUG("Found old session id %s", buf);
			/* Add matching here */
			if (disconnected_sessionid_exists(buf, client_id)) {
				sprintf(client->enonce1, "%016lx", client->enonce1_64);
				old_match = true;
			}
		}
	} else
		client->useragent = ckzalloc(1);
	if (!old_match) {
		/* Create a new extranonce1 based on a uint64_t pointer */
		new_enonce1(client);
		LOGINFO("Set new subscription %d to new enonce1 %s", client->id,
			client->enonce1);
	} else {
		LOGINFO("Set new subscription %d to old matched enonce1 %s", client->id,
			 client->enonce1);
	}

	ck_rlock(&workbase_lock);
	if (likely(workbases))
		n2len = workbases->enonce2varlen;
	else
		n2len = 8;
	ret = json_pack("[[[s,s]],s,i]", "mining.notify", client->enonce1, client->enonce1,
			n2len);
	ck_runlock(&workbase_lock);

	return ret;
}

/* This simply strips off the first part of the workername and matches it to a
 * user or creates a new one. */
static user_instance_t *authorise_user(const char *workername)
{
	char *fullname = strdupa(workername);
	char *username = strsep(&fullname, ".");
	user_instance_t *instance;

	if (strlen(username) > 127)
		username[127] = '\0';

	ck_ilock(&instance_lock);
	HASH_FIND_STR(user_instances, username, instance);
	if (!instance) {
		/* New user instance */
		instance = ckzalloc(sizeof(user_instance_t));
		strcpy(instance->username, username);

		ck_ulock(&instance_lock);
		instance->id = user_instance_id++;
		HASH_ADD_STR(user_instances, username, instance);
		ck_dwilock(&instance_lock);
	}
	ck_uilock(&instance_lock);

	return instance;
}

/* Send this to the database and parse the response to authorise a user
 * and get SUID parameters back. We don't add these requests to the ckdbqueue
 * since we have to wait for the response but this is done from the authoriser
 * thread so it won't hold anything up but other authorisations. */
static bool send_recv_auth(stratum_instance_t *client)
{
	ckpool_t *ckp = client->ckp;
	char cdfield[64];
	bool ret = false;
	json_t *val;
	char *buf;
	ts_t now;

	ts_realtime(&now);
	sprintf(cdfield, "%lu,%lu", now.tv_sec, now.tv_nsec);

	val = json_pack("{ss,ss,ss,ss,si,ss,ss,ss,ss,ss}",
			"username", client->user_instance->username,
			"workername", client->workername,
			"poolinstance", ckp->name,
			"useragent", client->useragent,
			"clientid", client->id,
			"enonce1", client->enonce1,
			"createdate", cdfield,
			"createby", "code",
			"createcode", __func__,
			"createinet", client->address);
	buf = json_ckdb_call(ckp, ckdb_ids[ID_AUTH], val, false);
	if (likely(buf)) {
		char *secondaryuserid, *response = alloca(128);

		sscanf(buf, "id.%*d.%s", response);
		secondaryuserid = response;
		strsep(&secondaryuserid, ".");
		LOGINFO("User %s Worker %s got auth response: %s  suid: %s",
			client->user_instance->username, client->workername,
			response, secondaryuserid);
		if (!strcmp(response, "ok") && secondaryuserid) {
			client->secondaryuserid = strdup(secondaryuserid);
			ret = true;
		}
	} else {
		LOGWARNING("Got no auth response from ckdb :(");
		json_decref(val);
	}

	return ret;
}

static json_t *parse_authorise(stratum_instance_t *client, json_t *params_val, json_t **err_val, const char *address)
{
	bool ret = false;
	const char *buf;
	int arr_size;
	ts_t now;

	if (unlikely(!json_is_array(params_val))) {
		*err_val = json_string("params not an array");
		goto out;
	}
	arr_size = json_array_size(params_val);
	if (unlikely(arr_size < 1)) {
		*err_val = json_string("params missing array entries");
		goto out;
	}
	buf = json_string_value(json_array_get(params_val, 0));
	if (!buf) {
		*err_val = json_string("Invalid workername parameter");
		goto out;
	}
	if (!strlen(buf)) {
		*err_val = json_string("Empty workername parameter");
		goto out;
	}
	if (!memcmp(buf, ".", 1)) {
		*err_val = json_string("Empty username parameter");
		goto out;
	}
	client->user_instance = authorise_user(buf);
	client->user_id = client->user_instance->id;
	ts_realtime(&now);
	client->start_time = now.tv_sec;
	strcpy(client->address, address);

	LOGNOTICE("Authorised client %d worker %s as user %s", client->id, buf,
		  client->user_instance->username);
	client->workername = strdup(buf);
	if (client->ckp->standalone)
		ret = true;
	else
		ret = send_recv_auth(client);
	client->authorised = ret;
	if (client->authorised) {
		mutex_lock(&stats_lock);
		if (!stats.workers++)
			stats.users++;
		mutex_unlock(&stats_lock);
	}
out:
	return json_boolean(ret);
}

static void stratum_send_diff(stratum_instance_t *client)
{
	json_t *json_msg;

	json_msg = json_pack("{s[I]soss}", "params", client->diff, "id", json_null(),
			     "method", "mining.set_difficulty");
	stratum_add_send(json_msg, client->id);
}

static void stratum_send_message(stratum_instance_t *client, const char *msg)
{
	json_t *json_msg;

	json_msg = json_pack("{sosss[s]}", "id", json_null(), "method", "client.show_message",
			     "params", msg);
	stratum_add_send(json_msg, client->id);
}

static double time_bias(double tdiff, double period)
{
	double dexp = tdiff / period;

	/* Sanity check to prevent silly numbers for double accuracy **/
	if (unlikely(dexp > 36))
		dexp = 36;
	return 1.0 - 1.0 / exp(dexp);
}

/* Sanity check to prevent clock adjustments backwards from screwing up stats */
static double sane_tdiff(tv_t *end, tv_t *start)
{
	double tdiff = tvdiff(end, start);

	if (unlikely(tdiff < 0.001))
		tdiff = 0.001;
	return tdiff;
}

static void add_submit(stratum_instance_t *client, int diff, bool valid)
{
	double tdiff, bdiff, dsps, drr, network_diff, bias;
	int64_t next_blockid, optimal;
	ckpool_t *ckp = client->ckp;
	tv_t now_t;

	tv_time(&now_t);

	ck_rlock(&workbase_lock);
	next_blockid = workbase_id + 1;
	network_diff = current_workbase->network_diff;
	ck_runlock(&workbase_lock);

	tdiff = sane_tdiff(&now_t, &client->last_share);
	if (unlikely(!client->first_share.tv_sec)) {
		copy_tv(&client->first_share, &now_t);
		copy_tv(&client->ldc, &now_t);
	}
	decay_time(&client->dsps1, diff, tdiff, 60);
	decay_time(&client->dsps5, diff, tdiff, 300);
	decay_time(&client->dsps60, diff, tdiff, 3600);
	decay_time(&client->dsps1440, diff, tdiff, 86400);
	copy_tv(&client->last_share, &now_t);
	client->idle = false;

	client->ssdc++;
	bdiff = sane_tdiff(&now_t, &client->first_share);
	bias = time_bias(bdiff, 300);
	tdiff = sane_tdiff(&now_t, &client->ldc);

	mutex_lock(&stats_lock);
	if (valid) {
		stats.unaccounted_shares++;
		stats.unaccounted_diff_shares += diff;
	} else
		stats.unaccounted_rejects += diff;
	mutex_unlock(&stats_lock);

	if (valid) {
		user_instance_t *instance = client->user_instance;

		instance->pprop_shares += diff;
		log_pprop(ckp->logdir, instance);
	}
	/* Check the difficulty every 240 seconds or as many shares as we
	 * should have had in that time, whichever comes first. */
	if (client->ssdc < 72 && tdiff < 240)
		return;

	if (diff != client->diff)
		return;

	/* We have the effect of a change pending */
	if (client->diff_change_job_id >= next_blockid)
		return;

	/* Diff rate ratio */
	dsps = client->dsps5 / bias;
	drr = dsps / (double)client->diff;

	/* Optimal rate product is 0.3, allow some hysteresis. */
	if (drr > 0.15 && drr < 0.4)
		return;

	optimal = round(dsps * 3.33);
	if (optimal <= client->ckp->mindiff) {
		if (client->diff == client->ckp->mindiff)
			return;
		optimal = client->ckp->mindiff;
	}

	if (optimal > network_diff) {
		/* Intentionally round down here */
		optimal = network_diff;
		if (client->diff == optimal)
			return;
	}

	/* Don't drop diff to rapidly in case the client simply switched away
	 * and has just returned */
	if (optimal < client->diff / 2)
		optimal = client->diff / 2;
	client->ssdc = 0;

	LOGINFO("Client %d biased dsps %.2f dsps %.2f drr %.2f adjust diff from %ld to: %ld ",
		client->id, dsps, client->dsps5, drr, client->diff, optimal);

	copy_tv(&client->ldc, &now_t);
	client->diff_change_job_id = next_blockid;
	client->old_diff = client->diff;
	client->diff = optimal;
	stratum_send_diff(client);
}

/* We should already be holding the workbase_lock */
static void test_blocksolve(stratum_instance_t *client, workbase_t *wb, const uchar *data, double diff, const char *coinbase,
			    int cblen, const char *nonce2, const char *nonce)
{
	int transactions = wb->transactions + 1;
	char *gbt_block, varint[12];
	char hexcoinbase[512];
	json_t *val = NULL;
	char cdfield[64];
	ts_t ts_now;

	/* Submit anything over 95% of the diff in case of rounding errors */
	if (diff < current_workbase->network_diff * 0.95)
		return;

	LOGWARNING("Possible block solve diff %f !", diff);
	if (wb->proxy)
		return;

	ts_realtime(&ts_now);
	sprintf(cdfield, "%lu,%lu", ts_now.tv_sec, ts_now.tv_nsec);

	gbt_block = ckalloc(512);
	sprintf(gbt_block, "submitblock:");
	__bin2hex(gbt_block + 12, data, 80);
	if (transactions < 0xfd) {
		uint8_t val8 = transactions;

		__bin2hex(varint, (const unsigned char *)&val8, 1);
	} else if (transactions <= 0xffff) {
		uint16_t val16 = htole16(transactions);

		strcat(gbt_block, "fd");
		__bin2hex(varint, (const unsigned char *)&val16, 2);
	} else {
		uint32_t val32 = htole32(transactions);

		strcat(gbt_block, "fe");
		__bin2hex(varint, (const unsigned char *)&val32, 4);
	}
	strcat(gbt_block, varint);
	__bin2hex(hexcoinbase, coinbase, cblen);
	strcat(gbt_block, hexcoinbase);
	if (wb->transactions)
		realloc_strcat(&gbt_block, wb->txn_data);
	send_proc(wb->ckp->generator, gbt_block);
	free(gbt_block);
	val = json_pack("{si,ss,ss,sI,ss,ss,si,ss,ss,ss,sI,ss,ss,ss,ss}",
			"height", wb->height,
			"blockhash", data,
			"confirmed", "n",
			"workinfoid", wb->id,
			"username", client->user_instance->username,
			"workername", client->workername,
			"clientid", client->id,
			"enonce1", client->enonce1,
			"nonce2", nonce2,
			"nonce", nonce,
			"reward", wb->coinbasevalue,
			"createdate", cdfield,
			"createby", "code",
			"createcode", __func__,
			"createinet", "127.0.0.1");
	ckdbq_add(client->ckp, ID_BLOCK, val);
}

static double submission_diff(stratum_instance_t *client, workbase_t *wb, const char *nonce2,
			      uint32_t ntime32, const char *nonce, uchar *hash)
{
	unsigned char merkle_root[32], merkle_sha[64];
	uint32_t *data32, *swap32, benonce32;
	char *coinbase, data[80];
	uchar swap[80], hash1[32];
	int cblen, i;
	double ret;

	coinbase = alloca(wb->coinb1len + wb->enonce1constlen + wb->enonce1varlen + wb->enonce2varlen + wb->coinb2len);
	memcpy(coinbase, wb->coinb1bin, wb->coinb1len);
	cblen = wb->coinb1len;
	memcpy(coinbase + cblen, &client->enonce1bin, wb->enonce1constlen + wb->enonce1varlen);
	cblen += wb->enonce1constlen + wb->enonce1varlen;
	hex2bin(coinbase + cblen, nonce2, wb->enonce2varlen);
	cblen += wb->enonce2varlen;
	memcpy(coinbase + cblen, wb->coinb2bin, wb->coinb2len);
	cblen += wb->coinb2len;

	gen_hash((uchar *)coinbase, merkle_root, cblen);
	memcpy(merkle_sha, merkle_root, 32);
	for (i = 0; i < wb->merkles; i++) {
		memcpy(merkle_sha + 32, &wb->merklebin[i], 32);
		gen_hash(merkle_sha, merkle_root, 64);
		memcpy(merkle_sha, merkle_root, 32);
	}
	data32 = (uint32_t *)merkle_sha;
	swap32 = (uint32_t *)merkle_root;
	flip_32(swap32, data32);

	/* Copy the cached header binary and insert the merkle root */
	memcpy(data, wb->headerbin, 80);
	memcpy(data + 36, merkle_root, 32);

	/* Insert the nonce value into the data */
	hex2bin(&benonce32, nonce, 4);
	data32 = (uint32_t *)(data + 64 + 12);
	*data32 = benonce32;

	/* Insert the ntime value into the data */
	data32 = (uint32_t *)(data + 68);
	*data32 = htobe32(ntime32);

	/* Hash the share */
	data32 = (uint32_t *)data;
	swap32 = (uint32_t *)swap;
	flip_80(swap32, data32);
	sha256(swap, 80, hash1);
	sha256(hash1, 32, hash);

	/* Calculate the diff of the share here */
	ret = diff_from_target(hash);

	/* Test we haven't solved a block regardless of share status */
	test_blocksolve(client, wb, swap, ret, coinbase, cblen, nonce2, nonce);

	return ret;
}

static bool new_share(const uchar *hash, int64_t  wb_id)
{
	share_t *share, *match = NULL;
	bool ret = false;

	ck_ilock(&share_lock);
	HASH_FIND(hh, shares, hash, 32, match);
	if (match)
		goto out_unlock;
	share = ckzalloc(sizeof(share_t));
	memcpy(share->hash, hash, 32);
	share->workbase_id = wb_id;
	ck_ulock(&share_lock);
	HASH_ADD(hh, shares, hash, 32, share);
	ck_dwilock(&share_lock);
	ret = true;
out_unlock:
	ck_uilock(&share_lock);

	return ret;
}

/* Submit a share in proxy mode to the parent pool. workbase_lock is held */
static void submit_share(stratum_instance_t *client, int64_t jobid, const char *nonce2,
			 const char *ntime, const char *nonce, int msg_id)
{
	ckpool_t *ckp = client->ckp;
	json_t *json_msg;
	char enonce2[32];
	char *msg;

	sprintf(enonce2, "%s%s", client->enonce1var, nonce2);
	json_msg = json_pack("{sisssssssisi}", "jobid", jobid, "nonce2", enonce2,
			     "ntime", ntime, "nonce", nonce, "client_id", client->id,
			     "msg_id", msg_id);
	msg = json_dumps(json_msg, 0);
	json_decref(json_msg);
	send_proc(ckp->generator, msg);
	free(msg);
}

#define JSON_ERR(err) json_string(SHARE_ERR(err))

static json_t *parse_submit(stratum_instance_t *client, json_t *json_msg,
			    json_t *params_val, json_t **err_val)
{
	bool share = false, result = false, invalid = true, submit = false;
	const char *user, *job_id, *nonce2, *ntime, *nonce;
	double diff = client->diff, wdiff = 0, sdiff = -1;
	char hexhash[68] = {}, sharehash[32], cdfield[64];
	enum share_err err = SE_NONE;
	ckpool_t *ckp = client->ckp;
	char idstring[20];
	uint32_t ntime32;
	char *fname, *s;
	workbase_t *wb;
	uchar hash[32];
	int64_t id;
	json_t *val;
	ts_t now;
	FILE *fp;
	int len;

	ts_realtime(&now);
	sprintf(cdfield, "%lu,%lu", now.tv_sec, now.tv_nsec);

	if (unlikely(!json_is_array(params_val))) {
		err = SE_NOT_ARRAY;
		*err_val = JSON_ERR(err);
		goto out;
	}
	if (unlikely(json_array_size(params_val) != 5)) {
		err = SE_INVALID_SIZE;
		*err_val = JSON_ERR(err);
		goto out;
	}
	user = json_string_value(json_array_get(params_val, 0));
	if (unlikely(!user || !strlen(user))) {
		err = SE_NO_USERNAME;
		*err_val = JSON_ERR(err);
		goto out;
	}
	job_id = json_string_value(json_array_get(params_val, 1));
	if (unlikely(!job_id || !strlen(job_id))) {
		err = SE_NO_JOBID;
		*err_val = JSON_ERR(err);
		goto out;
	}
	nonce2 = json_string_value(json_array_get(params_val, 2));
	if (unlikely(!nonce2 || !strlen(nonce2))) {
		err = SE_NO_NONCE2;
		*err_val = JSON_ERR(err);
		goto out;
	}
	ntime = json_string_value(json_array_get(params_val, 3));
	if (unlikely(!ntime || !strlen(ntime))) {
		err = SE_NO_NTIME;
		*err_val = JSON_ERR(err);
		goto out;
	}
	nonce = json_string_value(json_array_get(params_val, 4));
	if (unlikely(!nonce || !strlen(nonce))) {
		err = SE_NO_NONCE;
		*err_val = JSON_ERR(err);
		goto out;
	}
	if (strcmp(user, client->workername)) {
		err = SE_WORKER_MISMATCH;
		*err_val = JSON_ERR(err);
		goto out;
	}
	sscanf(job_id, "%lx", &id);
	sscanf(ntime, "%x", &ntime32);

	share = true;

	ck_rlock(&workbase_lock);
	HASH_FIND_INT(workbases, &id, wb);
	if (unlikely(!wb)) {
		err = SE_INVALID_JOBID;
		json_set_string(json_msg, "reject-reason", SHARE_ERR(err));
		strcpy(idstring, job_id);
		ASPRINTF(&fname, "%s.sharelog", current_workbase->logdir);
		goto out_unlock;
	}
	wdiff = wb->diff;
	strcpy(idstring, wb->idstring);
	ASPRINTF(&fname, "%s.sharelog", wb->logdir);
	sdiff = submission_diff(client, wb, nonce2, ntime32, nonce, hash);
	bswap_256(sharehash, hash);
	__bin2hex(hexhash, sharehash, 32);

	if (id < blockchange_id) {
		err = SE_STALE;
		json_set_string(json_msg, "reject-reason", SHARE_ERR(err));
		goto out_unlock;
	}
	if ((int)strlen(nonce2) != wb->enonce2varlen * 2) {
		err = SE_INVALID_NONCE2;
		*err_val = JSON_ERR(err);
		goto out_unlock;
	}
	/* Ntime cannot be less, but allow forward ntime rolling up to max */
	if (ntime32 < wb->ntime32 || ntime32 > wb->ntime32 + 7000) {
		err = SE_NTIME_INVALID;
		json_set_string(json_msg, "reject-reason", SHARE_ERR(err));
		goto out_unlock;
	}
	invalid = false;
	if (wb->proxy && sdiff > wdiff)
		submit = true;
out_unlock:
	ck_runlock(&workbase_lock);

	if (submit)
		submit_share(client, id, nonce2, ntime, nonce, json_integer_value(json_object_get(json_msg, "id")));

	/* Accept the lower of new and old diffs until the next update */
	if (id < client->diff_change_job_id && client->old_diff < client->diff)
		diff = client->old_diff;
	if (!invalid) {
		char wdiffsuffix[16];

		suffix_string(wdiff, wdiffsuffix, 16, 0);
		if (sdiff >= diff) {
			if (new_share(hash, id)) {
				LOGINFO("Accepted client %d share diff %.1f/%.0f/%s: %s",
					client->id, sdiff, diff, wdiffsuffix, hexhash);
				result = true;
			} else {
				err = SE_DUPE;
				json_set_string(json_msg, "reject-reason", SHARE_ERR(err));
				LOGINFO("Rejected client %d dupe diff %.1f/%.0f/%s: %s",
					client->id, sdiff, diff, wdiffsuffix, hexhash);
			}
		} else {
			err = SE_HIGH_DIFF;
			LOGINFO("Rejected client %d high diff %.1f/%.0f/%s: %s",
				client->id, sdiff, diff, wdiffsuffix, hexhash);
			json_set_string(json_msg, "reject-reason", SHARE_ERR(err));
		}
	}  else
		LOGINFO("Rejected client %d invalid share", client->id);
	add_submit(client, diff, result);

	/* Now write to the pool's sharelog. */
	val = json_object();
	json_set_int(val, "workinfoid", id);
	json_set_int(val, "clientid", client->id);
	json_set_string(val, "enonce1", client->enonce1);
	json_set_string(val, "secondaryuserid", client->secondaryuserid);
	json_set_string(val, "nonce2", nonce2);
	json_set_string(val, "nonce", nonce);
	json_set_string(val, "ntime", ntime);
	json_set_double(val, "diff", diff);
	json_set_double(val, "sdiff", sdiff);
	json_set_string(val, "hash", hexhash);
	json_set_bool(val, "result", result);
	json_object_set(val, "reject-reason", json_object_dup(json_msg, "reject-reason"));
	json_object_set(val, "error", *err_val);
	json_set_int(val, "errn", err);
	json_set_string(val, "createdate", cdfield);
	json_set_string(val, "createby", "code");
	json_set_string(val, "createcode", __func__);
	json_set_string(val, "createinet", "127.0.0.1");
	json_set_string(val, "workername", client->workername);
	json_set_string(val, "username", client->user_instance->username);

	fp = fopen(fname, "a");
	if (likely(fp)) {
		s = json_dumps(val, 0);
		len = strlen(s);
		len = fprintf(fp, "%s\n", s);
		free(s);
		fclose(fp);
		if (unlikely(len < 0))
			LOGERR("Failed to fwrite to %s", fname);
	} else
		LOGERR("Failed to fopen %s", fname);
	ckdbq_add(ckp, ID_SHARES, val);
out:
	if (!share) {
		val = json_pack("{si,ss,ss,sI,ss,ss,so,si,ss,ss,ss,ss}",
				"clientid", client->id,
				"secondaryuserid", client->secondaryuserid,
				"enonce1", client->enonce1,
				"workinfoid", current_workbase->id,
				"workername", client->workername,
				"username", client->user_instance->username,
				"error", json_copy(*err_val),
				"errn", err,
				"createdate", cdfield,
				"createby", "code",
				"createcode", __func__,
				"createinet", "127.0.0.1");
		ckdbq_add(ckp, ID_SHAREERR, val);
		LOGINFO("Invalid share from client %d: %s", client->id, client->workername);
	}
	return json_boolean(result);
}

/* Must enter with workbase_lock held */
static json_t *__stratum_notify(bool clean)
{
	json_t *val;

	val = json_pack("{s:[ssssosssb],s:o,s:s}",
			"params",
			current_workbase->idstring,
			current_workbase->prevhash,
			current_workbase->coinb1,
			current_workbase->coinb2,
			json_copy(current_workbase->merkle_array),
			current_workbase->bbversion,
			current_workbase->nbit,
			current_workbase->ntime,
			clean,
			"id", json_null(),
			"method", "mining.notify");
	return val;
}

static void stratum_broadcast_update(bool clean)
{
	json_t *json_msg;

	ck_rlock(&workbase_lock);
	json_msg = __stratum_notify(clean);
	ck_runlock(&workbase_lock);

	stratum_broadcast(json_msg);
}

/* For sending a single stratum template update */
static void stratum_send_update(int client_id, bool clean)
{
	json_t *json_msg;

	ck_rlock(&workbase_lock);
	json_msg = __stratum_notify(clean);
	ck_runlock(&workbase_lock);

	stratum_add_send(json_msg, client_id);
}

static void send_json_err(int client_id, json_t *id_val, const char *err_msg)
{
	json_t *val;

	val = json_pack("{soss}", "id", json_copy(id_val), "error", err_msg);
	stratum_add_send(val, client_id);
}

static void update_client(const int client_id)
{
	stratum_instance_t *client;

	stratum_send_update(client_id, true);

	ck_rlock(&instance_lock);
	client = __instance_by_id(client_id);
	ck_runlock(&instance_lock);

	if (likely(client))
		stratum_send_diff(client);
}

static json_params_t *create_json_params(const int client_id, const json_t *params, const json_t *id_val, const char *address)
{
	json_params_t *jp = ckalloc(sizeof(json_params_t));

	jp->params = json_deep_copy(params);
	jp->id_val = json_deep_copy(id_val);
	jp->client_id = client_id;
	strcpy(jp->address, address);
	return jp;
}

static void parse_method(const int client_id, json_t *id_val, json_t *method_val,
			 json_t *params_val, char *address)
{
	stratum_instance_t *client;
	const char *method;

	/* Random broken clients send something not an integer as the id so we copy
	 * the json item for id_val as is for the response. */
	method = json_string_value(method_val);
	if (!strncasecmp(method, "mining.subscribe", 16)) {
		json_t *val, *result_val = parse_subscribe(client_id, params_val);

		if (!result_val)
			return;
		val = json_object();
		json_object_set_new_nocheck(val, "result", result_val);
		json_object_set_nocheck(val, "id", id_val);
		json_object_set_new_nocheck(val, "error", json_null());
		stratum_add_send(val, client_id);
		update_client(client_id);
		return;
	}

	ck_rlock(&instance_lock);
	client = __instance_by_id(client_id);
	ck_runlock(&instance_lock);

	if (unlikely(!client)) {
		LOGINFO("Failed to find client id %d in hashtable!", client_id);
		return;
	}

	if (!strncasecmp(method, "mining.auth", 11)) {
		json_params_t *jp = create_json_params(client_id, params_val, id_val, address);

		ckmsgq_add(sauthq, jp);
		return;
	}

	/* We should only accept authorised requests from here on */
	if (!client->authorised) {
		char buf[256];

		/* Dropping unauthorised clients here also allows the
		 * stratifier process to restart since it will have lost all
		 * the stratum instance data. Clients will just reconnect. */
		LOGINFO("Dropping unauthorised client %d", client->id);
		snprintf(buf, 255, "dropclient=%d", client->id);
		send_proc(client->ckp->connector, buf);
		return;
	}

	if (!strncasecmp(method, "mining.submit", 13)) {
		json_params_t *jp = create_json_params(client_id, params_val, id_val, address);

		ckmsgq_add(sshareq, jp);
		return;
	}

	/* Unhandled message here */
}

static void parse_instance_msg(smsg_t *msg)
{
	json_t *val = msg->json_msg, *id_val, *method, *params;
	int client_id = msg->client_id;

	/* Return back the same id_val even if it's null or not existent. */
	id_val = json_object_get(val, "id");

	method = json_object_get(val, "method");
	if (unlikely(!method)) {
		send_json_err(client_id, id_val, "-3:method not found");
		goto out;
	}
	if (unlikely(!json_is_string(method))) {
		send_json_err(client_id, id_val, "-1:method is not string");
		goto out;
	}
	params = json_object_get(val, "params");
	if (unlikely(!params)) {
		send_json_err(client_id, id_val, "-1:params not found");
		goto out;
	}
	parse_method(client_id, id_val, method, params, msg->address);
out:
	json_decref(val);
	free(msg);
}

static void srecv_process(ckpool_t *ckp, smsg_t *msg)
{
	stratum_instance_t *instance;
	json_t *val;

	val = json_object_get(msg->json_msg, "client_id");
	if (unlikely(!val)) {
		char *s;

		s = json_dumps(msg->json_msg, 0);
		LOGWARNING("Failed to extract client_id from connector json smsg %s", s);
		free(s);
		json_decref(msg->json_msg);
		free(msg);
		return;
	}

	msg->client_id = json_integer_value(val);
	json_object_clear(val);

	val = json_object_get(msg->json_msg, "address");
	if (unlikely(!val)) {
		char *s;

		s = json_dumps(msg->json_msg, 0);
		LOGWARNING("Failed to extract address from connector json smsg %s", s);
		free(s);
		json_decref(msg->json_msg);
		free(msg);
		return;
	}
	strcpy(msg->address, json_string_value(val));
	json_object_clear(val);

	/* Parse the message here */
	ck_ilock(&instance_lock);
	instance = __instance_by_id(msg->client_id);
	if (!instance) {
		/* client_id instance doesn't exist yet, create one */
		ck_ulock(&instance_lock);
		instance = __stratum_add_instance(ckp, msg->client_id);
		ck_dwilock(&instance_lock);
	}
	ck_uilock(&instance_lock);

	parse_instance_msg(msg);

}

static void discard_stratum_msg(smsg_t **msg)
{
	json_decref((*msg)->json_msg);
	free(*msg);
	*msg = NULL;
}

static void ssend_process(ckpool_t *ckp, smsg_t *msg)
{
	char *s;

	if (unlikely(!msg->json_msg)) {
		LOGERR("Sent null json msg to stratum_sender");
		free(msg);
		return;
	}

	/* Add client_id to the json message and send it to the
	 * connector process to be delivered */
	json_object_set_new_nocheck(msg->json_msg, "client_id", json_integer(msg->client_id));
	s = json_dumps(msg->json_msg, 0);
	send_proc(ckp->connector, s);
	free(s);
	discard_stratum_msg(&msg);
}

static void discard_json_params(json_params_t **jp)
{
	json_decref((*jp)->params);
	json_decref((*jp)->id_val);
	free(*jp);
	*jp = NULL;
}

static void sshare_process(ckpool_t __maybe_unused *ckp, json_params_t *jp)
{
	json_t *result_val, *json_msg, *err_val = NULL;
	stratum_instance_t *client;
	int client_id;

	client_id = jp->client_id;

	ck_rlock(&instance_lock);
	client = __instance_by_id(client_id);
	ck_runlock(&instance_lock);

	if (unlikely(!client)) {
		LOGINFO("Share processor failed to find client id %d in hashtable!", client_id);
		goto out;
	}
	json_msg = json_object();
	result_val = parse_submit(client, json_msg, jp->params, &err_val);
	json_object_set_new_nocheck(json_msg, "result", result_val);
	json_object_set_new_nocheck(json_msg, "error", err_val ? err_val : json_null());
	json_object_set_nocheck(json_msg, "id", jp->id_val);
	stratum_add_send(json_msg, client_id);
out:
	discard_json_params(&jp);
}

static void sauth_process(ckpool_t *ckp, json_params_t *jp)
{
	json_t *result_val, *json_msg, *err_val = NULL;
	stratum_instance_t *client;
	int client_id;

	client_id = jp->client_id;

	ck_rlock(&instance_lock);
	client = __instance_by_id(client_id);
	ck_runlock(&instance_lock);

	if (unlikely(!client)) {
		LOGINFO("Authoriser failed to find client id %d in hashtable!", client_id);
		goto out;
	}
	result_val = parse_authorise(client, jp->params, &err_val, jp->address);
	if (json_is_true(result_val)) {
		char *buf;

		ASPRINTF(&buf, "Authorised, welcome to %s %s!", ckp->name,
			 client->user_instance->username);
		stratum_send_message(client, buf);
	} else
		stratum_send_message(client, "Failed authorisation :(");
	json_msg = json_object();
	json_object_set_new_nocheck(json_msg, "result", result_val);
	json_object_set_new_nocheck(json_msg, "error", err_val ? err_val : json_null());
	json_object_set_nocheck(json_msg, "id", jp->id_val);
	stratum_add_send(json_msg, client_id);
out:
	discard_json_params(&jp);

}

static void ckdbq_process(ckpool_t *ckp, ckdb_msg_t *data)
{
	bool logged = false;
	char *buf = NULL;

	while (!buf) {
		buf = json_ckdb_call(ckp, ckdb_ids[data->idtype], data->val, logged);
		if (unlikely(!buf)) {
			LOGWARNING("Failed to talk to ckdb, queueing messages");
			sleep(5);
		}
		logged = true;
	}
	LOGINFO("Got %s ckdb response: %s", ckdb_ids[data->idtype], buf);
	free(buf);
}

static const double nonces = 4294967296;

/* Called every 20 seconds, we send the updated stats to ckdb of those users
 * who have gone 10 minutes between updates. This ends up staggering stats to
 * avoid floods of stat data coming at once. */
static void update_userstats(ckpool_t *ckp)
{
	stratum_instance_t *client, *tmp;
	json_t *val = NULL;
	char cdfield[64];
	time_t now_t;
	ts_t ts_now;

	if (++stats.userstats_cycle > 0x1f)
		stats.userstats_cycle = 0;

	ts_realtime(&ts_now);
	sprintf(cdfield, "%lu,%lu", ts_now.tv_sec, ts_now.tv_nsec);
	now_t = ts_now.tv_sec;

	ck_rlock(&instance_lock);
	HASH_ITER(hh, stratum_instances, client, tmp) {
		double ghs1, ghs5, ghs60, ghs1440;
		uint8_t cycle_mask;
		int elapsed;

		/* Send one lot of stats once the client is idle if they have submitted
		 * no shares in the last 10 minutes with the idle bool set. */
		if (client->idle && client->notified_idle)
			continue;
		/* Select clients using a mask to return each user's stats once
		 * every ~10 minutes */
		cycle_mask = client->user_id & 0x1f;
		if (cycle_mask != stats.userstats_cycle)
			continue;

		if (val) {
			json_set_bool(val,"eos", false);
			ckdbq_add(ckp, ID_USERSTATS, val);
			val = NULL;
		}
		elapsed = now_t - client->start_time;
		ghs1 = client->dsps1 * nonces;
		ghs5 = client->dsps5 * nonces;
		ghs60 = client->dsps60 * nonces;
		ghs1440 = client->dsps1440 * nonces;
		val = json_pack("{ss,si,si,ss,ss,sf,sf,sf,sf,sb,ss,ss,ss,ss}",
				"poolinstance", ckp->name,
				"instanceid", client->id,
				"elapsed", elapsed,
				"username", client->user_instance->username,
				"workername", client->workername,
				"hashrate", ghs1,
				"hashrate5m", ghs5,
				"hashrate1hr", ghs60,
				"hashrate24hr", ghs1440,
				"idle", client->idle,
				"createdate", cdfield,
				"createby", "code",
				"createcode", __func__,
				"createinet", "127.0.0.1");
		client->notified_idle = client->idle;
	}
	/* Mark the last userstats sent on this pass of stats with an end of
	 * stats marker. */
	if (val) {
		json_set_bool(val,"eos", true);
		ckdbq_add(ckp, ID_USERSTATS, val);
	}
	ck_runlock(&instance_lock);
}

static void *statsupdate(void *arg)
{
	ckpool_t *ckp = (ckpool_t *)arg;

	pthread_detach(pthread_self());
	rename_proc("statsupdate");

	tv_time(&stats.start_time);
	cksleep_prepare_r(&stats.last_update);

	while (42) {
		char suffix1[16], suffix5[16], suffix15[16], suffix60[16], cdfield[64];
		double ghs1, ghs5, ghs15, ghs60, ghs360, ghs1440, tdiff, bias;
		char suffix360[16], suffix1440[16];
		double sps1, sps5, sps15, sps60;
		stratum_instance_t *client, *tmp;
		int64_t pprop_shares;
		char fname[512] = {};
		tv_t now, diff;
		ts_t ts_now;
		json_t *val;
		FILE *fp;
		char *s;
		int i;

		tv_time(&now);
		timersub(&now, &stats.start_time, &diff);
		tdiff = diff.tv_sec + (double)diff.tv_usec / 1000000;

		bias = time_bias(tdiff, 60);
		ghs1 = stats.dsps1 * nonces / bias;
		suffix_string(ghs1, suffix1, 16, 0);
		sps1 = stats.sps1 / bias;

		bias = time_bias(tdiff, 300);
		ghs5 = stats.dsps5 * nonces / bias;
		suffix_string(ghs5, suffix5, 16, 0);
		sps5 = stats.sps5 / bias;

		bias = time_bias(tdiff, 900);
		ghs15 = stats.dsps15 * nonces / bias;
		suffix_string(ghs15, suffix15, 16, 0);
		sps15 = stats.sps15 / bias;

		bias = time_bias(tdiff, 3600);
		ghs60 = stats.dsps60 * nonces / bias;
		suffix_string(ghs60, suffix60, 16, 0);
		sps60 = stats.sps60 / bias;

		bias = time_bias(tdiff, 21600);
		ghs360 = stats.dsps360 * nonces / bias;
		suffix_string(ghs360, suffix360, 16, 0);

		bias = time_bias(tdiff, 86400);
		ghs1440 = stats.dsps1440 * nonces / bias;
		suffix_string(ghs1440, suffix1440, 16, 0);

		snprintf(fname, 511, "%s/pool.status", ckp->logdir);
		fp = fopen(fname, "w");
		if (unlikely(!fp))
			LOGERR("Failed to fopen %s", fname);

		pprop_shares = stats.pprop_shares + 1;

		val = json_pack("{si,si,si}",
				"runtime", diff.tv_sec,
				"Users", stats.users,
				"Workers", stats.workers);
		s = json_dumps(val, 0);
		json_decref(val);
		LOGNOTICE("Pool:%s", s);
		fprintf(fp, "%s\n", s);
		dealloc(s);

		val = json_pack("{ss,ss,ss,ss,ss,ss}",
				"hashrate1m", suffix1,
				"hashrate5m", suffix5,
				"hashrate15m", suffix15,
				"hashrate1hr", suffix60,
				"hashrate6hr", suffix360,
				"hashrate1d", suffix1440);
		s = json_dumps(val, 0);
		json_decref(val);
		LOGNOTICE("Pool:%s", s);
		fprintf(fp, "%s\n", s);
		dealloc(s);

		val = json_pack("{sI,sf,sf,sf,sf}",
				"Round shares", stats.round_shares,
				"SPS1m", sps1,
				"SPS5m", sps5,
				"SPS15m", sps15,
				"SPS1h", sps60);
		s = json_dumps(val, 0);
		json_decref(val);
		LOGNOTICE("Pool:%s", s);
		fprintf(fp, "%s\n", s);
		dealloc(s);
		fclose(fp);

		ck_rlock(&instance_lock);
		HASH_ITER(hh, stratum_instances, client, tmp) {
			user_instance_t *instance = client->user_instance;
			double reward, ghs;
			bool idle = false;

			if (now.tv_sec - client->last_share.tv_sec > 60) {
				idle = true;
				/* No shares for over a minute, decay to 0 */
				decay_time(&client->dsps1, 0, tdiff, 60);
				decay_time(&client->dsps5, 0, tdiff, 300);
				decay_time(&client->dsps60, 0, tdiff, 3600);
				decay_time(&client->dsps1440, 0, tdiff, 86400);
				if (now.tv_sec - client->last_share.tv_sec > 600)
					client->idle = true;
			}
			ghs = client->dsps1 * nonces;
			suffix_string(ghs, suffix1, 16, 0);
			ghs = client->dsps5 * nonces;
			suffix_string(ghs, suffix5, 16, 0);
			ghs = client->dsps60 * nonces;
			suffix_string(ghs, suffix60, 16, 0);
			ghs = client->dsps1440 * nonces;
			suffix_string(ghs, suffix1440, 16, 0);

			reward = 25 * instance->pprop_shares;
			reward /= pprop_shares;
			val = json_pack("{sI,sI,sf,ss,ss,ss,ss,ss,ss}",
					"Accepted", instance->diff_accepted,
					"Rejected", instance->diff_rejected,
					"Est reward", reward,
					"hashrate1m", suffix1,
					"hashrate5m", suffix5,
					"hashrate1hr", suffix60,
					"hashrate1d", suffix1440);

			snprintf(fname, 511, "%s/%s", ckp->logdir, client->workername);
			fp = fopen(fname, "w");
			if (unlikely(!fp)) {
				LOGERR("Failed to fopen %s", fname);
				continue;
			}
			s = json_dumps(val, 0);
			fprintf(fp, "%s\n", s);
			/* Only display the status of connected users to the
			 * console log. */
			if (!idle)
				LOGNOTICE("Worker %s:%s", client->workername, s);
			dealloc(s);
			json_decref(val);
			fclose(fp);
		}
		ck_runlock(&instance_lock);

		ts_realtime(&ts_now);
		sprintf(cdfield, "%lu,%lu", ts_now.tv_sec, ts_now.tv_nsec);
		val = json_pack("{ss,si,si,si,sf,sf,sf,sf,ss,ss,ss,ss}",
				"poolinstance", ckp->name,
				"elapsed", diff.tv_sec,
				"users", stats.users,
				"workers", stats.workers,
				"hashrate", ghs1,
				"hashrate5m", ghs5,
				"hashrate1hr", ghs60,
				"hashrate24hr", ghs1440,
				"createdate", cdfield,
				"createby", "code",
				"createcode", __func__,
				"createinet", "127.0.0.1");
		ckdbq_add(ckp, ID_POOLSTATS, val);

		/* Update stats 3 times per minute for smooth values, displaying
		 * status every minute. */
		for (i = 0; i < 3; i++) {
			cksleep_ms_r(&stats.last_update, 20000);
			cksleep_prepare_r(&stats.last_update);
			update_userstats(ckp);

			mutex_lock(&stats_lock);
			stats.accounted_shares += stats.unaccounted_shares;
			stats.accounted_diff_shares += stats.unaccounted_diff_shares;
			stats.accounted_rejects += stats.unaccounted_rejects;
			stats.round_shares += stats.unaccounted_diff_shares;
			stats.pprop_shares += stats.unaccounted_diff_shares;

			decay_time(&stats.sps1, stats.unaccounted_shares, 15, 60);
			decay_time(&stats.sps5, stats.unaccounted_shares, 15, 300);
			decay_time(&stats.sps15, stats.unaccounted_shares, 15, 900);
			decay_time(&stats.sps60, stats.unaccounted_shares, 15, 3600);

			decay_time(&stats.dsps1, stats.unaccounted_diff_shares, 15, 60);
			decay_time(&stats.dsps5, stats.unaccounted_diff_shares, 15, 300);
			decay_time(&stats.dsps15, stats.unaccounted_diff_shares, 15, 900);
			decay_time(&stats.dsps60, stats.unaccounted_diff_shares, 15, 3600);
			decay_time(&stats.dsps360, stats.unaccounted_diff_shares, 15, 21600);
			decay_time(&stats.dsps1440, stats.unaccounted_diff_shares, 15, 86400);

			stats.unaccounted_shares =
			stats.unaccounted_diff_shares =
			stats.unaccounted_rejects = 0;
			mutex_unlock(&stats_lock);
		}
	}

	return NULL;
}

static void load_users(ckpool_t *ckp)
{
	struct dirent *ep;
	DIR *dp;

	dp = opendir(ckp->logdir);
	if (!dp)
		quit(1, "Failed to open logdir %s!", ckp->logdir);
	while ((ep = readdir(dp))) {
		int64_t diff_accepted, diff_rejected = 0;
		user_instance_t *instance;
		uint64_t pprop_shares;
		char fname[512] = {};
		char *period;
		int results;
		FILE *fp;

		if (strlen(ep->d_name) < 7)
			continue;
		if (!strstr(ep->d_name, ".pprop"))
			continue;

		snprintf(fname, 511, "%s%s", ckp->logdir, ep->d_name);
		fp = fopen(fname, "r");
		if (!fp) {
			LOGERR("Failed to open pprop logfile %s!", fname);
			continue;
		}
		results = fscanf(fp, "%lu,%ld,%ld", &pprop_shares, &diff_accepted, &diff_rejected);
		if (results < 1)
			continue;
		if (results == 1)
			diff_accepted = pprop_shares;
		if (!pprop_shares)
			continue;

		/* Create a new user instance */
		instance = ckzalloc(sizeof(user_instance_t));
		strncpy(instance->username, ep->d_name, 127);
		period = strstr(instance->username, ".");
		*period = '\0';
		instance->pprop_shares = pprop_shares;
		stats.pprop_shares += pprop_shares;
		instance->diff_accepted = diff_accepted;
		instance->diff_rejected = diff_rejected;
		stats.round_shares += diff_accepted;

		ck_wlock(&instance_lock);
		HASH_ADD_STR(user_instances, username, instance);
		ck_wunlock(&instance_lock);

		LOGDEBUG("Added user %s with %lu shares", instance->username, pprop_shares);
	}
	closedir(dp);
}

int stratifier(proc_instance_t *pi)
{
	pthread_t pth_blockupdate, pth_statsupdate;
	ckpool_t *ckp = pi->ckp;
	char *buf;
	int ret;

	/* Store this for use elsewhere */
	hex2bin(scriptsig_header_bin, scriptsig_header, 41);
	address_to_pubkeytxn(pubkeytxnbin, ckp->btcaddress);
	__bin2hex(pubkeytxn, pubkeytxnbin, 25);

	/* Set the initial id to time as high bits so as to not send the same
	 * id on restarts */
	blockchange_id = workbase_id = ((int64_t)time(NULL)) << 32;

	/* Wait for the generator to have something for us */
	do {
		if (!ping_main(ckp)) {
			ret = 1;
			goto out;
		}
		buf = send_recv_proc(ckp->generator, "ping");
	} while (!buf);
	dealloc(buf);

	cklock_init(&instance_lock);

	ssends = create_ckmsgq(ckp, "ssender", &ssend_process);
	srecvs = create_ckmsgq(ckp, "sreceiver", &srecv_process);
	sshareq = create_ckmsgq(ckp, "sprocessor", &sshare_process);
	sauthq = create_ckmsgq(ckp, "authoriser", &sauth_process);
	ckdbq = create_ckmsgq(ckp, "ckdbqueue", &ckdbq_process);

	cklock_init(&workbase_lock);
	if (!ckp->proxy)
		create_pthread(&pth_blockupdate, blockupdate, ckp);

	mutex_init(&stats_lock);
	create_pthread(&pth_statsupdate, statsupdate, ckp);

	cklock_init(&share_lock);

	load_users(ckp);

	ret = stratum_loop(ckp, pi);
out:
	return process_exit(ckp, pi, ret);
}
