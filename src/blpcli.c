/*** blpcli.c -- simple command-line interface to blpapi
 *
 * Copyright (C) 2013-2015 Sebastian Freundt
 *
 * Author:  Sebastian Freundt <freundt@ga-group.nl>
 *
 * This file is part of blpcli.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the author nor the names of any contributors
 *    may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR "AS IS" AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 ***/
#if defined HAVE_CONFIG_H
# include "config.h"
#endif	/* HAVE_CONFIG_H */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <signal.h>
#include <setjmp.h>
#include <errno.h>
#include <blpapi_correlationid.h>
#include <blpapi_element.h>
#include <blpapi_event.h>
#include <blpapi_message.h>
#include <blpapi_request.h>
#include <blpapi_session.h>
#include <blpapi_subscriptionlist.h>
#include "nifty.h"

static blpapi_Session_t *gsess;
static jmp_buf gjmp;


static __attribute__((format(printf, 1, 2))) void
error(const char *fmt, ...)
{
	va_list vap;
	va_start(vap, fmt);
	vfprintf(stderr, fmt, vap);
	va_end(vap);
	if (errno) {
		fputc(':', stderr);
		fputc(' ', stderr);
		fputs(strerror(errno), stderr);
	}
	fputc('\n', stderr);
	return;
}


static int
wrcb(const char *data, int len, void *f)
{
	return fwrite(data, 1, len, f);
}

static int
proc_beef(const blpapi_Event_t *e)
{
	blpapi_MessageIterator_t *iter;
	blpapi_Message_t *msg;
	int rc = 0;

	iter = blpapi_MessageIterator_create(e);
	while (!blpapi_MessageIterator_next(iter, &msg)) {
		static const char msg_term[] = "SessionTerminated";
		const unsigned int typ = blpapi_Event_eventType(e);
		blpapi_Element_t *els;

		els = blpapi_Message_elements(msg);
		blpapi_Element_print(els, wrcb, stdout, 0, 4);

		if (typ == BLPAPI_EVENTTYPE_SESSION_STATUS &&
		    !strcmp(blpapi_Message_typeString(msg), msg_term)) {
			/* yep, it's really terminated, better believe it */
			rc = -1;
			break;
		}
	}
	blpapi_MessageIterator_destroy(iter);
	return rc;
}


#include "blpcli.yucc"

static int
cmd_get(const struct yuck_cmd_get_s argi[static 1U])
{
	static const char svc_ref[] = "//blp/refdata";
	blpapi_Service_t *svc;
	blpapi_Request_t *req;

	if (UNLIKELY(blpapi_Session_openService(gsess, svc_ref))) {
		errno = 0, error("\
Error: cannot open service %s", svc_ref);
		return 1;
	}

	blpapi_Session_getService(gsess, &svc, svc_ref);
	blpapi_Service_createRequest(svc, &req, "ReferenceDataRequest");

	with (blpapi_Element_t *e = blpapi_Request_elements(req), *sub) {
		blpapi_Element_getElement(e, &sub, "securities", 0);
		blpapi_Element_setValueString(
			sub, "CBK GY Equity", BLPAPI_ELEMENT_INDEX_END);
		blpapi_Element_getElement(e, &sub, "fields", 0);
		blpapi_Element_setValueString(
			sub, "LEGAL_ENTITY_IDENTIFIER", BLPAPI_ELEMENT_INDEX_END);
	}

	blpapi_CorrelationId_t cid = {
		.size = sizeof(cid),
		.valueType = BLPAPI_CORRELATION_TYPE_INT,
		.value.intValue = 1,
	};

	/* finally send the whole shebang */
	blpapi_Session_sendRequest(gsess, req, &cid, 0, 0, 0, 0);
	blpapi_Request_destroy(req);
#if 0
	for (blpapi_Event_t *e;
	     (blpapi_Session_nextEvent(sess, &e, 0), proc_evt(e));
	     blpapi_Event_release(e));
#endif
	return 0;
}

static int
cmd_sub(const struct yuck_cmd_sub_s argi[static 1U])
{
	static const char svc_mkt[] = "//blp/mktdata";
	blpapi_SubscriptionList_t *subs;
	blpapi_CorrelationId_t cid = {
		.size = sizeof(cid),
		.valueType = BLPAPI_CORRELATION_TYPE_INT,
		.value.intValue = 2,
	};
	const char *flds[] = {
		"NEWS_SENTIMENT_RT",
		"TIME_OF_LAST_NEWS_STORY",
		"WIRE_OF_LAST_NEWS_STORY",
	};
	const char *opts[] = {};

	if (UNLIKELY(blpapi_Session_openService(gsess, svc_mkt))) {
		errno = 0, error("\
Error: cannot open service %s", svc_mkt);
		return 1;
	} else if (UNLIKELY((subs = blpapi_SubscriptionList_create()) == NULL)) {
		errno = 0, error("\
Error: cannot instantiate subscriptions");
		return 1;
	}

	/* subscribe */
	blpapi_SubscriptionList_add(
		subs, "CBK GY Equity",
		&cid, flds, opts, countof(flds), countof(opts));
	blpapi_Session_subscribe(gsess, subs, 0, 0, 0);

	/* main loop */
	for (blpapi_Event_t *ev = NULL; 
	     (blpapi_Session_nextEvent(gsess, &ev, 0), ev);
	     blpapi_Event_release(ev), ev = NULL) {
		if (proc_beef(ev) < 0) {
			break;
		}
	}
	return 0;
}

static void
int_handler(int sig, siginfo_t *UNUSED(nfo), void *UNUSED(ctx))
{
	longjmp(gjmp, sig);
	return;
}

int
main(int argc, char *argv[])
{
	static yuck_t argi[1U];
	static struct sigaction hdl_int = {
		.sa_flags = SA_SIGINFO,
		.sa_sigaction = int_handler,
	};
	static struct sigaction old_int;
	int rc = 0;

	/* parse options, set up longjmp target and
	 * install a C-c handler */
	if (yuck_parse(argi, argc, argv) < 0) {
		errno = 0, error("\
Fatal: cannot parse options");
		rc = 1;
		goto out;
	} else if (setjmp(gjmp)) {
		/* just go past the main loop */
		goto out;
	} else if (sigaction(SIGINT, &hdl_int, &old_int) < 0) {
		error("\
Fatal: cannot install handler for SIGINT");
		rc = 1;
		goto out;
	}

	/* barf early if there's no command */
	if (UNLIKELY(!argi->cmd)) {
		errno = 0, error("\
Error: no command given.  See --help.");
		rc = 1;
		goto out;
	}

	/* get ourselves a session handle */
	with (blpapi_SessionOptions_t *opt) {
		/* suppress blpapi messages */
		setbuf(stdout, NULL);

		if (UNLIKELY((opt = blpapi_SessionOptions_create()) == NULL)) {
			error("\
Error: cannot create session options");
			rc = 1;
			goto out;
		}
		/* otherwise set them according to our command-line flags */
		blpapi_SessionOptions_setServerHost(opt, "localhost");
		blpapi_SessionOptions_setServerPort(opt, 8194);
		blpapi_SessionOptions_setMaxEventQueueSize(opt, 8192);

		gsess = blpapi_Session_create(opt, 0, 0, 0);
		blpapi_SessionOptions_destroy(opt);
	}

	/* check session handle before we continue with the setup*/
	if (UNLIKELY(gsess == NULL)) {
		errno = 0, error("\
Error: cannot set up session");
		rc = 1;
		goto out;
	} else if (blpapi_Session_start(gsess)) {
		errno = 0, error("\
Error: cannot start session");
		rc = 1;
		goto out;
	}

	switch (argi->cmd) {
	case BLPCLI_CMD_GET:
		rc = cmd_get((const struct yuck_cmd_get_s*)argi);
		break;
	case BLPCLI_CMD_SUB:
		rc = cmd_sub((const struct yuck_cmd_sub_s*)argi);
		break;

	case BLPCLI_CMD_NONE:
	default:
		/* huh? */
		rc = 127;
		break;
	}

out:
	/* establish old C-c handler */
	(void)sigaction(SIGINT, &old_int, NULL);

	if (gsess != NULL) {
		blpapi_Session_stop(gsess);
		blpapi_Session_destroy(gsess);
		gsess = NULL;
	}
	yuck_free(argi);
	return rc;
}

/* blpcli.c ends here */
