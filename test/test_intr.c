/*
 * FUSE: Filesystem in Userspace
 *
 * Unit tests for selective interrupts in the low-level library.
 *
 * Requests are fed straight into fuse_session_process_buf() and replies are
 * captured through a custom io, so no mount is needed. Only requests that
 * called fuse_req_interrupt_func() take part in interrupt matching; an
 * unmatched FUSE_INTERRUPT is dropped without a reply, and a request that
 * never registers takes no session lock for interrupts.
 *
 * This program can be distributed under the terms of the GNU LGPLv2.
 * See the file COPYING.LIB
 */

#define FUSE_USE_VERSION FUSE_MAKE_VERSION(3, 17)

#include "fuse_config.h"
#include "fuse_lowlevel.h"
#include "fuse_i.h"		/* se->lock, for the hot path test */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * Marks the unique of a FUSE_INTERRUPT; the uapi header does not export it.
 * Kernel: REDFS_INT_REQ_BIT in contrib/redfs.ko/src/<ver>/redfs_i.h,
 * FUSE_INT_REQ_BIT in fs/fuse/fuse_i.h.
 */
#undef FUSE_INT_REQ_BIT
#define FUSE_INT_REQ_BIT (1ULL << 0)

#define FAIL(fmt, ...) \
	do { fprintf(stderr, "FAIL: %s:%d: " fmt "\n", __func__, __LINE__, \
		     ##__VA_ARGS__); exit(1); } while (0)
#define CHECK(cond) \
	do { if (!(cond)) FAIL("%s", #cond); } while (0)

/* ------------------------------------------------------------------ */
/* Reply capture                                                       */
/* ------------------------------------------------------------------ */

#define MAX_REPLIES 64

struct reply {
	uint64_t unique;
	int error;
};

static pthread_mutex_t rep_lock = PTHREAD_MUTEX_INITIALIZER;
static struct reply replies[MAX_REPLIES];
static int nreplies;
static int capture = 1;

static ssize_t t_writev(int fd, struct iovec *iov, int count, void *userdata)
{
	const struct fuse_out_header *out = iov[0].iov_base;
	ssize_t len = 0;
	int i;

	(void)fd;
	(void)userdata;
	for (i = 0; i < count; i++)
		len += iov[i].iov_len;
	pthread_mutex_lock(&rep_lock);
	if (capture && nreplies < MAX_REPLIES) {
		replies[nreplies].unique = out->unique;
		replies[nreplies].error = out->error;
		nreplies++;
	}
	pthread_mutex_unlock(&rep_lock);
	return len;
}

static ssize_t t_read(int fd, void *buf, size_t len, void *userdata)
{
	(void)fd;
	(void)buf;
	(void)len;
	(void)userdata;
	errno = ENODEV;
	return -1;
}

static void reset_replies(void)
{
	pthread_mutex_lock(&rep_lock);
	nreplies = 0;
	pthread_mutex_unlock(&rep_lock);
}

static int count_replies(void)
{
	int n;

	pthread_mutex_lock(&rep_lock);
	n = nreplies;
	pthread_mutex_unlock(&rep_lock);
	return n;
}

static void expect_only_reply(uint64_t unique, int error)
{
	pthread_mutex_lock(&rep_lock);
	if (nreplies != 1 || replies[0].unique != unique ||
	    replies[0].error != error) {
		int i;

		for (i = 0; i < nreplies; i++)
			fprintf(stderr, "  reply %d: unique %llu error %d\n", i,
				(unsigned long long)replies[i].unique,
				replies[i].error);
		FAIL("expected exactly one reply, unique %llu error %d, got %d",
		     (unsigned long long)unique, error, nreplies);
	}
	pthread_mutex_unlock(&rep_lock);
}

/* ------------------------------------------------------------------ */
/* Filesystem                                                          */
/* ------------------------------------------------------------------ */

enum setlk_mode {
	SETLK_REGISTER,		/* register on_intr and leave the request */
	SETLK_PARK,		/* leave the request, do not register */
	SETLK_REGISTER_REPLY,	/* register a callback that replies itself */
};

static enum setlk_mode setlk_mode;
static fuse_req_t parked;
static atomic_int intr_calls;

static void on_intr(fuse_req_t req, void *data)
{
	(void)req;
	(void)data;
	atomic_fetch_add(&intr_calls, 1);
}

static void on_intr_reply(fuse_req_t req, void *data)
{
	(void)data;
	atomic_fetch_add(&intr_calls, 1);
	fuse_reply_err(req, EINTR);
}

static void t_init(void *ud, struct fuse_conn_info *conn)
{
	(void)ud;
	conn->no_interrupt = 0;
}

static void t_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	struct stat st = {0};

	(void)fi;
	st.st_ino = ino;
	st.st_mode = S_IFREG | 0644;
	st.st_nlink = 1;
	fuse_reply_attr(req, &st, 0);
}

static void t_setlk(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi,
		    struct flock *lk, int sleep)
{
	(void)ino;
	(void)fi;
	(void)lk;
	(void)sleep;

	switch (setlk_mode) {
	case SETLK_REGISTER:
		fuse_req_interrupt_func(req, on_intr, NULL);
		parked = req;
		break;
	case SETLK_PARK:
		parked = req;
		break;
	case SETLK_REGISTER_REPLY:
		/* May reply from inside, then req is gone. */
		parked = NULL;
		fuse_req_interrupt_func(req, on_intr_reply, NULL);
		break;
	}
}

static const struct fuse_lowlevel_ops ops = {
	.init    = t_init,
	.getattr = t_getattr,
	.setlk   = t_setlk,
};

/* ------------------------------------------------------------------ */
/* Request builders                                                    */
/* ------------------------------------------------------------------ */

static void send_req(struct fuse_session *se, uint32_t opcode, uint64_t unique,
		     uint64_t nodeid, const void *arg, size_t argsize)
{
	char mem[sizeof(struct fuse_in_header) + 128];
	struct fuse_in_header *in = (struct fuse_in_header *)mem;
	struct fuse_buf buf = { 0 };

	memset(mem, 0, sizeof(mem));
	in->len = sizeof(*in) + argsize;
	in->opcode = opcode;
	in->unique = unique;
	in->nodeid = nodeid;
	in->uid = getuid();
	in->gid = getgid();
	in->pid = getpid();
	memcpy(in + 1, arg, argsize);
	buf.mem = mem;
	buf.size = in->len;
	fuse_session_process_buf(se, &buf);
}

static void send_init(struct fuse_session *se)
{
	struct fuse_init_in arg = {
		.major = FUSE_KERNEL_VERSION,
		.minor = FUSE_KERNEL_MINOR_VERSION,
		.max_readahead = 65536,
		.flags = FUSE_POSIX_LOCKS,
	};

	send_req(se, FUSE_INIT, 2, 0, &arg, sizeof(arg));
}

static void send_setlkw(struct fuse_session *se, uint64_t unique)
{
	struct fuse_lk_in arg = { 0 };

	arg.owner = 1;
	arg.lk.start = 0;
	arg.lk.end = UINT64_MAX;
	arg.lk.type = F_WRLCK;
	arg.lk.pid = getpid();
	send_req(se, FUSE_SETLKW, unique, 2, &arg, sizeof(arg));
}

static void send_interrupt(struct fuse_session *se, uint64_t unique)
{
	struct fuse_interrupt_in arg = { .unique = unique };

	send_req(se, FUSE_INTERRUPT, unique | FUSE_INT_REQ_BIT, 0, &arg,
		 sizeof(arg));
}

static void send_getattr(struct fuse_session *se, uint64_t unique)
{
	struct fuse_getattr_in arg = { 0 };

	send_req(se, FUSE_GETATTR, unique, 2, &arg, sizeof(arg));
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

/* A registered request gets its callback, and the interrupt no reply. */
static void test_matched(struct fuse_session *se)
{
	reset_replies();
	atomic_store(&intr_calls, 0);
	setlk_mode = SETLK_REGISTER;
	send_setlkw(se, 10);
	CHECK(parked != NULL);
	send_interrupt(se, 10);
	CHECK(atomic_load(&intr_calls) == 1);
	CHECK(fuse_req_interrupted(parked));
	CHECK(count_replies() == 0);
	fuse_reply_err(parked, EINTR);
	parked = NULL;
	expect_only_reply(10, -EINTR);
	printf("PASS: matched interrupt calls the callback\n");
}

/*
 * An interrupt that matches nothing is dropped. Stock libfuse parks it and
 * answers EAGAIN when the next request arrives, which the kernel requeues
 * forever.
 */
static void test_unmatched_dropped(struct fuse_session *se)
{
	reset_replies();
	send_interrupt(se, 20);
	send_getattr(se, 22);
	send_getattr(se, 24);
	pthread_mutex_lock(&rep_lock);
	CHECK(nreplies == 2);
	CHECK(replies[0].unique == 22 && replies[0].error == 0);
	CHECK(replies[1].unique == 24 && replies[1].error == 0);
	pthread_mutex_unlock(&rep_lock);
	printf("PASS: unmatched interrupt dropped without a reply\n");
}

/* Dispatched, then interrupted, then registered: the callback still runs. */
static void test_before_register(struct fuse_session *se)
{
	reset_replies();
	atomic_store(&intr_calls, 0);
	setlk_mode = SETLK_PARK;
	send_setlkw(se, 30);
	CHECK(parked != NULL);
	send_interrupt(se, 30);
	CHECK(atomic_load(&intr_calls) == 0);
	fuse_req_interrupt_func(parked, on_intr, NULL);
	CHECK(atomic_load(&intr_calls) == 1);
	CHECK(fuse_req_interrupted(parked));
	fuse_reply_err(parked, EINTR);
	parked = NULL;
	expect_only_reply(30, -EINTR);
	printf("PASS: interrupt before registration is applied on register\n");
}

/* The interrupt is read before the request it names is dispatched. */
static void test_before_dispatch(struct fuse_session *se)
{
	reset_replies();
	atomic_store(&intr_calls, 0);
	setlk_mode = SETLK_REGISTER;
	send_interrupt(se, 40);
	CHECK(count_replies() == 0);
	send_setlkw(se, 40);
	CHECK(parked != NULL);
	CHECK(atomic_load(&intr_calls) == 1);
	fuse_reply_err(parked, EINTR);
	parked = NULL;
	expect_only_reply(40, -EINTR);
	printf("PASS: interrupt before dispatch is applied on register\n");
}

/* A callback may reply to the request itself, from either delivery path. */
static void test_callback_replies(struct fuse_session *se)
{
	reset_replies();
	atomic_store(&intr_calls, 0);
	setlk_mode = SETLK_REGISTER_REPLY;
	send_setlkw(se, 50);
	send_interrupt(se, 50);
	CHECK(atomic_load(&intr_calls) == 1);
	expect_only_reply(50, -EINTR);

	reset_replies();
	send_interrupt(se, 60);
	send_setlkw(se, 60);
	CHECK(atomic_load(&intr_calls) == 2);
	expect_only_reply(60, -EINTR);
	printf("PASS: callback that replies does not deadlock\n");
}

/* An interrupt for a request that was already answered is dropped. */
static void test_after_reply(struct fuse_session *se)
{
	reset_replies();
	atomic_store(&intr_calls, 0);
	setlk_mode = SETLK_REGISTER;
	send_setlkw(se, 80);
	CHECK(parked != NULL);
	fuse_reply_err(parked, 0);
	parked = NULL;
	send_interrupt(se, 80);
	CHECK(atomic_load(&intr_calls) == 0);
	expect_only_reply(80, 0);
	printf("PASS: interrupt after the reply is dropped\n");
}

/*
 * fuse_reply_none() on an io-uring request sends nothing and commits
 * nothing, so it has to unregister itself; fuse_free_req() leaves uring
 * entries alone. A real entry needs a kernel ring, so a heap request is
 * dressed up as one: is_uring set and the extra baseline reference an
 * entry keeps.
 */
static void test_uring_reply_none(struct fuse_session *se)
{
	fuse_req_t req;

	reset_replies();
	atomic_store(&intr_calls, 0);
	setlk_mode = SETLK_REGISTER;
	send_setlkw(se, 90);
	CHECK(parked != NULL);
	req = parked;
	parked = NULL;
	req->is_uring = true;
	req->ref_cnt++;
	fuse_reply_none(req);
	CHECK(!req->intr_registered);
	send_interrupt(se, 90);
	CHECK(atomic_load(&intr_calls) == 0);
	CHECK(count_replies() == 0);
	/* Back to a heap request, and drop the last reference. */
	req->is_uring = false;
	fuse_reply_none(req);
	printf("PASS: reply_none on a uring request unregisters it\n");
}

struct hot_arg {
	struct fuse_session *se;
	atomic_int done;
};

static void *hot_worker(void *data)
{
	struct hot_arg *a = data;

	send_getattr(a->se, 70);
	atomic_store(&a->done, 1);
	return NULL;
}

/*
 * D3 regression guard: with interrupts enabled, a request that never
 * registers must not touch se->lock on its way in or out. Hold the lock
 * and make sure a GETATTR still completes.
 */
static void test_hot_path_lock_free(struct fuse_session *se)
{
	struct hot_arg a = { .se = se };
	pthread_t t;
	int i;

	reset_replies();
	CHECK(!se->conn.no_interrupt);
	pthread_mutex_lock(&se->lock);
	CHECK(pthread_create(&t, NULL, hot_worker, &a) == 0);
	for (i = 0; i < 200 && !atomic_load(&a.done); i++)
		usleep(10000);
	pthread_mutex_unlock(&se->lock);
	pthread_join(t, NULL);
	if (i == 200)
		FAIL("GETATTR blocked on se->lock with no_interrupt == 0");
	expect_only_reply(70, 0);
	printf("PASS: unregistered request takes no session lock\n");
}

/* ------------------------------------------------------------------ */
/* Race stress: interrupt against dispatch, register and reply          */
/* ------------------------------------------------------------------ */

/*
 * Each round dispatches one SETLKW whose handler registers a callback and
 * replies at once. Its interrupt comes
 *  - round % 4 == 0: from the main thread before the dispatch, so it is
 *    dropped, remembered, and applied by fuse_req_interrupt_func() in the
 *    owner thread (a guaranteed callback, hence the lower bound);
 *  - odd rounds: from the interrupt thread, published before the dispatch,
 *    so it lands before the dispatch, between register and reply (the
 *    callback in intr_deliver() racing the reply and its unregister-wait),
 *    or after the reply;
 *  - round % 4 == 2: from the interrupt thread after the reply, a lookup
 *    for a request already freed or, for the next round, not yet sent.
 * Every round checks the callback ran at most once and never after the
 * reply returned.
 */

#define STRESS_ROUNDS 20000

struct stress_req {
	atomic_int calls;
	atomic_int replied;
};

static void on_intr_stress(fuse_req_t req, void *data)
{
	struct stress_req *s = data;

	(void)req;
	if (atomic_load(&s->replied))
		FAIL("callback ran after the request was answered");
	atomic_fetch_add(&s->calls, 1);
}

static atomic_ullong stress_unique;
static atomic_int stress_stop;
static struct stress_req *stress_cur;

static void t_setlk_stress(fuse_req_t req, struct stress_req *s)
{
	fuse_req_interrupt_func(req, on_intr_stress, s);
	if (rand() & 1)
		sched_yield();
	/*
	 * fuse_reply_err() returns only once a running callback has finished
	 * and no new one can start, so the flag below is set after the last
	 * possible callback.
	 */
	fuse_reply_err(req, EINTR);
	atomic_store(&s->replied, 1);
}

static void *stress_intr(void *data)
{
	struct fuse_session *se = data;
	unsigned long long last = 0;

	while (!atomic_load(&stress_stop)) {
		unsigned long long u = atomic_load(&stress_unique);

		if (u == last) {
			sched_yield();
			continue;
		}
		last = u;
		send_interrupt(se, u);
	}
	return NULL;
}

static void stress_setlk(fuse_req_t req, fuse_ino_t ino,
			 struct fuse_file_info *fi, struct flock *lk, int sleep)
{
	(void)ino;
	(void)fi;
	(void)lk;
	(void)sleep;
	t_setlk_stress(req, stress_cur);
}

static void test_stress(void)
{
	static const struct fuse_lowlevel_ops sops = {
		.init  = t_init,
		.setlk = stress_setlk,
	};
	static char name[] = "test_intr";
	char *argv[] = { name, NULL };
	struct fuse_args args = FUSE_ARGS_INIT(1, argv);
	const struct fuse_custom_io io = { .writev = t_writev, .read = t_read };
	struct fuse_session *se;
	pthread_t t;
	int i, fired = 0;
	int fd;

	alarm(30);
	se = fuse_session_new(&args, &sops, sizeof(sops), NULL);
	CHECK(se != NULL);
	fd = open("/dev/null", O_RDWR);
	CHECK(fd >= 0);
	CHECK(fuse_session_custom_io(se, &io, sizeof(io), fd) == 0);
	send_init(se);
	capture = 0;
	CHECK(pthread_create(&t, NULL, stress_intr, se) == 0);
	for (i = 0; i < STRESS_ROUNDS; i++) {
		struct stress_req s = { 0 };
		uint64_t u = 1000 + 2 * (uint64_t)i;

		stress_cur = &s;
		if (i % 4 == 0)
			send_interrupt(se, u);
		else if (i & 1)
			atomic_store(&stress_unique, u);
		send_setlkw(se, u);
		if (i % 4 == 2)
			atomic_store(&stress_unique, u);
		if (i % 4 == 0 && atomic_load(&s.calls) != 1)
			FAIL("round %d: remembered interrupt not applied", i);
		if (atomic_load(&s.calls) > 1)
			FAIL("callback ran %d times", atomic_load(&s.calls));
		fired += atomic_load(&s.calls);
	}
	atomic_store(&stress_stop, 1);
	pthread_join(t, NULL);
	capture = 1;
	fuse_session_destroy(se);
	fuse_opt_free_args(&args);
	CHECK(fired >= STRESS_ROUNDS / 4);
	printf("PASS: stress, %d rounds, %d callbacks\n", STRESS_ROUNDS, fired);
}

static void silent_log(enum fuse_log_level level, const char *fmt, va_list ap)
{
	if (level <= FUSE_LOG_ERR)
		vfprintf(stderr, fmt, ap);
}

static const struct {
	const char *name;
	void (*fn)(struct fuse_session *se);
} tests[] = {
	{ "matched",		test_matched },
	{ "unmatched",		test_unmatched_dropped },
	{ "before_register",	test_before_register },
	{ "before_dispatch",	test_before_dispatch },
	{ "callback_replies",	test_callback_replies },
	{ "after_reply",	test_after_reply },
	{ "hot_path",		test_hot_path_lock_free },
	{ "uring_reply_none",	test_uring_reply_none },
};

/* With an argument, run only the test of that name ("stress" included). */
int main(int argc, char *argv[])
{
	static char name[] = "test_intr";
	char *sargv[] = { name, NULL };
	struct fuse_args args = FUSE_ARGS_INIT(1, sargv);
	const struct fuse_custom_io io = { .writev = t_writev, .read = t_read };
	const char *only = argc > 1 ? argv[1] : NULL;
	struct fuse_session *se;
	size_t i;
	int fd;

	/* A deadlock fails the test instead of hanging the runner. */
	alarm(60);
	setvbuf(stdout, NULL, _IOLBF, 0);
	fuse_set_log_func(silent_log);

	se = fuse_session_new(&args, &ops, sizeof(ops), NULL);
	CHECK(se != NULL);
	fd = open("/dev/null", O_RDWR);
	CHECK(fd >= 0);
	CHECK(fuse_session_custom_io(se, &io, sizeof(io), fd) == 0);
	send_init(se);
	expect_only_reply(2, 0);

	for (i = 0; i < sizeof(tests) / sizeof(tests[0]); i++)
		if (!only || strcmp(only, tests[i].name) == 0)
			tests[i].fn(se);

	fuse_session_destroy(se);
	fuse_opt_free_args(&args);

	if (!only || strcmp(only, "stress") == 0)
		test_stress();
	return 0;
}
