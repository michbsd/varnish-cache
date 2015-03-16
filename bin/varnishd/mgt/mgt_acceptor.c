/*-
 * Copyright (c) 2006 Verdens Gang AS
 * Copyright (c) 2006-2014 Varnish Software AS
 * All rights reserved.
 *
 * Author: Poul-Henning Kamp <phk@phk.freebsd.dk>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Acceptor socket management
 */

#include "config.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mgt/mgt.h"
#include "common/heritage.h"

#include "vav.h"
#include "vsa.h"
#include "vss.h"
#include "vtcp.h"

/*=====================================================================
 * Open and close the accept sockets.
 *
 * (The child is priv-sep'ed, so it can't do it.)
 */

int
MAC_open_sockets(void)
{
	struct listen_sock *ls;
	int fail;

	VJ_master(JAIL_MASTER_PRIVPORT);
	VTAILQ_FOREACH(ls, &heritage.socks, list) {
		assert(ls->sock < 0);
		ls->sock = VTCP_bind(ls->addr, NULL);
		if (ls->sock < 0)
			break;
		mgt_child_inherit(ls->sock, "sock");
	}
	fail = errno;
	VJ_master(JAIL_MASTER_LOW);
	if (ls == NULL)
		return (0);
	MAC_close_sockets();
	errno = fail;
	return (-1);
}

/*--------------------------------------------------------------------*/

void
MAC_close_sockets(void)
{
	struct listen_sock *ls;

	VTAILQ_FOREACH(ls, &heritage.socks, list) {
		if (ls->sock < 0)
			continue;
		mgt_child_inherit(ls->sock, NULL);
		AZ(close(ls->sock));
		ls->sock = -1;
	}
}

/*--------------------------------------------------------------------*/

struct mac_help {
	unsigned		magic;
#define MAC_HELP_MAGIC		0x1e00a9d9
	const char		*name;
	int			good;
	const char		**err;
};

static int __match_proto__(vss_resolver_f)
mac_callback(void *priv, const struct suckaddr *sa)
{
	struct mac_help *mh;
	struct listen_sock *ls;
	int sock;

	CAST_OBJ_NOTNULL(mh, priv, MAC_HELP_MAGIC);
	sock = VTCP_bind(sa, NULL);
	if (sock < 0) {
		*(mh->err) = strerror(errno);
		return (0);
	}

	ALLOC_OBJ(ls, LISTEN_SOCK_MAGIC);
	AN(ls);
	if (VSA_Port(sa) == 0)
		ls->addr = VTCP_my_suckaddr(sock);
	else
		ls->addr = VSA_Clone(sa);
	AN(ls->addr);
	AZ(close(sock));
	ls->sock = -1;
	ls->name = strdup(priv);
	AN(ls->name);
	VTAILQ_INSERT_TAIL(&heritage.socks, ls, list);
	mh->good++;
	return (0);
}

void
MAC_Arg(const char *arg)
{
	char **av;
	struct mac_help *mh;
	const char *err;
	int error;

	av = VAV_Parse(arg, NULL, ARGV_COMMA);
	if (av == NULL)
		ARGV_ERR("Parse error: out of memory\n");
	if (av[0] != NULL)
		ARGV_ERR("%s\n", av[0]);
	if (av[2] != NULL)
		ARGV_ERR("XXX: not yet\n");

	ALLOC_OBJ(mh, MAC_HELP_MAGIC);
	AN(mh);
	mh->name = av[1];
	mh->err = &err;
	error = VSS_resolver(av[1], "80", mac_callback, mh, &err);
	if (mh->good == 0 || err != NULL)
		ARGV_ERR("Could not bind to address %s: %s\n", av[1], err);
	AZ(error);
	FREE_OBJ(mh);
}
