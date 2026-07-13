// SPDX-License-Identifier: GPL-2.0
/*
 * rpmsg_rtt_decomp: RTT decomposition benchmark (Henry's decomp4 method).
 *
 * Per iteration, the RTT is split into segments from three probe layers:
 *   userspace : t0 (before write), t1 (after read), CLOCK_MONOTONIC
 *   kernel    : /proc/rpmsg_rtt -> b1 b1_tick b1d b2 b2_tick b2b
 *               (TX notify entry/done, echo IRQ entry, workqueue start)
 *   firmware  : 24-byte echo carries s1/s3 rdtime ticks
 *               (mailbox ISR entry, right before echo send)
 *
 * Both harts' time CSRs tick at 25 MHz (40 ns/tick). Forward and reverse
 * transport each contain the unknown inter-CLINT offset with opposite
 * sign, so their SUM (transport_total) is offset-free; raw fwd/rev are
 * also emitted for offset inspection.
 *
 * Segments reported (ns):
 *   syscall_in  = b1  - t0        write() entry -> notify
 *   notify      = b1d - b1        mailbox send cost
 *   fwd_raw     = s1*40 - b1_tick*40      (contains +offset)
 *   turnaround  = (s3 - s1) * 40  firmware receive -> echo send
 *   rev_raw     = b2_tick*40 - s3*40      (contains -offset)
 *   transport   = fwd_raw + rev_raw       (offset-free)
 *   workwait    = b2b - b2        IRQ handler -> workqueue ran
 *   wake        = t1  - b2b       vq callback + ept deliver + wakeup
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include "rpmsg_user.h"

#define TICK_NS 40LL		/* 25 MHz shared timebase */
#define STAMP_MAGIC 0xB47C0DE5U

struct stamped_msg {
	uint32_t data;
	uint32_t magic;
	uint64_t s1_ticks;
	uint64_t s3_ticks;
};

static inline long long now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int env_int(const char *name, int def)
{
	const char *s = getenv(name);

	return (s && *s) ? atoi(s) : def;
}

static int read_probes(unsigned long long p[6])
{
	char buf[256];
	int fd = open("/proc/rpmsg_rtt", O_RDONLY);
	ssize_t n;

	if (fd < 0)
		return -1;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = 0;
	return sscanf(buf, "%llu %llu %llu %llu %llu %llu",
		      &p[0], &p[1], &p[2], &p[3], &p[4], &p[5]) == 6 ? 0 : -1;
}

static int cmp_ll(const void *a, const void *b)
{
	long long x = *(const long long *)a, y = *(const long long *)b;

	return (x > y) - (x < y);
}

static void pstats(const char *name, long long *v, int n)
{
	qsort(v, n, sizeof(long long), cmp_ll);
	fprintf(stderr, "%-12s p50=%7lld p90=%7lld p99=%7lld max=%9lld\n",
		name, v[n / 2], v[(int)(n * 0.90)], v[(int)(n * 0.99)],
		v[n - 1]);
}

int main(void)
{
	struct rpmsg_endpoint_info ept_info;
	int fd = -1, fd_ept = -1;
	int iters = env_int("RPMSG_RTT_ITERS", 10000);
	int warmup = env_int("RPMSG_RTT_WARMUP", 5);
	long long *rtt, *sysin, *notif, *turn, *trans, *wwait, *wake;
	int i, ok = 0;

	rtt   = malloc(sizeof(long long) * iters);
	sysin = malloc(sizeof(long long) * iters);
	notif = malloc(sizeof(long long) * iters);
	turn  = malloc(sizeof(long long) * iters);
	trans = malloc(sizeof(long long) * iters);
	wwait = malloc(sizeof(long long) * iters);
	wake  = malloc(sizeof(long long) * iters);
	if (!rtt || !sysin || !notif || !turn || !trans || !wwait || !wake)
		return 1;

	rpmsg_init_endpoint_info(&ept_info, RPMSG_DEFAULT_EPT_NAME,
				 RPMSG_DEFAULT_SRC_ADDR, RPMSG_DEFAULT_DST_ADDR);
	if (rpmsg_open_endpoint(&ept_info, &fd, &fd_ept) != 0)
		return 1;

	printf("seq,t0_ns,t1_ns,rtt_ns,syscall_in,notify,fwd_raw,turnaround,rev_raw,transport,workwait,wake,residual,status\n");

	for (i = 0; i < warmup + iters; i++) {
		struct stamped_msg rx;
		uint32_t tx = 0;
		unsigned long long p[6];
		long long t0, t1;
		ssize_t wn, rn;

		memset(&rx, 0, sizeof(rx));
		t0 = now_ns();
		wn = write(fd_ept, &tx, sizeof(tx));
		rn = (wn == (ssize_t)sizeof(tx)) ?
			read(fd_ept, &rx, sizeof(rx)) : -1;
		t1 = now_ns();

		if (i < warmup)
			continue;

		if (wn != (ssize_t)sizeof(tx) || rn < (ssize_t)sizeof(rx) ||
		    rx.magic != STAMP_MAGIC || read_probes(p) != 0) {
			printf("%d,%lld,%lld,%lld,,,,,,,,,,err\n",
			       i + 1, t0, t1, t1 - t0);
			fprintf(stderr,
				"iter %d failed (w=%zd r=%zd magic=%x errno=%d)\n",
				i + 1, wn, rn, rx.magic, errno);
			break;
		}

		{
			long long b1 = p[0], b1t = p[1], b1d = p[2];
			long long b2 = p[3], b2t = p[4], b2b = p[5];
			long long v_rtt   = t1 - t0;
			long long v_sysin = b1 - t0;
			long long v_notif = b1d - b1;
			long long v_fwd   = ((long long)rx.s1_ticks - b1t) * TICK_NS;
			long long v_turn  = ((long long)rx.s3_ticks -
					     (long long)rx.s1_ticks) * TICK_NS;
			long long v_rev   = (b2t - (long long)rx.s3_ticks) * TICK_NS;
			long long v_trans = v_fwd + v_rev;
			long long v_wwait = b2b - b2;
			long long v_wake  = t1 - b2b;
			long long v_resid = v_rtt - (v_sysin + v_notif +
					v_trans + v_turn + v_wwait + v_wake);

			rtt[ok] = v_rtt;  sysin[ok] = v_sysin;
			notif[ok] = v_notif; turn[ok] = v_turn;
			trans[ok] = v_trans; wwait[ok] = v_wwait;
			wake[ok] = v_wake; ok++;

			printf("%d,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,ok\n",
			       i + 1, t0, t1, v_rtt, v_sysin, v_notif, v_fwd,
			       v_turn, v_rev, v_trans, v_wwait, v_wake,
			       v_resid);
		}
	}

	if (ok > 0) {
		fprintf(stderr, "samples=%d (ns)\n", ok);
		pstats("rtt", rtt, ok);
		pstats("syscall_in", sysin, ok);
		pstats("notify", notif, ok);
		pstats("transport", trans, ok);
		pstats("turnaround", turn, ok);
		pstats("workwait", wwait, ok);
		pstats("wake", wake, ok);
	}

	rpmsg_close_endpoint(fd, fd_ept);
	return ok > 0 ? 0 : 1;
}
