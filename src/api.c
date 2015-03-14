/*
 * Copyright 2014-2015 Con Kolivas
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 */

#include "config.h"

#include <stdio.h>
#include <unistd.h>

#include "api.h"

#include "libckpool.h"
#include "generator.h"
#include "stratifier.h"
#include "connector.h"

void ckpool_api(ckpool_t __maybe_unused *ckp, apimsg_t *apimsg)
{
	if (unlikely(!apimsg->buf || !strlen(apimsg->buf))) {
		LOGWARNING("Received NULL buffer in ckpool_api");
		goto out;
	}
	send_unix_msg(apimsg->sockd, "API message received!");
out:
	close(apimsg->sockd);
	free(apimsg->buf);
	free(apimsg);
}
