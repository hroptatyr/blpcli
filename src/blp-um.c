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
#include <net/if.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <math.h>
#include <blpapi_correlationid.h>
#include <blpapi_element.h>
#include <blpapi_event.h>
#include <blpapi_message.h>
#include <blpapi_request.h>
#include <blpapi_session.h>
#include <blpapi_subscriptionlist.h>
#include "nifty.h"

#include "blp-um.yucc"

#define MCAST_ADDR	"ff05::134"
#define MCAST_PORT	7878

typedef struct {
	blpapi_Float64_t bid;
	blpapi_Float64_t ask;
} quo_t;

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
	size_t ninstr;
	char *const *instr;
	quo_t *book;
	uint8_t *touched;
	int rc;
	int sok;
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


/* socket goodies */
static inline void
setsock_nonblock(int sock)
{
	int opts;

	/* get former options */
	opts = fcntl(sock, F_GETFL);
	if (opts < 0) {
		return;
	}
	opts |= O_NONBLOCK;
	(void)fcntl(sock, F_SETFL, opts);
	return;
}

static int
mc6_socket(void)
{
	volatile int s;

	/* try v6 first */
	if ((s = socket(PF_INET6, SOCK_DGRAM, IPPROTO_IP)) < 0) {
		return -1;
	}

#if defined IPV6_V6ONLY
	{
		int yes = 1;
		setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &yes, sizeof(yes));
	}
#endif	/* IPV6_V6ONLY */
	/* be less blocking */
	setsock_nonblock(s);
	return s;
}

static int
mc6_set_pub(int s, const char *addr, short unsigned int port, const char *iface)
{
	struct sockaddr_in6 sa = {
		.sin6_family = AF_INET6,
		.sin6_addr = IN6ADDR_ANY_INIT,
		.sin6_port = htons(port),
		.sin6_flowinfo = 0,
		.sin6_scope_id = 0,
	};

	/* we pick link-local here for simplicity */
	if (inet_pton(AF_INET6, addr, &sa.sin6_addr) < 0) {
		return -1;
	}
	/* scope id */
	if (iface != NULL) {
		sa.sin6_scope_id = if_nametoindex(iface);
	}
	/* and do the connect() so we can use send() instead of sendto() */
	return connect(s, (struct sockaddr*)&sa, sizeof(sa));
}

static int
mc6_unset_pub(int UNUSED(s))
{
	/* do fuckall */
	return 0;
}


static const char *flds[] = {
	"BID", "ASK",
};

static void
send_quo(int sok, const char *instr, quo_t q)
{
	char buf[1280U];
	size_t len;

	memcpy(buf, instr, (len = strlen(instr)));
	buf[len++] = '\t';
	if (!isnan(q.bid)) {
		len += snprintf(buf + len, sizeof(buf) - len, "%f", q.bid);
	}
	buf[len++] = '\t';
	if (!isnan(q.ask)) {
		len += snprintf(buf + len, sizeof(buf) - len, "%f", q.ask);
	}
	/* and finalise */
	buf[len++] = '\n';
	buf[len] = '\0';
	send(sok, buf, len, 0);
	return;
}

static void
dump_pub(const struct ctx_s ctx[static 1U], blpapi_Message_t *msg)
{
	blpapi_Element_t *els;
	blpapi_Element_t *el;
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
	if (UNLIKELY((els = blpapi_Message_elements(msg)) == NULL)) {
		goto nop;
	}

	if (!blpapi_Element_getElement(els, &el, flds[0U], NULL)) {
		blpapi_Element_getValueAsFloat64(el, &ctx->book[ix].bid, 0U);
		ctx->touched[ix] = 1U;
	}
	if (!blpapi_Element_getElement(els, &el, flds[1U], NULL)) {
		blpapi_Element_getValueAsFloat64(el, &ctx->book[ix].ask, 0U);
		ctx->touched[ix] = 1U;
	}
	if (!isnan(ctx->book[ix].bid && !isnan(ctx->book[ix].ask))) {
		send_quo(ctx->sok, ctx->instr[ix], ctx->book[ix]);
		ctx->touched[ix] = 0U;
	}

nop:
	return;
}

static void
dump_evs(const struct ctx_s ctx[static 1U], blpapi_MessageIterator_t *iter)
{
	blpapi_Message_t *msg;

	memset(ctx->book, -1, sizeof(*ctx->book) * ctx->ninstr);
	memset(ctx->touched, 0, sizeof(*ctx->touched) * ctx->ninstr);
	while (!blpapi_MessageIterator_next(iter, &msg)) {
		dump_pub(ctx, msg);
	}
	/* send the touched ones now */
	for (size_t i = 0U; i < ctx->ninstr; i++) {
		if (!ctx->touched[i]) {
			continue;
		}
		send_quo(ctx->sok, ctx->instr[i], ctx->book[i]);
	}
	return;
}


static int
svc_sta_sub(blpapi_Session_t *s, char *const *instr, size_t ninstr)
{
	blpapi_SubscriptionList_t *subs;
	const char *opts[] = {};

	if (UNLIKELY((subs = blpapi_SubscriptionList_create()) == NULL)) {
		errno = 0, error("\
Error: cannot instantiate subscriptions");
		return -1;
	}

	/* subscribe */
	for (size_t i = 0U; i < ninstr; i++) {
		const char *top = instr[i];
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
	kill(0, SIGQUIT);
	return 0;
}

static int
svc_sta(blpapi_Session_t *sess, struct ctx_s *ctx)
{
	switch (ctx->st) {
	case ST_SES:
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

	if (svc_sta_sub(sess, ctx->instr, ctx->ninstr) < 0) {
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
	static struct ctx_s ctx;
	blpapi_Session_t *sess = NULL;
	int sok = -1;
	int rc = 0;

	/* parse options, set up longjmp target and
	 * install a C-c handler */
	if (yuck_parse(argi, argc, argv) < 0) {
		errno = 0, error("\
Fatal: cannot parse options");
		rc = 1;
		goto out;
	}

	ctx.instr = argi->args, ctx.ninstr = argi->nargs;
	ctx.book = malloc(argi->nargs * sizeof(*ctx.book));
	ctx.touched = malloc(argi->nargs * sizeof(*ctx.touched));

	/* we can't do with interruptions */
	block_sigs();

	/* open multicast channel */
	if (UNLIKELY((sok = mc6_socket()) < 0)) {
		error("\
Error: cannot create multicast socket");
		rc = 1;
		goto out;
	} else if (mc6_set_pub(sok, MCAST_ADDR, MCAST_PORT, NULL) < 0) {
			error("\
Error: cannot activate publishing mode on socket %d", sok);
			rc = 1;
			goto out;
	}
	/* this can be considered ready */
	ctx.sok = sok;

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
	if (sok >= 0) {
		mc6_unset_pub(sok);
		close(sok);
	}
	if (sess != NULL) {
		blpapi_Session_stop(sess);
		blpapi_Session_destroy(sess);
		sess = NULL;
	}
	if (ctx.book) {
		free(ctx.book);
	}

	yuck_free(argi);
	return rc;
}

/* blpcli.c ends here */
