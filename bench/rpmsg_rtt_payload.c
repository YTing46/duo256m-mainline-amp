// SPDX-License-Identifier: GPL-2.0
/*
 * rpmsg_rtt_payload: RTT vs payload-size sweep (Henry's payload curve).
 *
 * For each payload size (default 4, then 32..496 step 32 -- 496 is the
 * rpmsg buffer limit: 512B vring buffer minus 16B header), runs
 * warmup+iters echo round trips and records the RTT plus the same
 * decomposition segments as rpmsg_rtt_decomp (kernel /proc/rpmsg_rtt
 * probes + firmware s1/s3 stamps carried in the first 24B of the echo),
 * so the growth with payload can be attributed to a segment.
 *
 * Echo contract (firmware comm_main.c): request of N bytes is echoed
 * back with length max(N, 24); the first 24 bytes are overwritten with
 * the stamp header {DATA+1, magic, s1_ticks, s3_ticks}.
 *
 * Env: RPMSG_RTT_ITERS  iterations per size (default 2000)
 *      RPMSG_RTT_WARMUP warmup per size, excluded (default 5)
 *      RPMSG_RTT_SIZES  comma list overriding the sweep, e.g. "4,128,496"
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
#define MAX_PAYLOAD 496
#define STAMP_LEN 24

struct stamp_hdr {
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

static long long pct(long long *v, int n, double q)
{
	return v[(int)(n * q)];
}

int main(void)
{
	struct rpmsg_endpoint_info ept_info;
	int fd = -1, fd_ept = -1;
	int iters = env_int("RPMSG_RTT_ITERS", 2000);
	int warmup = env_int("RPMSG_RTT_WARMUP", 5);
	int sizes[64], nsizes = 0;
	long long *rtt, *trans, *turn, *wake, *wwait, *sysin, *notif;
	int si;

	const char *sz_env = getenv("RPMSG_RTT_SIZES");
	if (sz_env && *sz_env) {
		char *dup = strdup(sz_env), *tok;

		for (tok = strtok(dup, ","); tok && nsizes < 64;
		     tok = strtok(NULL, ","))
			sizes[nsizes++] = atoi(tok);
	} else {
		int s;

		sizes[nsizes++] = 4;
		for (s = 32; s <= MAX_PAYLOAD; s += 32)
			sizes[nsizes++] = s;
	}

	rtt   = malloc(sizeof(long long) * iters);
	trans = malloc(sizeof(long long) * iters);
	turn  = malloc(sizeof(long long) * iters);
	wake  = malloc(sizeof(long long) * iters);
	wwait = malloc(sizeof(long long) * iters);
	sysin = malloc(sizeof(long long) * iters);
	notif = malloc(sizeof(long long) * iters);
	if (!rtt || !trans || !turn || !wake || !wwait || !sysin || !notif)
		return 1;

	rpmsg_init_endpoint_info(&ept_info, RPMSG_DEFAULT_EPT_NAME,
				 RPMSG_DEFAULT_SRC_ADDR, RPMSG_DEFAULT_DST_ADDR);
	if (rpmsg_open_endpoint(&ept_info, &fd, &fd_ept) != 0)
		return 1;

	printf("seq,payload_len,t0_ns,t1_ns,rtt_ns,syscall_in,notify,transport,turnaround,workwait,wake,status\n");
	fprintf(stderr,
		"%7s %6s | %8s %8s %8s | %8s %8s %8s %8s %8s (p50 ns)\n",
		"payload", "n", "rtt_p50", "rtt_p90", "rtt_p99",
		"sysin", "notify", "transprt", "turnarnd", "hostrx");

	for (si = 0; si < nsizes; si++) {
		int size = sizes[si], i, ok = 0;
		uint8_t tx[MAX_PAYLOAD], rx[MAX_PAYLOAD];
		ssize_t expect = size > STAMP_LEN ? size : STAMP_LEN;

		if (size < 1 || size > MAX_PAYLOAD) {
			fprintf(stderr, "skip invalid size %d\n", size);
			continue;
		}
		memset(tx, 0xA5, sizeof(tx));

		for (i = 0; i < warmup + iters; i++) {
			struct stamp_hdr h;
			unsigned long long p[6];
			long long t0, t1;
			ssize_t wn, rn;

			memset(rx, 0, expect);
			t0 = now_ns();
			wn = write(fd_ept, tx, size);
			rn = (wn == size) ? read(fd_ept, rx, sizeof(rx)) : -1;
			t1 = now_ns();

			if (i < warmup)
				continue;

			memcpy(&h, rx, sizeof(h));
			if (wn != size || rn != expect ||
			    h.magic != STAMP_MAGIC || read_probes(p) != 0) {
				printf("%d,%d,%lld,%lld,%lld,,,,,,,err\n",
				       i + 1, size, t0, t1, t1 - t0);
				fprintf(stderr,
					"size %d iter %d failed (w=%zd r=%zd/%zd magic=%x errno=%d)\n",
					size, i + 1, wn, rn, expect,
					h.magic, errno);
				goto next_size;
			}

			{
				long long b1 = p[0], b1t = p[1], b1d = p[2];
				long long b2 = p[3], b2t = p[4], b2b = p[5];
				long long v_fwd = ((long long)h.s1_ticks - b1t) * TICK_NS;
				long long v_rev = (b2t - (long long)h.s3_ticks) * TICK_NS;

				rtt[ok]   = t1 - t0;
				sysin[ok] = b1 - t0;
				notif[ok] = b1d - b1;
				trans[ok] = v_fwd + v_rev;
				turn[ok]  = ((long long)h.s3_ticks -
					     (long long)h.s1_ticks) * TICK_NS;
				wwait[ok] = b2b - b2;
				wake[ok]  = t1 - b2b;

				printf("%d,%d,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,ok\n",
				       i + 1, size, t0, t1, rtt[ok], sysin[ok],
				       notif[ok], trans[ok], turn[ok],
				       wwait[ok], wake[ok]);
				ok++;
			}
		}
next_size:
		if (ok > 0) {
			qsort(rtt, ok, sizeof(long long), cmp_ll);
			qsort(sysin, ok, sizeof(long long), cmp_ll);
			qsort(notif, ok, sizeof(long long), cmp_ll);
			qsort(trans, ok, sizeof(long long), cmp_ll);
			qsort(turn, ok, sizeof(long long), cmp_ll);
			qsort(wwait, ok, sizeof(long long), cmp_ll);
			qsort(wake, ok, sizeof(long long), cmp_ll);
			fprintf(stderr,
				"%7d %6d | %8lld %8lld %8lld | %8lld %8lld %8lld %8lld %8lld\n",
				size, ok, pct(rtt, ok, 0.5), pct(rtt, ok, 0.9),
				pct(rtt, ok, 0.99), pct(sysin, ok, 0.5),
				pct(notif, ok, 0.5), pct(trans, ok, 0.5),
				pct(turn, ok, 0.5),
				pct(wwait, ok, 0.5) + pct(wake, ok, 0.5));
		}
	}

	rpmsg_close_endpoint(fd, fd_ept);
	return 0;
}
