/*
 * Copyright 2014-2015 Con Kolivas
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 */

#ifndef API_H
#define API_H

#include "config.h"

#include "ckpool.h"

typedef struct apimsg apimsg_t;

struct apimsg {
	char *buf;
	int sockd;
};

void ckpool_api(ckpool_t __maybe_unused *ckp, apimsg_t *apimsg);
void _send_api_response(json_t *val, const int sockd, const char *file, const char *func, const int line);
#define send_api_response(_val, _sockd) _send_api_response(_val, _sockd,  __FILE__, __func__, __LINE__)
json_t *_json_encode_errormsg(json_error_t *err_val, const char *func);
#define json_encode_errormsg(err_val) _json_encode_errormsg(err_val, __func__)
json_t *json_errormsg(const char *fmt, ...);

#endif /* API_H */
