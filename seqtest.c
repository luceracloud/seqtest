/*
 * Copyright 2016 Lucera Financial Infrastructures, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use file except in compliance with the License.
 * You may obtain a copy of the license at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * This program is used to stress test TCP connections, verifying that
 * ordering constraints are preserved across a connection.  The intent
 * is to validate correct function of a TCP proxy.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <unistd.h>
#include <assert.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <sys/time.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <math.h>
#include <pthread.h>

#define	FLAG_REPLY	(1u << 0)
#define	FLAG_ERROR	(1u << 1)

#define	min(x, y) ((x) < (y) ? (x) : (y))
#define	max(x, y) ((x) > (y) ? (x) : (y))

static int start_wait = 0;
static int start_ready = 0;
static pthread_cond_t waitcv;
static pthread_cond_t startcv;
static pthread_mutex_t startmx;

typedef struct sample {
	uint64_t	when;
	uint64_t	lat;
	uint16_t	ssz;
	uint16_t	rsz;
} sample_t;

/*
 * Amazing - MacOS X doesn't have a standards conforming version
 * of high resolution timers.
 */
#ifdef __APPLE__
uint64_t
gethrtime(void)
{
	struct timeval tv;
	uint64_t nsec;

	gettimeofday(&tv, NULL);
	nsec = tv.tv_sec * 1000000000ull + tv.tv_usec * 1000ull;
	return (nsec);
}
#elif defined(__linux__)
uint64_t
gethrtime(void)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) != 0) {
		perror("clock_gettime");
		exit(1);
	}
	return (ts.tv_nsec + (ts.tv_sec * 1000000000ull));
}
#endif

/* We probably don't want to exchange messages in excess of this. */
uint32_t maxmsg = 8000;

int debug = 0;

/*
 * Test header, used at the start of every message.
 */
typedef struct test_header {
	uint64_t	seqno;
	uint64_t	ts1;	/* senders send time */
	uint64_t	ts2;	/* repliers recv time */
	uint64_t	ts3;	/* repliers send time */
	uint32_t	rdly;	/* reply delay (ns) */
	uint16_t	ssz;	/* send size */
	uint16_t	rsz;	/* reply size */
} test_header_t;

/*
 * Each thread in the sending system is driven by a single state.
 * This allows us to set up the test, but otherwise each thread runs
 * independent of the others, so we have no locks, nor races.
 */
typedef struct test {
	int		sock;
	uint64_t	sseqno;
	uint64_t	rseqno;
	uint32_t	rdly_min;	/* reply delay (ns) */
	uint32_t	rdly_max;	/* reply delay (ns) */
	uint32_t	sdly_min;	/* interpacket send delay (ns) */
	uint32_t	sdly_max;	/* interpacket send delay (ns) */
	uint16_t	ssz_min;	/* send size min */
	uint16_t	ssz_max;	/* send size max */
	uint16_t	rsz_min;	/* reply size min */
	uint16_t	rsz_max;	/* reply size max */
	uint32_t	rintvl;		/* reply interval (0 = none) */
	uint64_t	count;		/* num to exchange */
	uint64_t	replies;	/* total replies */
	uint32_t	flags;		/* flags */
	pthread_t	tid;		/* pthread processing this test */
	struct sockaddr	*addr;		/* address for the socket */
	struct addrinfo *lai;		/* local addr to bind (client only) */
	socklen_t	addrlen;
	sample_t	*samples;
} test_t;

/*
 * randtime determines the number of nsec used for each call to random().
 * The idea here is that we can use random() as a busy worker to spin.  This
 * will prevent it from being optimized away, and gives us some idea of the
 * involved with each iteration.
 */
uint64_t
randtime(void)
{
	uint64_t now, end;
	static uint64_t rtime;
	int i;

	if (rtime < 1) {
		now = gethrtime();
		for (i = 0; i < 1U << 20; i++) {
			random();
		}
		end = gethrtime();
		rtime = (end - now) >> 20;
	}
	if (rtime < 1) {
		rtime = 1;
	}
	return rtime;
}

int
cmpu64(const void *u1, const void *u2)
{
	return (*(uint64_t *)u1 - *(uint64_t *)u2);
}

double
pctile(uint64_t *samples, size_t nsamples, double pctile)
{
	double i, k;

	i = (nsamples * pctile / 100.0);
	k = ceil(i);

	if (k != i) {
		return (double)samples[(int)k-1];
	}

	k = floor(i);
	return ((samples[(int)k-1]+samples[(int)k])/2.0);
}

/*
 * ndelay waits a given number of nsec.  It does this by sleeping for large
 * values of nsec, but will spin when a smaller delay is required.
 */
void
ndelay(uint32_t nsec)
{
	uint64_t now, end;
	end = gethrtime() + nsec;
	while ((now = gethrtime()) < end) {
		if ((end - now) > 1000000) {
			struct timespec ts;
			ts.tv_sec = 0;
			ts.tv_nsec = end - now;
			/* we'll probably sleep too long, that's ok */
			nanosleep(&ts, NULL);
			continue;
		}
		/*
		 * Do some work, shouldn't take long, but this eases the pressure
		 * we put on gethrtime.
		 */
		random();
	}
}

/*
 * range returns a value chosen at random between a min and a max.  The value
 * is chosen using rand(), so this is not suitable for cryptographic purposes.
 */
uint32_t
range(uint32_t minval, uint32_t maxval)
{
	uint32_t val = minval;

	if (maxval > minval) {
		val += (rand() % (maxval - minval));
	}
	return (val);
}

/*
 * senderreceiver is a pthread worker that sends a single message and expects
 * a reply.
 */
void *
senderreceiver(void *arg)
{
	test_t		*t = arg;
	char		*sbuf, *rbuf, *sptr, *rptr;
	uint64_t	count = 0, stime, now, deltat;
	uint32_t	nbytes = 0;
	int		rv;
	test_header_t	*sh, *rh;
	int		i;

	sbuf = malloc(maxmsg);
	rbuf = malloc(maxmsg);

	pthread_mutex_lock(&startmx);
	start_wait++;
	pthread_cond_signal(&waitcv);
	while (!start_ready) {
		pthread_cond_wait(&startcv, &startmx);
	}
	pthread_mutex_unlock(&startmx);

	rptr = rbuf;
	count = t->count;
	t->rintvl = 1;

	for (i = 0; count == 0 || (i < count); i++) {

		uint16_t ssz, rsz;
		uint32_t sdly, rdly;
		sh = (void *)sbuf;
		sptr = sbuf;

		ssz = (uint16_t) range(t->ssz_min, t->ssz_max);
		rsz = (uint16_t) range(t->rsz_min, t->rsz_max);
		sdly = range(t->sdly_min, t->sdly_min);
		rdly = range(t->rdly_min, t->rdly_min);

		sh->ssz = ssz;
		sh->rsz = (t->rintvl && ((i % t->rintvl) == 0)) ? rsz : 0;
		sh->rdly = sh->rsz ? rdly : 0;
		sh->seqno = t->sseqno++;

		ndelay(sdly);

		stime = gethrtime();
		sh->ts3 = 0;
		sh->ts2 = 0;
		sh->ts1 = stime;

		while (ssz > 0) {
			rv = send(t->sock, sptr, ssz, 0);
			if (rv < 0) {
				perror("sender/send");
				goto out;
			}
			ssz -= rv;
			sptr += rv;
		}
		if (debug)
			write(1, ">", 1);

		rh = (void *)rbuf;
		for (;;) {
			size_t resid;
			if (nbytes < sizeof (*rh)) {
				/* suck in as much as we can */
				resid = maxmsg - sizeof (*rh);
			} else if (rh->rsz > maxmsg) {
				fprintf(stderr, "h->rsz too big\n");
				goto out;
			} else if (nbytes < rh->rsz) {
				resid = rh->rsz - nbytes;
			} else {
				break;
			}
			rv = recv(t->sock, rptr, resid, 0);
			now = gethrtime();
			if (rv < 0) {
				perror("rcvr/recv");
				goto out;
			}
			if (rv == 0) {
				fprintf(stderr, "recv closed to soon\n");
				goto out;
			}
			nbytes += rv;
			rptr += rv;
		}
		assert(nbytes >= sizeof (*rh));
		assert(nbytes >= rh->rsz);

		if (rh->seqno != sh->seqno) {
			fprintf(stderr,
			    "reply seqno out of order (%"
			    PRIu64 " != %" PRIu64 ")!!\n",
			    rh->seqno, sh->seqno);
			goto out;
		}
		if (rh->ts3 < rh->ts2) {
			fprintf(stderr, "negative packet processing cost\n");
			goto out;
		}
		if (rh->ts1 != sh->ts1) {
			fprintf(stderr, "mismatched timestamps: %" PRIu64
			    " != %" PRIu64 "\n", rh->ts1, sh->ts1);
			goto out;
		}
		deltat = (now - rh->ts1) - (rh->ts3 - rh->ts2);
		t->samples[t->rseqno].when = rh->ts1;
		t->samples[t->rseqno].lat = deltat;
		t->samples[t->rseqno].ssz = sh->ssz;
		t->samples[t->rseqno].rsz = rh->rsz;
		t->rseqno++;
		/* if seqno dropped or duplicate, we expect many error msgs */

		t->replies++;

		if (debug)
			write(1, "<", 1);

		nbytes -= rh->rsz;
		memmove(rbuf, rbuf + rh->rsz, nbytes);
		rptr = rbuf + nbytes;
	}

out:
	close(t->sock);
	free(rbuf);
	free(sbuf);
	return (NULL);
}

/*
 * sender is a pthread worker that sends the initial messages.
 */
void *
sender(void *arg)
{
	test_t		*t = arg;
	char		*buf, *ptr;
	uint64_t	count = 0, stime;
	int		rv;
	test_header_t	*h;
	int		i;

	buf = malloc(maxmsg);

	count = t->count;

	for (i = 0; count == 0 || (i < count); i++) {

		uint16_t ssz, rsz;
		uint32_t sdly, rdly;
		h = (void *)buf;
		ptr = buf;

		ssz = (uint16_t) range(t->ssz_min, t->ssz_max);
		rsz = (uint16_t) range(t->rsz_min, t->rsz_max);
		sdly = range(t->sdly_min, t->sdly_min);
		rdly = range(t->rdly_min, t->rdly_min);

		h->ssz = ssz;
		h->rsz = (t->rintvl && ((i % t->rintvl) == 0)) ? rsz : 0;
		h->rdly = h->rsz ? rdly : 0;
		h->seqno = t->sseqno++;

		ndelay(sdly);

		stime = gethrtime();
		h->ts3 = 0;
		h->ts2 = 0;
		h->ts1 = stime;

		while (ssz > 0) {
			rv = send(t->sock, ptr, ssz, 0);
			if (rv < 0) {
				perror("sender/send");
				return (NULL);
			}
			ssz -= rv;
			ptr += rv;
		}
		if (debug)
			write(1, ">", 1);
	}
	free(buf);
	return (NULL);
}

/*
 * receiver is a pthread worker that receives any replies.  It runs in the
 * same process as sender.
 */
void *
receiver(void *arg)
{
	test_t		*t = arg;
	char		*buf, *ptr;
	uint32_t	exp;
	uint32_t	nbytes = 0;
	uint64_t	ltime = 0, now = 0, deltat = 0;
	test_header_t	*h;
	int		rv;

	buf = malloc(maxmsg);
	ptr = buf;
	exp = t->rintvl ? t->count / t->rintvl : 0;

	while (t->count == 0 || (exp > 0)) {
		h = (void *)buf;
		for (;;) {
			size_t resid;
			if (nbytes < sizeof (*h)) {
				/* suck in as much as we can */
				resid = maxmsg - sizeof (*h);
			} else if (h->rsz > maxmsg) {
				fprintf(stderr, "h->rsz too big\n");
				goto out;
			} else if (nbytes < h->rsz) {
				resid = h->rsz - nbytes;
			} else {
				break;
			}
			rv = recv(t->sock, ptr, resid, 0);
			now = gethrtime();
			if (rv < 0) {
				perror("rcvr/recv");
				goto out;
			}
			if (rv == 0) {
				fprintf(stderr, "recv closed to soon\n");
				goto out;
			}
			nbytes += rv;
			ptr += rv;
		}
		assert(nbytes >= sizeof (*h));
		assert(nbytes >= h->rsz);

		if (h->ts1 < ltime) {
			fprintf(stderr, "ts1 backwards %" PRIu64
			    " < %" PRIu64 " !!\n",
			    h->ts1, ltime);
		}
		if (now < ltime) {
			fprintf(stderr, "time-travelling packet\n");
		}
		if (h->ts3 < h->ts2) {
			fprintf(stderr, "negative packet processing cost\n");
		}
		deltat = (now - h->ts1) - (h->ts3 - h->ts2);
		ltime = h->ts1;
		if (h->seqno != t->rseqno) {
			fprintf(stderr,
			    "reply seqno out of order (%" PRIu64
			    " != %" PRIu64 ")!!\n",
			    h->seqno, t->rseqno);
		}
		t->samples[t->rseqno].when = h->ts1;
		t->samples[t->rseqno].lat = deltat;
		t->samples[t->rseqno].ssz = 0;	/* probably of no use here */
		t->samples[t->rseqno].rsz = 0;

		t->rseqno++;
		/* if seqno dropped or duplicate, we expect many error msgs */

		/* XXX: we could check timestamps, figure latency, etc. */
		t->replies++;

		if (debug)
			write(1, "<", 1);

		nbytes -= h->rsz;
		memmove(buf, buf + h->rsz, nbytes);
		ptr = buf + nbytes;
		if (exp > 0)
			exp--;
	}

out:
	free(buf);
	return (NULL);
}

/*
 * replier is a pthread worker that services the initial sent messages,
 * checking them for correctness and optionally sending a reply.  Note that
 * the nature of the reply is driven by the message received, rather than
 * by the test.  This allows this to run mostly configuration free.
 */
void *
replier(void *arg)
{
	test_t		*t = arg;
	char		*sbuf, *sptr;
	char		*rbuf, *rptr;
	uint32_t	nbytes = 0;
	uint64_t	ltime = 0, now = 0;
	test_header_t	*h;
	uint32_t	rdly;
	uint16_t	rsz, ssz;
	int		rv;

	rbuf = malloc(maxmsg);
	sbuf = malloc(maxmsg);
	rptr = rbuf;
	sptr = sbuf;

	for (;;) {
		h = (void *)rbuf;
		for (;;) {
			size_t resid;
			if (nbytes < sizeof (*h)) {
				/* suck in as much as we can */
				resid = t->ssz_max - nbytes;
			} else if (h->ssz > maxmsg) {
				fprintf(stderr, "h->ssz too big\n");
				goto out;
			} else if (nbytes < h->ssz) {
				resid = h->ssz - nbytes;
			} else {
				break;
			}
			rv = recv(t->sock, rptr, resid, 0);
			now = gethrtime();
			if (rv < 0) {
				perror("replier/recv");
				goto out;
			}
			if (rv == 0) {
				goto out;
			}
			rptr += rv;
			nbytes += rv;
		}
		if (debug)
			write(1, "-", 1);
		assert(nbytes >= sizeof (*h));
		assert(nbytes >= h->ssz);

		if (h->ts1 < ltime) {
			fprintf(stderr, "replier: ts1 backwards!!\n");
		}

		ltime = h->ts1;
		rdly = h->rdly;
		rsz = h->rsz;
		ssz = h->ssz;

		if (h->seqno != t->sseqno++) {
			fprintf(stderr, "reply seqno out of order!!\n");
		}
		/* if seqno dropped or duplicate, we expect many error msgs */

		nbytes -= ssz;
		memmove(rbuf, rptr, nbytes);
		rptr = rbuf + nbytes;

		if ((nbytes = rsz) == 0) {
			continue;
		}

		ndelay(h->rdly);

		h = (void *)sbuf;
		sptr = (void *)sbuf;

		h->seqno = t->rseqno++;
		h->ssz = ssz;
		h->rsz = rsz;
		h->ts1 = ltime;
		h->rdly = rdly;
		h->ts2 = now;
		h->ts3 = gethrtime();
		while (nbytes) {
			rv = send(t->sock, sptr, nbytes, 0);
			if (rv < 0) {
				perror("send");
				goto out;
			}
			nbytes -= rv;
			sptr += rv;
		}
		if (debug) {
			write(1, "+", 1);
		}
	}

out:
	close(t->sock);
	free(sbuf);
	free(rbuf);
	free(arg);
	return (NULL);
}

/*
 * acceptor runs in the replier's process, and is reponsible for firing
 * off a replier for each inbound connection.
 */
void *
acceptor(void *arg)
{
	test_t		*t = arg;
	test_t		*newt;
	int s;
	for (;;) {
		socklen_t slen;
		struct sockaddr_storage sa;
		slen = sizeof (sa);
		s  = accept(t->sock, (void *)&sa, &slen);
		if (s < 0) {
			perror("accept");
			close(t->sock);
			return (NULL);
		}
		newt = malloc(sizeof (*newt));
		memcpy(newt, t, sizeof (*newt));
		newt->sock = s;
		newt->tid = 0;
		pthread_create(&newt->tid, NULL, replier, newt);
		pthread_detach(newt->tid);
	}
}

test_t	*tests = NULL;
struct sockaddr **addrs = NULL;
int naddrs;

enum mode {
	MODE_ASYNC_SEND = 0,
	MODE_REPLIER,
	MODE_SYNC_SEND
};

char *myopts[] = {
#define	SMIN		0
	"ssize_min",
#define	SMAX		1
	"ssize_max",
#define	SSIZE		2
	"ssize",
#define	RMIN		3
	"rsize_min",
#define	RMAX		4
	"rsize_max",
#define	RSIZE		5
	"rsize",
#define	THREADS		6
	"threads",
#define	SDELAY		7
	"sdelay",
#define	RDELAY		8
	"rdelay",
#define	SDELAY_MIN	9
	"sdelay_min",
#define	SDELAY_MAX	10
	"sdelay_max",
#define	RDELAY_MIN	11
	"rdelay_min",
#define	RDELAY_MAX	12
	"rdelay_max",
#define	RINTERVAL	13
	"rinterval",
#define COUNT		14
	"count",
#define	DUMPFILE	15
	"dump",
	NULL
};

void
check_ndelay(void)
{
	/* some timing tests to make sure our implementation doesn't suck */
	uint64_t start, finish;
	printf("randtime is %llu\n", (unsigned long long)randtime());
	start = gethrtime();
	ndelay(1000000);
	finish = gethrtime();
	printf("ndelay 1 msec took %llu ns\n", (unsigned long long)(finish - start));
	start = gethrtime();
	ndelay(1000000000);
	finish = gethrtime();
	printf("ndelay 1 sec took %llu ns\n", (unsigned long long)(finish - start));

	start = gethrtime();
	sleep(1);
	finish = gethrtime();
	printf("sleep 1 sec took %llu ns\n", (unsigned long long)(finish - start));

	start = gethrtime();
	usleep(10000);
	finish = gethrtime();
	printf("usleep(10ms) took %llu ns\n", (unsigned long long)(finish - start));
}


/*
 * Parse the local address from addrstr. If one exists, point
 * *local_addr to it and update *addrstr to point to the rest of the
 * address string. If no local address is found then set *local_addr
 * to NULL and leave *addrstr unchanged.
 *
 * E.g.
 *
 * *addrstr	= "192.168.1.115,192.168.1.119:6789"
 *
 * then:
 *
 * *local_addr	= "192.168.1.115"
 * *addstr	= "192.168.1.119:6789"
 */
static void
parse_local_addr(char **addrstr, char **local_addr)
{
	char *delim = strchr(*addrstr, ',');

	if (delim != NULL) {
		/*
		 * There is a local bind address specified. Split it
		 * off from the rest of the address string.
		 */
		*delim = '\0';
		*local_addr = *addrstr;
		*addrstr = delim + 1;
	} else {
		*local_addr = NULL;
	}
}

/*
 * Parse the host and port from addrstr. If the addrstr is well formed
 * then *host will point to the host string and *port to port.
 * Otherwise an error will print to stderr and the program will exit.
 *
 *
 * This function is destrcutive to *addrstr. It is equal to *host upon
 * return.
 *
 * E.g.
 *
 * *addrstr	= "192.168.1.119:6789"
 *
 * then:
 *
 * *host	= "192.168.1.119"
 * *port	= "6789"
 * *addrstr	= "192.168.1.119"
 */
static void
parse_addr(char **addrstr, char **host, char **port)
{
	char *delim = strrchr(*addrstr, ':');

	if (delim == NULL) {
		fprintf(stderr, "no port found: %s\n", *addrstr);
		exit(1);
	}

	/*
	 * Separate host from port.
	 */
	*delim = '\0';
	*host = *addrstr;
	*port = delim + 1;

	/*
	 * Strip IPv6 brackets.
	 */
	if ((**host == '[') && ((*host)[strlen(*host) - 1] == ']')) {
		(*host)[strlen(*host) - 1] = '\0';
		(*host)++;
	}
}

int
main(int argc, char **argv)
{
	int c;
	char *options, *optval;

	uint16_t ssz_min, ssz_max, rsz_min, rsz_max;
	uint32_t rdly_min, rdly_max;
	uint32_t sdly_min, sdly_max;
	uint32_t rintvl;
	uint32_t nthreads;
	uint32_t count;
	enum mode mode;
	int nais;
	struct addrinfo **ais;
	struct addrinfo **lais;
	FILE *dumpfile = NULL;
	uint64_t begin_time, finish_time;
	int i;

	ssz_min = ssz_max = rsz_min = rsz_max = sizeof (test_header_t);
	rdly_min = rdly_max = 0;
	sdly_min = sdly_max = 0;
	rintvl = 1;
	nthreads = 1;
	count = 0;
	mode = MODE_ASYNC_SEND;

	/* initialize the timer */
	(void) randtime();

	while ((c = getopt(argc, argv, "o:srdS")) != EOF) {
		switch (c) {
		case 'd':
			debug++;
			break;
		case 's':
			mode = MODE_ASYNC_SEND;
			break;
		case 'S':
			mode = MODE_SYNC_SEND;
			break;
		case 'r':
			mode = MODE_REPLIER;
			break;
		case 'o':
			options = optarg;
			while (*options != '\0') {
				switch (getsubopt(&options, myopts, &optval)) {
				case SMIN:
					if (optval == NULL) {
						fprintf(stderr, "no value\n");
						exit(1);
					}
					ssz_min = atoi(optval);
					break;
				case SMAX:
					if (optval == NULL) {
						fprintf(stderr, "no value\n");
						exit(1);
					}
					ssz_max = atoi(optval);
					break;
				case SSIZE:
					if (optval == NULL) {
						fprintf(stderr, "no value\n");
						exit(1);
					}
					ssz_max = ssz_min = atoi(optval);
					break;
				case RMIN:
					if (optval == NULL) {
						fprintf(stderr, "no value\n");
						exit(1);
					}
					rsz_min = atoi(optval);
					break;
				case RMAX:
					if (optval == NULL) {
						fprintf(stderr, "no value\n");
						exit(1);
					}
					rsz_max = atoi(optval);
					break;
				case RSIZE:
					if (optval == NULL) {
						fprintf(stderr, "no value\n");
						exit(1);
					}
					rsz_max = rsz_min = atoi(optval);
					break;

				case THREADS:
					if (optval == NULL) {
						fprintf(stderr, "no value\n");
						exit(1);
					}
					nthreads = atoi(optval);
					break;
				case RDELAY_MIN:
					if (optval == NULL) {
						fprintf(stderr, "no value\n");
						exit(1);
					}
					rdly_min = atoi(optval);
					break;
				case RDELAY_MAX:
					if (optval == NULL) {
						fprintf(stderr, "no value\n");
						exit(1);
					}
					rdly_max = atoi(optval);
					break;
				case RDELAY:
					if (optval == NULL) {
						fprintf(stderr, "no value\n");
						exit(1);
					}
					rdly_min = rdly_max = atoi(optval);
					break;
				case SDELAY_MIN:
					if (optval == NULL) {
						fprintf(stderr, "no value\n");
						exit(1);
					}
					sdly_min = atoi(optval);
					break;
				case SDELAY_MAX:
					if (optval == NULL) {
						fprintf(stderr, "no value\n");
						exit(1);
					}
					sdly_max = atoi(optval);
					break;
				case SDELAY:
					if (optval == NULL) {
						fprintf(stderr, "no value\n");
						exit(1);
					}
					sdly_min = sdly_max = atoi(optval);
					break;
				case RINTERVAL:
					if (optval == NULL) {
						fprintf(stderr, "no value\n");
						exit(1);
					}
					rintvl = atoi(optval);
					break;
				case COUNT:
					if (optval == NULL) {
						fprintf(stderr, "no value\n");
						exit(1);
					}
					count = atoi(optval);
					break;
				case DUMPFILE:
					if (optval == NULL) {
						fprintf(stderr, "no value\n");
						exit(1);
					}
					dumpfile = fopen(optval, "w+");
					if (dumpfile == NULL) {
						fprintf(stderr, "open %s: %s\n", optval,
						    strerror(errno));
						exit(1);
					}
					break;
				default:
					fprintf(stderr, "bad option %s\n",
						optval);
					exit(1);
				}
			}
		}
	}

	/* addresses */
	if ((nais = (argc - optind)) == 0)  {
		fprintf(stderr, "no address!\n");
		exit(1);
	}

	ais = malloc(sizeof (struct addrinfo *) * (nais));
	lais = malloc(sizeof (struct addrinfo *) * (nais));

	for (i = 0; i < nais; i++) {
		struct addrinfo hints;
		int rv;
		size_t arglen = strlen(argv[i + optind]);
		char *hstr = malloc((arglen + 1) * sizeof(char));
		char *host;
		char *port;
		char *lhost;

		/*
		 * Make a copy of the original argument to modify in
		 * place for easier host and port extraction.
		 */
		strlcpy(hstr, argv[i + optind], arglen + 1);
		lais[i] = NULL;
		memset(&hints, 0, sizeof (hints));
		hints.ai_socktype = SOCK_STREAM;

		if (mode == MODE_REPLIER) {
			hints.ai_flags = AI_PASSIVE;
		}

		if (mode == MODE_ASYNC_SEND || mode == MODE_SYNC_SEND) {
			parse_local_addr(&hstr, &lhost);
			if (lhost != NULL) {
				/*
				 * Sender has local address; store in
				 * lais. Use ephemeral port because
				 * otherwise each run will have to
				 * wait for TIME-WAIT to expire.
				 */
				if ((rv = getaddrinfo(lhost, 0,
					    &hints, &lais[i])) != 0) {
					printf("failed to resolve %s: %s\n",
					    lhost, gai_strerror(rv));
					exit(1);
				}
			}
		}

		parse_addr(&hstr, &host, &port);

		if ((rv = getaddrinfo(host, port, &hints, &ais[i])) != 0) {
			printf("failed to resolve %s:%s: %s\n",
			    host, port, gai_strerror(rv));
			exit(1);
		}

		free(hstr);
	}

	naddrs = 0;

	for (i = 0; i < nais; i++) {
		struct addrinfo *ai;
		for (ai = ais[i]; ai; ai = ai->ai_next) {
			naddrs++;
		}
	}
	if (mode == MODE_REPLIER) {
		nthreads = naddrs;
	}
	addrs = malloc(naddrs * sizeof (struct sockaddr *));
	naddrs = 0;
	for (i = 0; i < nais; i++) {
		char hbuf[64];
		char pbuf[64];
		struct addrinfo *ai;

		for (ai = ais[i]; ai; ai = ai->ai_next) {
			if (getnameinfo(ai->ai_addr, ai->ai_addrlen, hbuf,
			    sizeof (hbuf), pbuf, sizeof (pbuf),
			    NI_NUMERICHOST | NI_NUMERICSERV)) {
				fprintf(stderr, "numeric host/port fail\n");
				exit(1);
			}
			printf("Address %d: Host %s Port %s\n", naddrs,
			    hbuf, pbuf);
			addrs[naddrs++] = ai->ai_addr;
		}
	}

	if (nthreads == 0) {
		nthreads = naddrs;
	}
	if (mode == MODE_ASYNC_SEND) {
		/* one for sender, and one for receiver */
		nthreads *= 2;
	}

	begin_time = gethrtime();
	tests = calloc(sizeof (test_t), nthreads);

	for (i = 0; i < nthreads; i++) {
		test_t *t = &tests[i];

		t->ssz_min = (uint16_t) min(ssz_min, maxmsg);
		t->ssz_min = (uint16_t) max(sizeof (test_header_t), t->ssz_min);

		t->ssz_max = (uint16_t) min(ssz_max, maxmsg);
		t->ssz_max = (uint16_t) max(t->ssz_min, t->ssz_max);

		t->rsz_min = (uint16_t) min(rsz_min, maxmsg);
		t->rsz_min = (uint16_t) max(sizeof (test_header_t), t->rsz_min);

		t->rsz_max = (uint16_t) min(rsz_max, maxmsg);
		t->rsz_max = (uint16_t) max(t->rsz_min, t->rsz_max);

		t->count = count;
		t->rdly_min = rdly_min;
		t->rdly_max = rdly_max;
		t->sdly_min = sdly_min;
		t->sdly_max = sdly_max;
		t->rintvl = rintvl;
		t->sock = -1;
		t->rseqno = 0;
		t->sseqno = 0;
		t->samples = calloc(count, sizeof (sample_t));

		if (mode == MODE_ASYNC_SEND) {
			t->addr = addrs[(i / 2) % naddrs];
			if ((i % 2) != 0) {
				t->sock = tests[i-1].sock;
			}

		} else {
			t->addr = addrs[i % naddrs];
			t->lai = lais[i % naddrs];
		}
		switch (t->addr->sa_family) {
		case AF_INET:
			t->addrlen = sizeof (struct sockaddr_in);
			break;
		case AF_INET6:
			t->addrlen = sizeof (struct sockaddr_in6);
			break;
		default:
			t->addrlen = 0;
			break;
		}

		if (t->sock < 0) {
			int on = 1;
			t->sock = socket(t->addr->sa_family, SOCK_STREAM, 0);

			if (setsockopt(t->sock, IPPROTO_TCP, TCP_NODELAY,
			    &on, sizeof (on)) != 0)
				perror("setting TCP_NODELAY");

			if (t->lai != NULL) {
				if (bind(t->sock,
				    (struct sockaddr *) t->lai->ai_addr,
				    t->lai->ai_addrlen) == -1) {
					perror("binding sender");
					exit(1);
				}
			}
		}
		if (t->sock == -1) {
			perror("socket");
			exit(1);
		}
		if ((mode == MODE_ASYNC_SEND) && ((i % 2) == 0)) {
			if (connect(t->sock, t->addr, t->addrlen) != 0) {
				perror("connect");
				exit(1);
			}
			pthread_create(&t->tid, NULL, sender, t);

		} else if (mode == MODE_ASYNC_SEND) {
			pthread_create(&t->tid, NULL, receiver, t);

		} else if (mode == MODE_SYNC_SEND) {
			if (connect(t->sock, t->addr, t->addrlen) != 0) {
				perror("connect");
				exit(1);
			}
			pthread_create(&t->tid, NULL, senderreceiver, t);

		} else if (mode == MODE_REPLIER) {
			if (bind(t->sock, t->addr, t->addrlen) < 0) {
				perror("bind");
				exit(1);
			}
			if (listen(t->sock, 128) < 0) {
				perror("listen");
				exit(1);
			}
			pthread_create(&t->tid, NULL, acceptor, t);
		}
	}

#ifdef TIMETEST
	check_ndelay();
#endif
	/* start all threads together */
	if (mode == MODE_SYNC_SEND) {
		pthread_mutex_lock(&startmx);
		while (start_wait < nthreads) {
			pthread_cond_wait(&waitcv, &startmx);
		}
		start_ready = 1;
		pthread_cond_broadcast(&startcv);
		pthread_mutex_unlock(&startmx);
		begin_time = gethrtime();
	}

	for (i = 0; i < nthreads; i++) {
		test_t *t = &tests[i];
		pthread_join(t->tid, NULL);
	}

	finish_time = gethrtime();

	if (mode == MODE_ASYNC_SEND || mode == MODE_SYNC_SEND) {
		uint64_t totmsgs = 0;
		uint64_t latency = 0;
		uint64_t mean = 0;
		uint64_t variance = 0;
		uint64_t *samples;
		uint64_t sampno = 0;
		int i, ii;

		for (i = 0; i < nthreads; i++) {
			test_t *t = &tests[i];
			totmsgs += t->replies;
		}

		samples = calloc(totmsgs, sizeof (uint64_t));

		for (i = 0; i < nthreads; i++) {
			test_t *t = &tests[i];
			for (ii = 0; ii < t->replies; ii++) {
				samples[sampno++] = t->samples[ii].lat;
			}
		}

		qsort(samples, totmsgs, sizeof (uint64_t), cmpu64);

		for (i = 0; i < totmsgs; i++) {
			latency += samples[i];
		}

		mean = latency / totmsgs;
		for (i = 0; i < totmsgs; i++) {
			uint64_t diff = samples[i] - mean;
			variance += diff * diff;
		}
		variance /= totmsgs;

		printf("Received %" PRIu64 " replies\n", totmsgs);
		printf("Time: %.1f us\n", (finish_time - begin_time) / 1000.0);
		printf("ROUND TRIP LATENCY:\n");
		printf("Average:  %.1f us\n", mean / 1000.0);
		printf("Stddev:   %.1f us\n", sqrt((double)variance)/1000.0);
		printf("Median:   %.1f us\n", pctile(samples, totmsgs, 50.0)/1000.0);
		printf("90.0%%ile: %.1f us\n", pctile(samples, totmsgs, 90.0)/1000.0);
		printf("99.0%%ile: %.1f us\n", pctile(samples, totmsgs, 99.0)/1000.0);
		printf("99.9%%ile: %.1f us\n", pctile(samples, totmsgs, 99.9)/1000.0);
		printf("Minimum:  %.1f us\n", samples[0]/1000.0);
		printf("Maximum:  %.1f us\n", samples[totmsgs-1]/1000.0);

		if (dumpfile != NULL) {
			int i, ii;
			fprintf(dumpfile, "# thread time latency rsz ssz\n");
			for (i = 0; i < nthreads; i++) {
				test_t *t = &tests[i];
				for (ii = 0; ii < t->replies; ii++) {
					fprintf(dumpfile,
					    "%d %" PRIu64 " %" PRIu64
					    " %u %u\n",
					    i,
					    t->samples[ii].when - begin_time,
					    t->samples[ii].lat,
					    t->samples[ii].rsz,
					    t->samples[ii].ssz);
				}
			}
			fclose(dumpfile);
		}
	}
	return (0);
}
