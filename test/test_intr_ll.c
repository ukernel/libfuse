/*
 * FUSE: Filesystem in Userspace
 *
 * A low-level filesystem with one regular file "/f" whose blocking setlk
 * waits until the request is interrupted. Used by cases/misc/interrupt.sh
 * to check that FUSE_INTERRUPT reaches a request that registered an
 * interrupt callback.
 *
 * This program can be distributed under the terms of the GNU LGPLv2.
 * See the file COPYING.LIB
 */

#define FUSE_USE_VERSION FUSE_MAKE_VERSION(3, 17)
#include <fuse_lowlevel.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cv  = PTHREAD_COND_INITIALIZER;

struct waiter {
	fuse_req_t req;
	int interrupted;
};

static void on_intr(fuse_req_t req, void *data)
{
	struct waiter *w = data;

	(void)req;
	pthread_mutex_lock(&mtx);
	w->interrupted = 1;
	pthread_cond_broadcast(&cv);
	pthread_mutex_unlock(&mtx);
}

static void tl_init(void *ud, struct fuse_conn_info *conn)
{
	(void)ud;
	conn->no_interrupt = 0;
}

static void tl_getattr(fuse_req_t req, fuse_ino_t ino,
		       struct fuse_file_info *fi)
{
	struct stat st = {0};

	(void)fi;
	st.st_ino = ino;
	st.st_mode = ino == 1 ? S_IFDIR | 0755 : S_IFREG | 0644;
	st.st_nlink = 1;
	fuse_reply_attr(req, &st, 0);
}

static void tl_lookup(fuse_req_t req, fuse_ino_t parent, const char *name)
{
	struct fuse_entry_param e = {0};

	if (parent != 1 || strcmp(name, "f") != 0) {
		fuse_reply_err(req, ENOENT);
		return;
	}
	e.ino = 2;
	e.attr.st_ino = 2;
	e.attr.st_mode = S_IFREG | 0644;
	e.attr.st_nlink = 1;
	fuse_reply_entry(req, &e);
}

static void tl_open(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
	(void)ino;
	fuse_reply_open(req, fi);
}

/* Never called by the test; libfuse offers POSIX locks only with both ops. */
static void tl_getlk(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi,
		     struct flock *lk)
{
	(void)ino;
	(void)fi;
	lk->l_type = F_UNLCK;
	fuse_reply_lock(req, lk);
}

static void tl_setlk(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi,
		     struct flock *lk, int sleep)
{
	struct waiter w = { req, 0 };

	(void)ino;
	(void)fi;
	(void)lk;
	if (!sleep) {
		fuse_reply_err(req, EAGAIN);
		return;
	}
	fuse_req_interrupt_func(req, on_intr, &w);
	pthread_mutex_lock(&mtx);
	while (!w.interrupted)
		pthread_cond_wait(&cv, &mtx);
	pthread_mutex_unlock(&mtx);
	fuse_reply_err(req, EINTR);
}

static const struct fuse_lowlevel_ops ops = {
	.init    = tl_init,
	.lookup  = tl_lookup,
	.getattr = tl_getattr,
	.open    = tl_open,
	.getlk   = tl_getlk,
	.setlk   = tl_setlk,
};

int main(int argc, char *argv[])
{
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	struct fuse_cmdline_opts opts;
	struct fuse_loop_config *config;
	struct fuse_session *se;
	int ret;

	if (fuse_parse_cmdline(&args, &opts) != 0)
		return 1;
	se = fuse_session_new(&args, &ops, sizeof(ops), NULL);
	if (se == NULL || fuse_session_mount(se, opts.mountpoint) != 0)
		return 1;
	if (fuse_daemonize(opts.foreground) != 0)
		return 1;
	config = fuse_loop_cfg_create();
	fuse_loop_cfg_set_clone_fd(config, opts.clone_fd);
	ret = fuse_session_loop_mt(se, config);
	fuse_loop_cfg_destroy(config);
	fuse_session_unmount(se);
	fuse_session_destroy(se);
	free(opts.mountpoint);
	fuse_opt_free_args(&args);
	return ret;
}
