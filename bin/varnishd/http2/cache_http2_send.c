/*-
 * Copyright (c) 2016 Varnish Software AS
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
 */

#include "config.h"

#include <sys/types.h>
#include <sys/socket.h>

#include <netinet/in.h>

#include <ctype.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cache/cache.h"
#include "cache/cache_filter.h"
#include "cache/cache_transport.h"
#include "http2/cache_http2.h"

#include "vend.h"
#include "vsb.h"

static void
h2_mk_hdr(uint8_t *hdr, enum h2_frame_e type, uint8_t flags,
    uint32_t len, uint32_t stream)
{

	AN(hdr);
	assert(len < (1U << 24));
	vbe32enc(hdr, len << 8);
	hdr[3] = (uint8_t)type;
	hdr[4] = flags;
	vbe32enc(hdr + 5, stream);
}

/*
 * This is the "raw" frame sender, all per stream accounting and
 * prioritization must have happened before this is called, and
 * the session mtx must be held.
 */

int
H2_Send_Frame(struct worker *wrk, const struct h2_sess *h2,
    enum h2_frame_e type, uint8_t flags,
    uint32_t len, uint32_t stream, const void *ptr)
{
	uint8_t hdr[9];

	Lck_AssertHeld(&h2->sess->mtx);
	(void)wrk;

	h2_mk_hdr(hdr, type, flags, len, stream);
	VSLb_bin(h2->vsl, SLT_H2TxHdr, 9, hdr);

	/*XXX*/(void)write(h2->sess->fd, hdr, sizeof hdr);
	if (len > 0) {
		/*XXX*/(void)write(h2->sess->fd, ptr, len);
		VSLb_bin(h2->vsl, SLT_H2TxBody, len, ptr);
	}
	return (0);
}

/*
 * This is the per-stream frame sender.
 * XXX: windows
 * XXX: priority
 */

int
H2_Send(struct worker *wrk, struct h2_req *r2, int flush,
    enum h2_frame_e type, uint8_t flags, uint32_t len, const void *ptr)
{
	int retval;
	struct h2_sess *h2;
	uint32_t mfs, tf;
	const char *p;

	(void)flush;

	CHECK_OBJ_NOTNULL(r2, H2_REQ_MAGIC);
	h2 = r2->h2sess;
	CHECK_OBJ_NOTNULL(h2, H2_SESS_MAGIC);
	assert(len == 0 || ptr != NULL);

	Lck_Lock(&h2->sess->mtx);
	mfs = h2->their_settings[H2S_MAX_FRAME_SIZE];
	if (len < mfs) {
		retval = H2_Send_Frame(wrk, h2,
		    type, flags, len, r2->stream, ptr);
	} else if (type == H2_FRAME_DATA) {
		AN(ptr);
		AN(len);
		p = ptr;
		do {
			tf = mfs;
			if (tf > len)
				tf = len;
			retval = H2_Send_Frame(wrk, h2, type,
			    tf == len ? flags : 0,
			    tf, r2->stream, p);
			p += tf;
			len -= tf;
		} while (len > 0);
	} else {
		INCOMPL();
	}
	Lck_Unlock(&h2->sess->mtx);
	return (retval);
}
