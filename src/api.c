/*
 * Copyright 2014-2015 Con Kolivas
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 */

#include "config.h"

#include <jansson.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>

#include "api.h"

#include "libckpool.h"
#include "generator.h"
#include "stratifier.h"
#include "connector.h"

#define PROC_MAIN	0
#define PROC_GENERATOR	1
#define PROC_STRATIFER	2
#define PROC_CONNECTOR	3

/*

API JSON COMMAND STRUCTURE:
{"command":$cmdname, "params":{$params}}
params are only mandatory for certain commands and can otherwise be omitted.

API JSON RESPONSE STRUCTURE:
{"result":$boolean, "error":$errorval, "response":$responseval}

 Result/error is whether we successfully passed the command to the relevant
 ckpool process.

 Responseval is a json structure in its own right and gives the process'
 response to the request and includes its own errormsg should the query fail.

$responseval includes the key errormsg which is null on success

ERROR VALUES:
-1, "Invalid json"
-2, "No command"
-3, "Unknown command"
-4, "Missing params"
-5, "No process response"	:internal code error
-6: "Invalid json response" 	:internal code error


COMMANDS WITHOUT PARAMS:
	COMMAND
	clients.info
	connector.stats
	generator.stats
	proxy.list
	stratifier.stats
	users.info
	workers.info


COMMANDS WITH PARAMS:
	COMMAND			PARAMS				OPTIONAL
	client.get		id:$clientid
	proxy.add		url:$url,auth:$auth,pass:$pass	userid:$userid
	proxy.del		id:$proxyid
	proxy.disable		id:$proxyid
	proxy.enable		id:$proxyid
	proxy.get		id:$proxyid			subid:$subproxyid
	proxy.info						userid:$userid
	proxy.setprio		id:$proxyid,priority:$priority
	proxy.stats		id:$proxyid			subid:$subproxyid
	subproxy.list		id:$proxyid
	user.get		user:$username
	user.getclients		user:$username
	worker.get		worker:$workername
	worker.getclients	worker:$workername


Examples of valid requests:
{"command":"stratifier.stats"}
{"command":"user.get","params":{"user":"a"}}
{"command":"user.getclients","params":{"user":"a"}}
{"command":"client.get","params":{"id":1}}

Examples of responses:
{"result": true, "error": null, "response": {"user": "a", "clients": [1, 5, 8], "errormsg": null}}


*/

struct api_command {
	const char *cmd;	/* API command we receive */
	int process;		/* Process to send request to */
	const char *proccmd;	/* Command to send to process */
	bool params;		/* Does this command take parameters */
} api_cmds[] = {
	{ "client.get",		PROC_STRATIFER, "getclient",	1},
	{ "clients.info",	PROC_STRATIFER, "clients",	0},
	{ "connector.stats",	PROC_CONNECTOR,	"stats",	0},
	{ "stratifier.stats",	PROC_STRATIFER,	"stats",	0},
	{ "generator.stats",	PROC_GENERATOR, "stats",	0},
	{ "proxy.list",		PROC_GENERATOR, "list",		0},
	{ "subproxy.list",	PROC_GENERATOR, "sublist",	1},
	{ "proxy.add",		PROC_GENERATOR, "addproxy",	1},
	{ "proxy.del",		PROC_GENERATOR, "delproxy",	1},
	{ "proxy.enable",	PROC_GENERATOR, "enableproxy",	1},
	{ "proxy.disable",	PROC_GENERATOR, "disableproxy",	1},
	{ "proxy.get",		PROC_STRATIFER, "getproxy",	1},
	{ "proxy.info",		PROC_STRATIFER, "proxyinfo",	0},
	{ "proxy.setprio",	PROC_STRATIFER,	"setproxy",	1},
	{ "proxy.stats",	PROC_GENERATOR, "proxystats",	1},
	{ "user.get",		PROC_STRATIFER, "getuser",	1},
	{ "user.getclients",	PROC_STRATIFER, "userclients",	1},
	{ "user.clientinfo",	PROC_STRATIFER, "ucinfo",	1},
	{ "users.info",		PROC_STRATIFER, "users",	0},
	{ "worker.clientinfo",	PROC_STRATIFER, "wcinfo",	1},
	{ "worker.get",		PROC_STRATIFER,	"getworker",	1},
	{ "worker.getclients",	PROC_STRATIFER, "workerclients",1},
	{ "workers.info",	PROC_STRATIFER, "workers",	0},
	{ "", -1, "" , 0}
};

/* Receive a command, find which process to send the command to, get its
 * response and return it on the original socket. */
void ckpool_api(ckpool_t __maybe_unused *ckp, apimsg_t *apimsg)
{
	char *cmd = NULL, *response = NULL, *procresponse = NULL, *command, *paramstr;
	json_t *val = NULL, *response_val = NULL, *params = NULL;
	struct api_command *ac = NULL;
	json_error_t err_val;
	int i = 0;

	if (unlikely(!apimsg->buf || !strlen(apimsg->buf))) {
		LOGWARNING("Received NULL buffer in ckpool_api");
		goto out;
	}
	LOGDEBUG("API received request %s", apimsg->buf);
	val = json_loads(apimsg->buf, 0, &err_val);
	if (unlikely(!val)) {
		LOGWARNING("Failed to JSON decode API message \"%s\" (%d):%s", apimsg->buf,
			   err_val.line, err_val.text);
		JSON_CPACK(response_val, "{s:b,s:[is],s:o}",
			   "result", false, "error", -1, "Invalid json", "response", json_null());
		goto out_send;
	}
	if (unlikely(!json_get_string(&cmd, val, "command"))) {
		LOGWARNING("Failed to find API command in message \"%s\"", apimsg->buf);
		JSON_CPACK(response_val, "{s:b,s:[is],s:o}",
			   "result", false, "error", -2, "No command", "response", json_null());
		goto out_send;
	}
	/* It's okay for there to be no parameters for many commands */
	params = json_object_get(val, "params");
	do {
		if (!safecmp(api_cmds[i].cmd, cmd))
			ac = &api_cmds[i];
		else
			i++;
	} while (!ac && api_cmds[i].process != -1);
	if (unlikely(!ac)) {
		LOGWARNING("Failed to find matching API command %s", cmd);
		JSON_CPACK(response_val, "{s:b,s:[is],s:o}",
			   "result", false, "error", -3, "Unknown command", "response", json_null());
		goto out_send;
	}
	if (ac->params) {
		if (unlikely(!params)) {
			LOGWARNING("Failed to find mandatory params in API command %s", apimsg->buf);
			JSON_CPACK(response_val, "{s:b,s:[is],s:o}",
				"result", false, "error", -4, "Missing params", "response", json_null());
			goto out_send;
		}
		paramstr = json_dumps(params, JSON_PRESERVE_ORDER);
		ASPRINTF(&command, "%s=%s", ac->proccmd, paramstr);
		free(paramstr);
	} else {
		if (params) {
			paramstr = json_dumps(params, JSON_PRESERVE_ORDER);
			ASPRINTF(&command, "%s=%s", ac->proccmd, paramstr);
			free(paramstr);
		} else
			command = strdup(ac->proccmd);
	}
	switch(ac->process) {
		case PROC_MAIN:
			procresponse = send_recv_proc(&ckp->main, command);
			break;
		case PROC_GENERATOR:
			procresponse = send_recv_proc(ckp->generator, command);
			break;
		case PROC_STRATIFER:
			procresponse = send_recv_proc(ckp->stratifier, command);
			break;
		case PROC_CONNECTOR:
			procresponse = send_recv_proc(ckp->connector, command);
			break;
	}
	free(command);
	if (unlikely(!procresponse)) {
		LOGWARNING("Failed to get API response from process %d to command %s msg %s",
			   ac->process, ac->proccmd, apimsg->buf);
		JSON_CPACK(response_val, "{s:b,s:[is],s:o}",
			   "result", false, "error", -5, "No process response", "response", json_null());
		goto out_send;
	}
	json_decref(val);
	val = json_loads(procresponse, 0, &err_val);
	if (unlikely(!val)) {
		LOGWARNING("Failed to JSON decode API response \"%s\" (%d):%s", procresponse,
			   err_val.line, err_val.text);
		JSON_CPACK(response_val, "{s:b,s:[is],s:o}",
			   "result", false, "error", -6, "Invalid json response", "response", json_null());
		goto out_send;
	}
	JSON_CPACK(response_val, "{s:b,s:o,s:o}",
		   "result", true, "error", json_null(), "response", val);
	val = NULL;
out_send:
	response = json_dumps(response_val, JSON_NO_UTF8 | JSON_PRESERVE_ORDER);
	if (unlikely(!send_unix_msg(apimsg->sockd, response))) {
		LOGWARNING("Failed to send API response: %s to sockd %d", response,
			   apimsg->sockd);
		goto out;
	}
	if (!wait_close(apimsg->sockd, 5))
		LOGWARNING("ckpool_api did not detect close from sockd %d", apimsg->sockd);
out:
	if (val)
		json_decref(val);
	free(procresponse);
	free(response);
	close(apimsg->sockd);
	free(apimsg->buf);
	free(apimsg);
}

/* Create a json errormsg string from the json_error_t created on failed json decode */
json_t *_json_encode_errormsg(json_error_t *err_val, const char *func)
{
	json_t *ret;
	char *buf;

	ASPRINTF(&buf, "Failed to JSON decode in %s (%d):%s",  func, err_val->line, err_val->text);
	JSON_CPACK(ret, "{ss}", "errormsg", buf);
	free(buf);
	return ret;
}

/* Create a json errormsg string from varargs */
json_t *json_errormsg(const char *fmt, ...)
{
	char *buf = NULL;
	json_t *ret;
	va_list ap;

	va_start(ap, fmt);
	VASPRINTF(&buf, fmt, ap);
	va_end(ap);
	JSON_CPACK(ret, "{ss}", "errormsg", buf);
	free(buf);
	return ret;
}

/* Return an API response from a json structure to sockd */
void _send_api_response(json_t *val, const int sockd, const char *file, const char *func, const int line)
{
	char *response;

	if (unlikely(!val))
		val = json_errormsg("Failed to get json to send from %s %s:%d", file, func, line);
	else if (!json_object_get(val, "errormsg"))
		json_object_set(val, "errormsg", json_null());
	response = json_dumps(val, JSON_NO_UTF8 | JSON_PRESERVE_ORDER);
	if (unlikely(!response)) {
		LOGWARNING("Failed to get json to send from %s %s:%d", file, func, line);
		send_unix_msg(sockd, "");
		return;
	}
	json_decref(val);
	send_unix_msg(sockd, response);
	free(response);
}
