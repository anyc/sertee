/*
 * sertee
 * ----------
 * 
 * sertee provides multiple "copies" of a character device using the CUSE
 * ( character device in userspace) interface of the Linux kernel
 * 
 * Written 2022 by Mario Kicherer (http://kicherer.org)
 * 
 * License: MPL-2.0
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <poll.h>

#define FUSE_USE_VERSION 34

#include <cuse_lowlevel.h>
#include <fuse_opt.h>
#include <fuse.h>

#ifdef DEBUG
#define DBG(fmt, ...) do { printf(fmt, ##__VA_ARGS__); } while (0)
#else
#define DBG(fmt, ...) do { } while (0)
#endif

#define STRINGIFYB(x) #x
#define STRINGIFY(x) STRINGIFYB(x)

#define DEFAULT_BUFSIZE 1024

struct sertee;

struct sertee_dev {
	struct sertee *sertee;
	
	char *name;
	
	const char *dev_info_argv[1];
	struct cuse_info ci;
	struct fuse_session *fsess;
	struct epoll_event eevent;
	struct fuse_pollhandle *poll_handle;
	
	char *pos;
	unsigned char round;
	unsigned int n_clients;
};

struct sertee {
	struct sertee_dev **devs;
	unsigned int n_devs;
	
	char *source_name;
	char *dev_names;
	
	int epoll_fd;
	int source_fd;
	
	char *buf;
	char *pos;
	unsigned char round;
	
	struct epoll_event source_eevent;
	
	char show_help;
	size_t bufsize;
};

#define SERTEE_OPT(t, p) { t, offsetof(struct sertee, p), 1 }

static const struct fuse_opt sertee_opts[] = {
	SERTEE_OPT("-n %s", dev_names),
	SERTEE_OPT("--name=%s", dev_names),
	SERTEE_OPT("-S %s", source_name),
	SERTEE_OPT("--source=%s", source_name),
	SERTEE_OPT("--bufsize=%s", bufsize),
	FUSE_OPT_KEY("-h", 0),
	FUSE_OPT_KEY("--help", 0),
	FUSE_OPT_END
};

void show_help(FILE *fd) {
	fprintf(fd, "usage: sertee [options]\n");
	fprintf(fd, "\n");
	fprintf(fd, "options:\n");
	fprintf(fd, "    --help|-h             print this help message\n");
	fprintf(fd, "    --name=NAME|-n NAME   device names (mandatory)\n");
	fprintf(fd, "    --source=NAME|-S NAME source device name (mandatory)\n");
	fprintf(fd, "    --bufsize=SIZE        size of internal buffer (default: " STRINGIFY(DEFAULT_BUFSIZE) " bytes)\n");
	fprintf(fd, "\n");
}

static int sertee_process_arg(void *data, const char *arg, int key, struct fuse_args *outargs) {
	struct sertee *sertee;
	
	sertee = data;
	
	switch (key) {
		case 0:
			sertee->show_help = 1;
			
			// show our help text
			show_help(stdout);
			
			// show help for cuse parameters
			return fuse_opt_add_arg(outargs, "-ho");
		default:
			return 1;
	}
	
	return 0;
}

static void sertee_open(fuse_req_t req, struct fuse_file_info *fi) {
	struct sertee_dev *sertee_dev = (struct sertee_dev *) fuse_req_userdata(req);
	
	DBG("OPEN: %s\n", sertee_dev->name);
	
	sertee_dev->pos = sertee_dev->sertee->pos;
	// if buffer contains only valid data, allow client to read the old data
	sertee_dev->round = sertee_dev->sertee->round - (sertee_dev->sertee->round > 0 ? 1 : 0);
	sertee_dev->n_clients += 1;
	
	fuse_reply_open(req, fi);
}

static void sertee_release(fuse_req_t req, struct fuse_file_info *fi) {
	struct sertee_dev *sertee_dev = (struct sertee_dev *) fuse_req_userdata(req);
	
	DBG("RELEASE: %s\n", sertee_dev->name);
	
	sertee_dev->n_clients -= 1;
	if (sertee_dev->n_clients <= 0) {
		sertee_dev->pos = 0;
		sertee_dev->n_clients = 0;
	}
	
	// workaround to call send_ok() as client would hang without it
	fuse_reply_buf(req, 0, 0);
}

static char *get_data_end(struct sertee_dev *sertee_dev) {
	char *end;
	
	if (sertee_dev->pos < sertee_dev->sertee->pos) {
		end = sertee_dev->sertee->pos;
	} else
	if (sertee_dev->pos == sertee_dev->sertee->pos && sertee_dev->sertee->round == sertee_dev->round) {
		end = sertee_dev->sertee->buf;
	} else {
		end = sertee_dev->sertee->buf + sertee_dev->sertee->bufsize;
	}
	
	return end;
}

static size_t get_avail_data_size(struct sertee_dev *sertee_dev) {
	size_t size;
	char *end;
	
	end = get_data_end(sertee_dev);
	
	if (sertee_dev->pos >= end) {
		size = 0;
	} else {
		size = end - sertee_dev->pos;
	}
	
	return size;
}

static void sertee_read(fuse_req_t req, size_t size, off_t off,
						 struct fuse_file_info *fi)
{
	struct sertee_dev *sertee_dev = (struct sertee_dev *) fuse_req_userdata(req);
	char *end;
	size_t available;
	
	DBG("READ: %s off %zu size %zu rnd %u rnd_dev %u |", sertee_dev->name, off, size, sertee_dev->sertee->round, sertee_dev->round);
	
	end = get_data_end(sertee_dev);
	
	available = get_avail_data_size(sertee_dev);
	if (off > available) {
		size = 0;
	} else {
		if (off + size > available)
			size = available - off;
	}
	
	DBG("%zu %zu %zu | %zd\n", off, size, available, sertee_dev->pos - sertee_dev->sertee->buf);
	
	fuse_reply_buf(req, sertee_dev->pos + off, size);
	
	sertee_dev->pos += size;
	if (sertee_dev->pos == sertee_dev->sertee->buf + sertee_dev->sertee->bufsize) {
		sertee_dev->pos = sertee_dev->sertee->buf;
		sertee_dev->round += 1;
	}
}

static void sertee_write(fuse_req_t req, const char *buf, size_t size,
						  off_t off, struct fuse_file_info *fi)
{
	struct sertee_dev *sertee_dev = (struct sertee_dev *) fuse_req_userdata(req);
	ssize_t srv;
	
	DBG("WRITE: %s ", sertee_dev->name);
	
	srv = write(sertee_dev->sertee->source_fd, buf, size);
	
	DBG("%zu -> %zd\n", size, srv);
	
	if (srv < 0) {
		fuse_reply_err(req, errno);
		return;
	} else {
		size = srv;
	}
	
	fuse_reply_write(req, size);
}

// static void sertee_ioctl(fuse_req_t req, int cmd, void *arg,
// 						  struct fuse_file_info *fi, unsigned flags,
// 						  const void *in_buf, size_t in_bufsz, size_t out_bufsz)
// {
// 	struct sertee_dev *sertee_dev = (struct sertee_dev *) fuse_req_userdata(req);
// 	
// 	if (flags & FUSE_IOCTL_COMPAT) {
// 		fuse_reply_err(req, ENOSYS);
// 		return;
// 	}
// 	
// 	switch (cmd) {
// 		default:
// 			fuse_reply_err(req, EINVAL);
// 	}
// }

static void sertee_poll(fuse_req_t req, struct fuse_file_info *fi,
			  struct fuse_pollhandle *ph)
{
	unsigned revents = 0;
	struct sertee_dev *sertee_dev = (struct sertee_dev *) fuse_req_userdata(req);
	size_t available;
	
	DBG("POLL: %s ph %p old %p ", sertee_dev->name, ph, sertee_dev->poll_handle);
	
	if (ph) {
		if (sertee_dev->poll_handle)
			fuse_pollhandle_destroy(sertee_dev->poll_handle);
		
		sertee_dev->poll_handle = ph;
	}
	
	available = get_avail_data_size(sertee_dev);
	
	DBG("avail %zu\n", available);
	
	if (available > 0)
		revents |= POLLIN;
	
	fuse_reply_poll(req, revents);
}

static const struct cuse_lowlevel_ops sertee_llops = {
	.open = sertee_open,
	.release = sertee_release,
	.read = sertee_read,
	.write = sertee_write,
	.poll = sertee_poll,
// 	.ioctl = sertee_ioctl,
};

void source_read(struct sertee *sertee) {
	int i;
	ssize_t srv;
	struct sertee_dev *sertee_dev;
	
	DBG("SOURCE_READ\n");
	
	while (1) {
		srv = read(sertee->source_fd, sertee->pos, (sertee->buf + sertee->bufsize) - sertee->pos);
		if (srv < 0) {
			if (errno == EAGAIN || errno == EINTR)
				break;
			
			fprintf(stderr, "read() from source failed: %s\n", strerror(errno));
			return;
		} else
		if (srv == 0)
			break;
		
		for (i=0; i < sertee->n_devs; i++) {
			sertee_dev = sertee->devs[i];
			
			// if we overtake a device, move its pointer to the oldest data
			// TODO or reset the pointer?
			if (sertee->pos < sertee_dev->pos && sertee_dev->pos <= sertee->pos + srv) {
				sertee_dev->pos = sertee->pos + srv;
			}
		}
		
		sertee->pos += srv;
		
		if (sertee->pos == sertee->buf + sertee->bufsize) {
			sertee->round += 1;
			sertee->pos = sertee->buf;
		}
		
		DBG("source read %zd bytes new pos %zd\n", srv, sertee->pos - sertee->buf);
		
		for (i=0; i < sertee->n_devs; i++) {
			sertee_dev = sertee->devs[i];
			
			if (sertee_dev->poll_handle && get_avail_data_size(sertee_dev)) {
				fuse_notify_poll(sertee_dev->poll_handle);
				fuse_pollhandle_destroy(sertee_dev->poll_handle);
				sertee_dev->poll_handle = 0;
			}
		}
	}
}

#define MAX_EVENTS 5

void sertee_loop(struct sertee *sertee) {
	int res = 0;
	struct fuse_buf fbuf = {.mem = NULL, };
	struct epoll_event event, events[MAX_EVENTS];
	struct fuse_session *fsess;
	struct sertee_dev *sertee_dev;
	
	int event_count, i, stop;
	
	stop = 0;
	while (!stop) {
		event_count = epoll_wait(sertee->epoll_fd, events, MAX_EVENTS, 30000);
		if (event_count < 0)
			break;
		
		for (i=0; i < event_count; i++) {
			sertee_dev = (struct sertee_dev *) events[i].data.ptr;
			
			if (sertee_dev == 0) {
				source_read(sertee);
				continue;
			}
			
			res = fuse_session_receive_buf(sertee_dev->fsess, &fbuf);
			
			if (res == -EINTR)
				continue;
			if (res <= 0) {
				stop = 1;
				break;
			}
			
			fuse_session_process_buf(sertee_dev->fsess, &fbuf);
			
			if (fuse_session_exited(sertee_dev->fsess)) {
				stop = 1;
				break;
			}
		}
	}
	
	free(fbuf.mem);
}

static int sertee_lowlevel_main(int argc, char *argv[],
								struct sertee *sertee, struct sertee_dev *sertee_dev)
{
	int multithreaded;
	int res;
	
	sertee_dev->fsess = cuse_lowlevel_setup(argc, argv, &sertee_dev->ci, &sertee_llops, &multithreaded, sertee_dev);
	if (sertee_dev->fsess == NULL)
		return 1;
	
	if (multithreaded) {
		fprintf(stdout, "multithreading not supported\n");
	}
	
	sertee_dev->eevent.events = EPOLLIN;
	sertee_dev->eevent.data.ptr = sertee_dev;

	if (epoll_ctl(sertee->epoll_fd, EPOLL_CTL_ADD, fuse_session_fd(sertee_dev->fsess), &sertee_dev->eevent)) {
		fprintf(stderr, "epoll_ctl failed\n");
		return 1;
	}
	
	return 0;
}

int main(int argc, char **argv) {
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	int rv, i;
	struct sertee_dev *sertee_dev;
	struct sertee sertee;
	char *it, *saveit, *dev_name;
	
	
	memset(&sertee, 0, sizeof(struct sertee));
	
	sertee.bufsize = DEFAULT_BUFSIZE;
	rv = fuse_opt_parse(&args, &sertee, sertee_opts, sertee_process_arg);
	if (rv) {
		fprintf(stderr, "fuse_opt_parse failed: %d\n", rv);
		return rv;
	}
	
	if (sertee.show_help) {
		fuse_opt_free_args(&args);
		
		return 0;
	}
	
	if (!sertee.dev_names) {
		fprintf(stderr, "error, device names required\n");
		
		fuse_opt_free_args(&args);
		
		return 1;
	}
	if (!sertee.source_name) {
		fprintf(stderr, "error, source name required\n");
		
		fuse_opt_free_args(&args);
		
		return 1;
	}
	
	sertee.buf = malloc(sertee.bufsize);
	sertee.pos = sertee.buf;
	
	sertee.epoll_fd = epoll_create1(0);
	if (sertee.epoll_fd == -1) {
		fprintf(stderr, "epoll_create1 failed\n");
		return 1;
	}
	
	sertee.source_fd = open(sertee.source_name, O_RDWR | O_NOCTTY | O_SYNC | O_NONBLOCK);
	if (sertee.source_fd == -1) {
		fprintf(stderr, "opening source \"%s\" failed: %s\n", sertee.source_name, strerror(errno));
		return errno;
	}
	sertee.source_eevent.events = EPOLLIN;
	sertee.source_eevent.data.ptr = 0;
	
	if (epoll_ctl(sertee.epoll_fd, EPOLL_CTL_ADD, sertee.source_fd, &sertee.source_eevent)) {
		fprintf(stderr, "epoll_ctl(source) failed\n");
		return 1;
	}
	
	it = strtok_r(sertee.dev_names, ",", &saveit);
	while (it != NULL) {
		sertee.n_devs += 1;
		sertee.devs = (struct sertee_dev **) realloc(sertee.devs, sizeof(void *) * sertee.n_devs);
		
		sertee.devs[sertee.n_devs-1] = (struct sertee_dev*) calloc(1, sizeof(struct sertee_dev));
		sertee_dev = sertee.devs[sertee.n_devs-1];
		
		sertee_dev->sertee = &sertee;
		
		DBG("creating dev \"%s\" (%p)\n", it, sertee_dev);
		
		rv = asprintf(&sertee_dev->name, "DEVNAME=%s", it);
		if (rv < 0) {
			fprintf(stderr, "asprintf() failed: %d\n", rv);
			return rv;
		}
		
		sertee_dev->dev_info_argv[0] = sertee_dev->name;
		
		sertee_dev->ci.dev_info_argc = 1;
		sertee_dev->ci.dev_info_argv = &sertee_dev->dev_info_argv[0];
// 		sertee_dev->ci.flags = CUSE_UNRESTRICTED_IOCTL;
		
		rv = sertee_lowlevel_main(args.argc, args.argv, &sertee, sertee_dev);
		if (rv)
			break;
		
		it = strtok_r(NULL, ",", &saveit);
	}
	
	sertee_loop(&sertee);
	
	for (i=0; i < sertee.n_devs; i++) {
		fuse_session_reset(sertee.devs[i]->fsess);
		cuse_lowlevel_teardown(sertee.devs[i]->fsess);
	}
	
	if (close(sertee.epoll_fd)) {
		fprintf(stderr, "close epoll_fd failed\n");
		rv = 1;
	}
	
	fuse_opt_free_args(&args);
	
	return rv;
}
