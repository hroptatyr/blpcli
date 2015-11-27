/*** blp-um.c -- simple command-line interface to blpapi
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
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <signal.h>
#include <setjmp.h>
#include <errno.h>
#include <sys/time.h>
#include <time.h>
#include <pthread.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <blpapi_correlationid.h>
#include <blpapi_element.h>
#include <blpapi_event.h>
#include <blpapi_message.h>
#include <blpapi_request.h>
#include <blpapi_session.h>
#include <blpapi_subscriptionlist.h>
#include "nifty.h"

#include "blp-um.yucc"

struct ctx_s {
	enum {
		ST_UNK,
		ST_SES,
		ST_SVC,

		/* beef states */
		ST_REQ,
		ST_SUB,

		ST_FIN,
	} st;
	const yuck_t *argi;
	int rc;
};

#define LOG(x)		fputs(x, stderr)
#define LOGF(fmt, ...)	fprintf(stderr, fmt, __VA_ARGS__)


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

static void
block_sigs(void)
{
	sigset_t fatal_signal_set[1];

	sigemptyset(fatal_signal_set);
	sigaddset(fatal_signal_set, SIGHUP);
	sigaddset(fatal_signal_set, SIGQUIT);
	sigaddset(fatal_signal_set, SIGINT);
	sigaddset(fatal_signal_set, SIGTERM);
	sigaddset(fatal_signal_set, SIGXCPU);
	sigaddset(fatal_signal_set, SIGXFSZ);
	(void)pthread_sigmask(SIG_BLOCK, fatal_signal_set, (sigset_t*)NULL);
	return;
}

static void
unblock_sigs(void)
{
	sigset_t empty_signal_set[1];

	sigemptyset(empty_signal_set);
	(void)pthread_sigmask(SIG_SETMASK, empty_signal_set, (sigset_t*)NULL);
	return;
}

static size_t
dt_strf_d(char *restrict buf, size_t bsz, int days_since_epoch)
{
	const unsigned int bas = 19359U/*#days between 1917-01-00 and epoch*/;
	unsigned int epo = days_since_epoch + bas;
	unsigned int y, m, d;
	unsigned int tmp;

	/* start with a guess */
	y = epo / 365U;
	tmp = y * 365U + y / 4U;
	if (UNLIKELY(tmp >= epo)) {
		y--;
		tmp = y * 365U + y / 4U;
	}
	/* repurpose tmp to hold the doy */
	tmp = epo - tmp;
	/* now the trimester algo to go from doy to m+d */
	if ((tmp -= 1U + !(y % 4U)/*__leapp(y)*/) < 59U) {
		/* 3rd trimester */
		tmp += !(y % 4U);
	} else if ((tmp += 2U) < 153U + 61U) {
		/* 1st trimester */
		;
	} else {
		/* 2nd trimester */
		tmp += 30U;
	}

	m = 2 * tmp / 61U;
	d = 2 * tmp % 61U;

	m = m + 1 - (m >= 7U);
	d = d / 2 + 1U;
	y += 1917U;
	return snprintf(buf, bsz, "%04u-%02u-%02u", y, m, d);
}

static size_t
dt_strf_t(char *restrict buf, size_t bsz, unsigned int tim, unsigned int nsec)
{
	unsigned int M, S;

	S = tim % 60U;
	tim /= 60U;
	M = tim % 60U;
	tim /= 60U;
	return snprintf(buf, bsz, "%02u:%02u:%02u.%09uZ", tim, M, S, nsec);
}


static const char *flds[] = {
	"BID", "ASK",
};

static size_t
cpyel(char *restrict buf, size_t bsz, const blpapi_Element_t *e)
{
	int rc = 0;

	switch (blpapi_Element_datatype(e)) {
		union {
			blpapi_Int32_t i32;
			blpapi_Int64_t i64;
			blpapi_Float32_t f32;
			blpapi_Float64_t f64;
			blpapi_Datetime_t dt;
			blpapi_HighPrecisionDatetime_t hp;
		} tmp;

	case BLPAPI_DATATYPE_INT32:
		rc = blpapi_Element_getValueAsInt32(e, &tmp.i32, 0U);
		rc = snprintf(buf, bsz, "%i", tmp.i32);
		break;
	case BLPAPI_DATATYPE_INT64:
		rc = blpapi_Element_getValueAsInt64(e, &tmp.i64, 0U);
		rc = snprintf(buf, bsz, "%lli", tmp.i64);
		break;
	case BLPAPI_DATATYPE_FLOAT32:
		rc = blpapi_Element_getValueAsFloat32(e, &tmp.f32, 0U);
		rc = snprintf(buf, bsz, "%f", tmp.f32);
		break;
	case BLPAPI_DATATYPE_FLOAT64:
		rc = blpapi_Element_getValueAsFloat64(e, &tmp.f64, 0U);
		rc = snprintf(buf, bsz, "%f", tmp.f64);
		break;
	case BLPAPI_DATATYPE_DATETIME:
	case BLPAPI_DATATYPE_DATE:
	case BLPAPI_DATATYPE_TIME:
		break;
	default:
		break;
	}
	return rc;
}

static void
dump_pub(const yuck_t argi[static 1U], blpapi_Message_t *msg)
{
	char buf[1280U];
	char *const *tops = argi->args;
	blpapi_Element_t *els;
	blpapi_Element_t *el;
	blpapi_CorrelationId_t cid;
	size_t ix;
	size_t len;

	cid = blpapi_Message_correlationId(msg, 0);
	if (UNLIKELY(cid.valueType != BLPAPI_CORRELATION_TYPE_INT)) {
		goto nop;
	}
	/* otherwise CID holds the index into TOPS */
	if (UNLIKELY((ix = cid.value.intValue) <= 0)) {
		goto nop;
	}

	ix--;
	if (UNLIKELY((els = blpapi_Message_elements(msg)) == NULL)) {
		goto nop;
	}
	memcpy(buf, tops[ix], (len = strlen(tops[ix])));
	buf[len++] = '\t';

	if (!blpapi_Element_getElement(els, &el, flds[0U], NULL)) {
		len += cpyel(buf + len, sizeof(buf) - len, el);
	}
	if (!blpapi_Element_getElement(els, &el, flds[1U], NULL)) {
		len += cpyel(buf + len, sizeof(buf) - len, el);
	}
	/* and finalise */
	buf[len++] = '\n';

nop:
	return;
}

static void
dump_evs(const struct ctx_s ctx[static 1U], blpapi_MessageIterator_t *iter)
{
	static int today;
	static char stmp[32U];
	blpapi_Message_t *msg;
	const yuck_t *argi = ctx->argi;

	with (struct timespec tsp) {
		int tspd;
		unsigned int tspt;

		clock_gettime(CLOCK_REALTIME, &tsp);
		tspd = tsp.tv_sec / 86400;
		tspt = tsp.tv_sec % 86400;
		if (UNLIKELY(today < tspd)) {
			/* oh no, we need to work a bit */
			today = tspd;
			dt_strf_d(stmp, sizeof(stmp), tspd);
			stmp[10U] = 'T';
		}
		/* always fill in time-of-day and nanos */
		dt_strf_t(stmp + 11U, sizeof(stmp) - 11U, tspt, tsp.tv_nsec);
	}

	while (!blpapi_MessageIterator_next(iter, &msg)) {
		fputs(stmp, stdout);
		fputc('\t', stdout);
		dump_pub(argi, msg);
	}
	return;
}


static int
svc_sta_sub(blpapi_Session_t *s, const struct yuck_s argi[static 1U])
{
	blpapi_SubscriptionList_t *subs;
	const char *opts[] = {};

	if (UNLIKELY((subs = blpapi_SubscriptionList_create()) == NULL)) {
		errno = 0, error("\
Error: cannot instantiate subscriptions");
		return -1;
	}

	/* subscribe */
	for (size_t i = 0U; i < argi->nargs; i++) {
		const char *top = argi->args[i];
		blpapi_CorrelationId_t cid = {
			.size = sizeof(cid),
			.valueType = BLPAPI_CORRELATION_TYPE_INT,
			.value.intValue = i + 1U,
		};

		blpapi_SubscriptionList_add(
			subs, top, &cid, flds, opts, countof(flds), countof(opts));
	}
	if (blpapi_Session_subscribe(s, subs, NULL, NULL, 0)) {
		errno = 0, error("\
Error: cannot subscribe");
	}
	blpapi_SubscriptionList_destroy(subs);
	return 0;
}


static int
sess_sta(blpapi_Session_t *sess, struct ctx_s *ctx)
{
	static const char svc_sub[] = "//blp/mktdata";
	const char *svc = svc_sub;

	switch (ctx->st) {
	case ST_UNK:
	case ST_FIN:
		break;

	case ST_SES:
	case ST_SVC:
	default:
		/* we're already in a session, just bog off */
		errno = 0, error("\
Warning: session message received but we are in a session already");
		return -1;
	}

	/* just open the service */
	blpapi_CorrelationId_t cid = {
		.size = sizeof(cid),
		.valueType = BLPAPI_CORRELATION_TYPE_INT,
		.value.intValue = 0,
	};
	if (UNLIKELY(blpapi_Session_openServiceAsync(sess, svc, &cid))) {
		errno = 0, error("\
Error: cannot open service %s", svc);
		ctx->rc = 1;
		return -1;
	}
	/* success, advance state */
	LOG("ST<-SES\n");
	ctx->st = ST_SES;
	return 0;
}

static int
sess_end(blpapi_Session_t *UNUSED(sess), struct ctx_s *ctx)
{
	/* indicate success */
	LOG("ST<-FIN\n");
	ctx->st = ST_FIN;
	return 0;
}

static int
svc_sta(blpapi_Session_t *sess, struct ctx_s *ctx)
{
	const yuck_t *argi;

	switch (ctx->st) {
	case ST_SES:
		argi = ctx->argi;
		break;
	case ST_UNK:
	case ST_SVC:
	case ST_FIN:
	default:
		/* do fuck all */
		errno = 0, error("\
Warning: service message received but we are past the session state");
		return -1;
	}

	if (svc_sta_sub(sess, (const struct yuck_s*)argi) < 0) {
		return -1;
	}
	/* success */
	LOG("ST<-SVC\n");
	ctx->st = ST_SVC;
	return 0;
}

static int
sub_sta(blpapi_Session_t *UNUSED(sess), struct ctx_s *ctx)
{
	switch (ctx->st) {
	case ST_SVC:
		break;
	case ST_UNK:
	case ST_SES:
	case ST_FIN:
	default:
		/* do fuck all */
		return -1;
	}

	/* indicate success */
	LOG("ST<-SUB\n");
	ctx->st = ST_SUB;
	return 0;
}

static void
beef(blpapi_Event_t *e, blpapi_Session_t *sess, void *ctx)
{
	blpapi_MessageIterator_t *iter;
	unsigned int typ;

	if (UNLIKELY((iter = blpapi_MessageIterator_create(e)) == NULL)) {
		LOG("NOMSG");
		return;
	}
	switch ((typ = blpapi_Event_eventType(e))) {
	case BLPAPI_EVENTTYPE_SESSION_STATUS:
		for (blpapi_Message_t *msg;
		     (!blpapi_MessageIterator_next(iter, &msg));) {
			static const char sta[] = "SessionStarted";
			static const char end[] = "SessionTerminated";
			const char *msgstr = blpapi_Message_typeString(msg);

			if (!strcmp(msgstr, sta)) {
				/* yay!!! */
				sess_sta(sess, ctx);
			} else if (!strcmp(msgstr, end)) {
				/* nawww :( */
				sess_end(sess, ctx);
			} else {
				/* just rubbish information I presume */
				;
			}
		}
		break;
	case BLPAPI_EVENTTYPE_SERVICE_STATUS:
		for (blpapi_Message_t *msg;
		     (!blpapi_MessageIterator_next(iter, &msg));) {
			static const char opn[] = "ServiceOpened";
			const char *msgstr = blpapi_Message_typeString(msg);

			if (!strcmp(msgstr, opn)) {
				/* yay!!! */
				svc_sta(sess, ctx);
			}
		}
		break;
	case BLPAPI_EVENTTYPE_SUBSCRIPTION_STATUS:
		for (blpapi_Message_t *msg;
		     (!blpapi_MessageIterator_next(iter, &msg));) {
			static const char sta[] = "SubscriptionStarted";
			const char *msgstr = blpapi_Message_typeString(msg);

			if (!strcmp(msgstr, sta)) {
				/* yay */
				sub_sta(sess, ctx);
			}
		}
		break;
	case BLPAPI_EVENTTYPE_PARTIAL_RESPONSE:
	case BLPAPI_EVENTTYPE_RESPONSE:
	case BLPAPI_EVENTTYPE_SUBSCRIPTION_DATA:
		dump_evs(ctx, iter);

		if (UNLIKELY(typ == BLPAPI_EVENTTYPE_RESPONSE)) {
			/* that was the final response, innit?
			 * pretend we pressed C-c */
			kill(getpid(), SIGINT);
		}
		break;
	default:
		/* uh oh */
		errno = 0, error("\
Warning: unknown event %u", typ);
		break;
	}
	blpapi_MessageIterator_destroy(iter);
	blpapi_Event_release(e);
	return;
}

int
main(int argc, char *argv[])
{
	static yuck_t argi[1U];
	static struct ctx_s ctx = {.argi = argi};
	blpapi_Session_t *sess;
	int rc = 0;

	/* parse options, set up longjmp target and
	 * install a C-c handler */
	if (yuck_parse(argi, argc, argv) < 0) {
		errno = 0, error("\
Fatal: cannot parse options");
		rc = 1;
		goto out;
	}

	/* we can't do with interruptions */
	block_sigs();

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

		sess = blpapi_Session_create(opt, beef, NULL, &ctx);
		blpapi_SessionOptions_destroy(opt);
	}

	/* check session handle before we continue with the setup*/
	if (UNLIKELY(sess == NULL)) {
		errno = 0, error("\
Error: cannot set up session");
		rc = 1;
		goto out;
	} else if (blpapi_Session_start(sess)) {
		errno = 0, error("\
Error: cannot start session");
		rc = 1;
		goto out;
	}

	/* sleep and let the bloomberg thread do the hard work */
	with (sigset_t sigs[1U]) {
		sigfillset(sigs);
		for (int sig; !sigwait(sigs, &sig);) {
			switch (sig) {
			case SIGQUIT:
			case SIGINT:
			case SIGPIPE:
				LOG("GOT INT\n");
				goto out;

			default:
				LOGF("GOT SIG %d\n", sig);
				break;
			}
		}
	}

out:
	unblock_sigs();
	if (sess != NULL) {
		blpapi_Session_stop(sess);
		blpapi_Session_destroy(sess);
		sess = NULL;
	}
	yuck_free(argi);
	return rc;
}

/* blpcli.c ends here */
