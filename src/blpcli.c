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
#include <blpapi_correlationid.h>
#include <blpapi_element.h>
#include <blpapi_event.h>
#include <blpapi_message.h>
#include <blpapi_request.h>
#include <blpapi_session.h>
#include <blpapi_subscriptionlist.h>
#include "nifty.h"

#include "blpcli.yucc"

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


static int
dump_Element(const blpapi_Element_t *e, FILE *whither)
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
		fprintf(whither, "%i", tmp.i32);
		break;
	case BLPAPI_DATATYPE_INT64:
		rc = blpapi_Element_getValueAsInt64(e, &tmp.i64, 0U);
		fprintf(whither, "%lli", tmp.i64);
		break;
	case BLPAPI_DATATYPE_FLOAT32:
		rc = blpapi_Element_getValueAsFloat32(e, &tmp.f32, 0U);
		fprintf(whither, "%f", tmp.f32);
		break;
	case BLPAPI_DATATYPE_FLOAT64:
		rc = blpapi_Element_getValueAsFloat64(e, &tmp.f64, 0U);
		fprintf(whither, "%f", tmp.f64);
		break;
	case BLPAPI_DATATYPE_DATETIME:
	case BLPAPI_DATATYPE_DATE:
	case BLPAPI_DATATYPE_TIME:
		rc = blpapi_Element_getValueAsHighPrecisionDatetime(
			e, &tmp.hp, 0U);
		if (rc) {
			break;
		}
		if (tmp.hp.datetime.parts & BLPAPI_DATETIME_YEAR_PART) {
			fprintf(whither, "%04hu-%02hhu-%02hhu",
				tmp.hp.datetime.year,
				tmp.hp.datetime.month,
				tmp.hp.datetime.day);
			if (tmp.hp.datetime.parts & BLPAPI_DATETIME_TIME_PART) {
				fputc('T', whither);
			}
		}
		if (tmp.hp.datetime.parts & BLPAPI_DATETIME_SECONDS_PART) {
			fprintf(whither, "%02hhu:%02hhu:%02hhu",
				tmp.hp.datetime.hours,
				tmp.hp.datetime.minutes,
				tmp.hp.datetime.seconds);
			if (tmp.hp.datetime.parts &
			    BLPAPI_DATETIME_FRACSECONDS_PART) {
				fputc('.', whither);
			}
		}
		if (tmp.hp.datetime.parts & BLPAPI_DATETIME_FRACSECONDS_PART) {
			fprintf(whither, "%03hu%09u",
				tmp.hp.datetime.milliSeconds,
				tmp.hp.picoseconds);
		}
		break;
	case BLPAPI_DATATYPE_STRING:
		with (const char *str[1U]) {
			rc = blpapi_Element_getValueAsString(e, str, 0U);
			if (rc) {
				break;
			}
			fputs(*str, whither);
		}
		break;
	default:
		rc = -1;
		break;
	}
	return rc;
}

static void
dump_rsp(const yuck_t argi[static 1U], blpapi_Message_t *UNUSED(msg))
{
	char *const *UNUSED(tops) = argi->topic_args;
	char *const *UNUSED(flds) = argi->field_args;

	fputc('\n', stdout);
	return;
}

static void
dump_pub(const yuck_t argi[static 1U], blpapi_Message_t *msg)
{
	char *const *tops = argi->topic_args;
	char *const *flds = argi->field_args;
	const size_t nflds = argi->field_nargs;
	blpapi_Element_t *els;
	blpapi_CorrelationId_t cid;
	size_t ix;

	cid = blpapi_Message_correlationId(msg, 0);
	if (UNLIKELY(cid.valueType != BLPAPI_CORRELATION_TYPE_INT)) {
		goto nop;
	}
	/* otherwise CID holds the index into TOPS */
	if (UNLIKELY((ix = cid.value.intValue) <= 0)) {
		goto nop;
	}

	ix--;
	fputs(tops[ix], stdout);

	if (UNLIKELY((els = blpapi_Message_elements(msg)) == NULL)) {
		goto nop;
	}

	for (size_t i = 0U; i < nflds; i++) {
		blpapi_Element_t *f;

		fputc('\t', stdout);
		if (!blpapi_Element_getElement(els, &f, flds[i], NULL)) {
			dump_Element(f, stdout);
		}
	}
nop:
	fputc('\n', stdout);
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
		switch (argi->cmd) {
		case BLPCLI_CMD_GET:
			dump_rsp(argi, msg);
			break;
		case BLPCLI_CMD_SUB:
			dump_pub(argi, msg);
			break;
		default:
			fputc('\n', stdout);
			break;
		}
	}
	return;
}


static int
svc_sta_get(blpapi_Session_t *s, const struct yuck_cmd_get_s argi[static 1U])
{
	static const char svc_ref[] = "//blp/refdata";
	blpapi_Service_t *svc;
	blpapi_Request_t *req;
	blpapi_Element_t *els;
	blpapi_CorrelationId_t cid = {
		.size = sizeof(cid),
		.valueType = BLPAPI_CORRELATION_TYPE_INT,
		.value.intValue = 1,
	};
	int rc = 0;

	if (UNLIKELY(blpapi_Session_openService(s, svc_ref))) {
		errno = 0, error("\
Error: cannot open service %s", svc_ref);
		return -1;
	}

	blpapi_Session_getService(s, &svc, svc_ref);
	blpapi_Service_createRequest(svc, &req, "ReferenceDataRequest");

	if (UNLIKELY((els = blpapi_Request_elements(req)) == NULL)) {
		errno = 0, error("\
Error: cannot acquire request elements");
		rc = -1;
		goto out;
	}

	with (blpapi_Element_t *secs) {
		blpapi_Element_getElement(els, &secs, "securities", 0);
		if (UNLIKELY(secs == NULL)) {
			errno = 0, error("\
Error: cannot fill securities into request");
			rc = -1;
			goto out;
		}
		for (size_t i = 0U; i < argi->topic_nargs; i++) {
			const char *top = argi->topic_args[i];

			blpapi_Element_setValueString(
				secs, top, BLPAPI_ELEMENT_INDEX_END);
		}
	}

	with (blpapi_Element_t *flds) {
		blpapi_Element_getElement(els, &flds, "fields", 0);
		if (UNLIKELY(flds == NULL)) {
			errno = 0, error("\
Error: cannot fill fields into request");
			rc = -1;
			goto out;
		}
		for (size_t i = 0U; i < argi->field_nargs; i++) {
			const char *fld = argi->field_args[i];

			blpapi_Element_setValueString(
				flds, fld, BLPAPI_ELEMENT_INDEX_END);
		}
	}

	/* finally send the whole shebang */
	blpapi_Session_sendRequest(s, req, &cid, 0, 0, 0, 0);
out:
	blpapi_Request_destroy(req);
	return rc;
}

static int
svc_sta_sub(blpapi_Session_t *s, const struct yuck_cmd_sub_s argi[static 1U])
{
	blpapi_SubscriptionList_t *subs;
	const char *opts[] = {};

	if (UNLIKELY((subs = blpapi_SubscriptionList_create()) == NULL)) {
		errno = 0, error("\
Error: cannot instantiate subscriptions");
		return -1;
	}

	/* subscribe */
	for (size_t i = 0U; i < argi->topic_nargs; i++) {
		const char *top = argi->topic_args[i];
		const char **flds = deconst(argi->field_args);
		const size_t nflds = argi->field_nargs;
		blpapi_CorrelationId_t cid = {
			.size = sizeof(cid),
			.valueType = BLPAPI_CORRELATION_TYPE_INT,
			.value.intValue = i + 1U,
		};

		blpapi_SubscriptionList_add(
			subs, top, &cid, flds, opts, nflds, countof(opts));
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
	const char *svc;

	switch (ctx->st) {
		const yuck_t *argi;

	case ST_UNK:
	case ST_FIN:
		switch ((argi = ctx->argi)->cmd) {
			static const char svc_get[] = "//blp/refdata";
			static const char svc_sub[] = "//blp/mktdata";

		case BLPCLI_CMD_GET:
			svc = svc_get;
			break;
		case BLPCLI_CMD_SUB:
			svc = svc_sub;
			break;
		default:
			/* hm? */
			errno = 0, error("\
Warning: session message other than GET/SUB received");
			return -1;
		}
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

	switch (argi->cmd) {
	case BLPCLI_CMD_GET:
		if (svc_sta_get(sess, (const struct yuck_cmd_get_s*)argi) < 0) {
			return -1;
		}
		break;
	case BLPCLI_CMD_SUB:
		if (svc_sta_sub(sess, (const struct yuck_cmd_sub_s*)argi) < 0) {
			return -1;
		}
		break;
	default:
		/* huh? */
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
	const yuck_t *argi;

	switch (ctx->st) {
	case ST_SVC:
		argi = ctx->argi;
		break;
	case ST_UNK:
	case ST_SES:
	case ST_FIN:
	default:
		/* do fuck all */
		return -1;
	}

	switch (argi->cmd) {
	case BLPCLI_CMD_GET:
		/* we should not be here */
		return -1;
	case BLPCLI_CMD_SUB:
		/* all is good and well */
		break;
	default:
		/* huh? */
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

	/* barf early if there's no command */
	if (UNLIKELY(!argi->cmd)) {
		errno = 0, error("\
Error: no command given.  See --help.");
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
