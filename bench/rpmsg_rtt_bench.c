// SPDX-License-Identifier: GPL-2.0
/*
 * rpmsg_rtt_bench: RPMsg round-trip latency benchmark (Henry's method).
 *
 * Path measured: userspace write(/dev/rpmsgN) -> virtio vring -> mailbox
 * kick -> C906L ThreadX echo -> vring -> mailbox kick -> userspace read().
 *
 * Time source: CLOCK_MONOTONIC (same base as kernel ktime_get_ns()).
 * Output: CSV on stdout, schema identical to report/current-5.10-x/rtt_smoke.csv:
 *   seq,payload_len,total_len,t0_ns,t1_ns,rtt_ns,rtt_adj_ns,clock_overhead_ns,
 *   remote_turnaround_ns,rx_len,status
 * Summary (p50/p90/p99/min/max) on stderr.
 *
 * Env: RPMSG_RTT_ITERS (default 10000), RPMSG_RTT_WARMUP (default 5,
 * excluded from CSV like the original smoke runs).
 *
 * NOTE: payload fixed at 4 bytes -- the demo firmware ept copies at most
 * sizeof(uint32_t) and drops larger payloads without echoing (would hang
 * the bench). Payload sweeps need a firmware-side change first.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include "rpmsg_user.h"

static inline long long now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* median cost of one now_ns() pair, so rtt_adj = rtt - overhead */
static long long calibrate_clock_overhead(void)
{
	long long d[1001];
	int i, j;

	for (i = 0; i < 1001; i++) {
		long long a = now_ns();
		long long b = now_ns();

		d[i] = b - a;
	}
	/* insertion sort, tiny n */
	for (i = 1; i < 1001; i++) {
		long long v = d[i];

		for (j = i - 1; j >= 0 && d[j] > v; j--)
			d[j + 1] = d[j];
		d[j + 1] = v;
	}
	return d[500];
}

static int env_int(const char *name, int def)
{
	const char *s = getenv(name);

	return (s && *s) ? atoi(s) : def;
}

static int cmp_ll(const void *a, const void *b)
{
	long long x = *(const long long *)a, y = *(const long long *)b;

	return (x > y) - (x < y);
}

int main(void)
{
	struct rpmsg_endpoint_info ept_info;
	int fd = -1, fd_ept = -1;
	int iters = env_int("RPMSG_RTT_ITERS", 10000);
	int warmup = env_int("RPMSG_RTT_WARMUP", 5);
	long long overhead = calibrate_clock_overhead();
	long long *rtt = malloc(sizeof(long long) * iters);
	unsigned int tx = 0, rx = 0;
	int i, ok = 0;

	if (!rtt)
		return 1;

	rpmsg_init_endpoint_info(&ept_info, RPMSG_DEFAULT_EPT_NAME,
				 RPMSG_DEFAULT_SRC_ADDR, RPMSG_DEFAULT_DST_ADDR);
	if (rpmsg_open_endpoint(&ept_info, &fd, &fd_ept) != 0)
		return 1;

	printf("seq,payload_len,total_len,t0_ns,t1_ns,rtt_ns,rtt_adj_ns,clock_overhead_ns,remote_turnaround_ns,rx_len,status\n");

	for (i = 0; i < warmup + iters; i++) {
		long long t0, t1;
		ssize_t wn, rn;

		t0 = now_ns();
		wn = write(fd_ept, &tx, sizeof(tx));
		rn = (wn == (ssize_t)sizeof(tx)) ?
			read(fd_ept, &rx, sizeof(rx)) : -1;
		t1 = now_ns();

		if (i < warmup)
			continue;

		if (wn == (ssize_t)sizeof(tx) && rn == (ssize_t)sizeof(rx)) {
			rtt[ok++] = t1 - t0;
			printf("%d,%zu,%zu,%lld,%lld,%lld,%lld,%lld,0,%zd,ok\n",
			       i + 1, sizeof(tx), sizeof(tx), t0, t1,
			       t1 - t0, t1 - t0 - overhead, overhead, rn);
		} else {
			printf("%d,%zu,%zu,%lld,%lld,0,0,%lld,0,%zd,err\n",
			       i + 1, sizeof(tx), sizeof(tx), t0, t1,
			       overhead, rn);
			fprintf(stderr, "iteration %d failed (w=%zd r=%zd errno=%d)\n",
				i + 1, wn, rn, errno);
			break;
		}
	}

	if (ok > 0) {
		qsort(rtt, ok, sizeof(long long), cmp_ll);
		fprintf(stderr,
			"samples=%d clock_overhead=%lldns\n"
			"min=%lld p50=%lld p90=%lld p99=%lld max=%lld (ns)\n",
			ok, overhead,
			rtt[0], rtt[ok / 2], rtt[(int)(ok * 0.90)],
			rtt[(int)(ok * 0.99)], rtt[ok - 1]);
	}

	rpmsg_close_endpoint(fd, fd_ept);
	free(rtt);
	return ok > 0 ? 0 : 1;
}
