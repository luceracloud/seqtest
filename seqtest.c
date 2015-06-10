#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <assert.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <pthread.h>

#define	FLAG_REPLY	(1u << 0)
#define	FLAG_ERROR	(1u << 1)

#define	min(x, y) ((x) < (y) ? (x) : (y))
#define	max(x, y) ((x) > (y) ? (x) : (y))

#ifdef __APPLE__
uint64_t	gethrtime(void) {
	struct timeval tv;
	uint64_t nsec;

	gettimeofday(&tv, NULL);
	nsec = tv.tv_sec * 1000000000ull + tv.tv_usec * 1000ull;
	return (nsec);
}
#endif

uint32_t maxmsg = 8000;
int debug = 0;

/*
 */
typedef struct test_header {
	uint64_t	seqno;
	uint64_t	ts1;	/* senders send time */
	uint64_t	ts2;	/* repliers recv time */
	uint64_t	ts3;	/* repliers send time */
	uint32_t	rdly;	/* reply delay */
	uint16_t	ssz;	/* send size */
	uint16_t	rsz;	/* reply size */
} test_header_t;

typedef struct test {
	int		sock;
	uint64_t	sseqno;
	uint64_t	rseqno;
	uint32_t	rdly_min;	/* ns */
	uint32_t	rdly_max;	/* ns */
	uint32_t	sdly_min;	/* ns */
	uint32_t	sdly_max;	/* ns */
	uint16_t	ssz_min;	/* send size min */
	uint16_t	ssz_max;	/* send size max */
	uint16_t	rsz_min;	/* reply size min */
	uint16_t	rsz_max;	/* reply size max */
	uint32_t	rintvl;		/* reply interval (0 = none) */
	uint64_t	count;		/* num to exchange */
	uint64_t	totlat;	/* total latency */
	uint64_t	replies; /* total replies */
	uint32_t	flags;
	pthread_t	tid;
	struct sockaddr	*addr;
	socklen_t	addrlen;
} test_t;

uint64_t
randtime(void)
{
	uint64_t now, end;
	static uint64_t rtime;

	if (rtime < 1) {
		now = gethrtime();
		for (int i = 0; i < 1U << 20; i++) {
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

void
ndelay(uint32_t nsec)
{
	uint64_t now, end;
	uint64_t rtime;
	end = gethrtime() + nsec;
	rtime = randtime();
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
		 * we don't care about this, just do some work - assume this
		 * takes less than 100 nsec.  This reduces the pressure we
		 * put on gethrtime.
		 */
		for (int i = 0; i < (end - now); i += rtime) {
			random();
		}
	}
}

uint32_t
range(uint32_t minval, uint32_t maxval)
{
	uint32_t val = minval;

	if (maxval > minval) {
		val += (rand() % (maxval - minval));
	}
	return (val);
}

void *
sender(void *arg)
{
	test_t		*t = arg;
	char		*buf, *ptr;
	uint64_t	count = 0;
	int		rv;
	test_header_t	*h;


	buf = malloc(t->ssz_max);

	count = t->count;

	for (int i = 0; count == 0 || (i < count); i++) {

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
		h->ts3 = 0;
		h->ts2 = 0;
		h->ts1 = gethrtime();

		ndelay(sdly);

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
	return (NULL);
}

void *
receiver(void *arg)
{
	test_t		*t = arg;
	char		*buf, *ptr;
	uint32_t	exp;
	uint32_t	nbytes = 0;
	uint64_t	ltime, now;
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
				return (NULL);
			} else if (nbytes < h->rsz) {
				resid = h->rsz - nbytes;
			} else {
				break;
			}
			rv = recv(t->sock, ptr, resid, 0);
			now = gethrtime();
			if (rv < 0) {
				perror("rcvr/recv");
				return (NULL);
			}
			if (rv == 0) {
				fprintf(stderr, "recv closed to soon\n");
				return (NULL);
			}
			nbytes += rv;
			ptr += rv;
		}
		assert(nbytes >= sizeof (*h));
		assert(nbytes >= h->rsz);

		if (h->ts1 < ltime) {
			fprintf(stderr, "ts1 backwards %llu < %llu !!\n",
			    h->ts1, ltime);
		}
		ltime = h->ts1;
		if (h->seqno != t->rseqno) {
			fprintf(stderr,
			    "reply seqno out of order (%llu != %llu)!!\n",
			    h->seqno, t->rseqno);
		}
		t->rseqno++;
		/* if seqno dropped or duplicate, we expect many error msgs */

		/* XXX: we could check timestamps, figure latency, etc. */
		t->totlat = now - ltime;
		t->replies++;

		if (debug)
			write(1, "<", 1);

		nbytes -= h->rsz;
		memmove(buf, buf + h->rsz, nbytes);
		ptr = buf + nbytes;
		if (exp > 0)
			exp--;
	}
	return (NULL);
}

void *
replier(void *arg)
{
	test_t		*t = arg;
	char		*sbuf, *sptr;
	char		*rbuf, *rptr;
	uint32_t	exp;
	uint32_t	nbytes = 0;
	uint64_t	ltime, now;
	test_header_t	*h;
	uint32_t	count, rdly;
	uint16_t	rsz, ssz;
	int		rv;

	rbuf = malloc(t->ssz_max);
	sbuf = malloc(t->rsz_max);
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
				close(t->sock);
				return (NULL);
			} else if (nbytes < h->ssz) {
				resid = h->ssz - nbytes;
			} else {
				break;
			}
			rv = recv(t->sock, rptr, resid, 0);
			now = gethrtime();
			if (rv < 0) {
				perror("replier/recv");
				close(t->sock);
				return (NULL);
			}
			if (rv == 0) {
				close(t->sock);
				return (NULL);
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
				close(t->sock);
				return (NULL);
			}
			nbytes -= rv;
			sptr += rv;
		}
		if (debug) {
			write(1, "+", 1);
		}
	}
	close(t->sock);
}

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
	NULL
};

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
	int mode;
	int nais;
	struct addrinfo **ais;

	ssz_min = ssz_max = rsz_min = rsz_max = sizeof (test_header_t);
	rdly_min = rdly_max = 0;
	sdly_min = sdly_max = 0;
	rintvl = 1;
	nthreads = 1;
	mode = 0;

	/* initialize the timer */
	(void) randtime();

	while ((c = getopt(argc, argv, "o:srd")) != EOF) {
		switch (c) {
		case 'd':
			debug++;
			break;
		case 's':
			mode = 0;
			break;
		case 'r':
			mode = 1;
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
	for (int i = 0; i < nais; i++) {
		struct addrinfo hints;
		int rv;
		char *pstr;
		char *hstr = argv[i + optind];

		memset(&hints, 0, sizeof (hints));
		hints.ai_socktype = SOCK_STREAM;
		if (mode == 1) {
			hints.ai_flags = AI_PASSIVE;
		}
		if ((pstr = strrchr(hstr, ':')) == NULL) {
			fprintf(stderr, "missing port!\n");
			exit(1);
		}
		*pstr++ = 0;
		if (*hstr == '[' && hstr[strlen(hstr)-1] == ']') {
			hstr[strlen(hstr) - 1] = '\0';
			hstr++;
		}
		if ((rv = getaddrinfo(hstr, pstr, &hints, &ais[i])) != 0) {
			printf("failed to resolve %s port %s!: %s\n",
				hstr, pstr, gai_strerror(rv));
			exit(1);
		}
	}
	for (int i = 0, naddrs = 0; i < nais; i++) {
		for (struct addrinfo *ai = ais[i]; ai; ai = ai->ai_next) {
			naddrs++;
		}
	}
	if (mode == 1) {
		nthreads = naddrs;
	}
	addrs = malloc(naddrs * sizeof (struct sockaddr *));
	naddrs = 0;
	for (int i = 0; i < nais; i++) {
		char hbuf[64];
		char pbuf[64];

		for (struct addrinfo *ai = ais[i]; ai; ai = ai->ai_next) {
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
	if (mode == 0) {
		/* one for sender, and one for receiver */
		nthreads *= 2;
	}

	tests = calloc(sizeof (test_t), nthreads);
	for (int i = 0; i < nthreads; i++) {
		test_t *t = &tests[i];

		t->ssz_min = (uint16_t) min(t->ssz_min, maxmsg);
		t->ssz_min = (uint16_t) max(sizeof (test_header_t), t->ssz_min);

		t->ssz_max = (uint16_t) min(t->ssz_max, maxmsg);
		t->ssz_max = (uint16_t) max(t->ssz_min, t->ssz_max);

		t->rsz_min = (uint16_t) min(t->rsz_min, maxmsg);
		t->rsz_min = (uint16_t) max(sizeof (test_header_t), t->rsz_min);

		t->rsz_max = (uint16_t) min(t->rsz_max, maxmsg);
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

		if (mode == 0) {
			t->addr = addrs[(i / 2) % naddrs];
			if ((i % 2) != 0) {
				t->sock = tests[i-1].sock;
			}
		} else {
			t->addr = addrs[i % naddrs];
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
			t->sock = socket(t->addr->sa_family, SOCK_STREAM, 0);
		}
		if (t->sock == -1) {
			perror("socket");
			exit(1);
		}
		if ((mode == 0) && ((i % 2) == 0)) {
			if (connect(t->sock, t->addr, t->addrlen) != 0) {
				perror("connect");
				exit(1);
			}
			pthread_create(&t->tid, NULL, sender, t);

		} else if (mode == 0) {
			pthread_create(&t->tid, NULL, receiver, t);

		} else if (mode == 1) {
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

#if 0
	/* some timing tests to make sure our implementation doesn't suck */
	printf("randtime is %llu\n", (unsigned long long)randtime());
	printf("Before ndelay 1 msec: %llu\n", (unsigned long long)gethrtime());
	ndelay(1000000);
	printf("After ndelay 1 msec: %llu\n", (unsigned long long)gethrtime());
	printf("Before ndelay 1 sec: %llu\n", (unsigned long long)gethrtime());
	ndelay(1000000000);
	printf("After ndelay 1 sec: %llu\n", (unsigned long long)gethrtime());
#endif

	for (int i = 0; i < nthreads; i++) {
		test_t *t = &tests[i];
		pthread_join(t->tid, NULL);
	}
	if (mode == 0) {
		uint64_t totmsgs = 0;
		uint64_t latency = 0;
		for (int i = 0; i < nthreads; i++) {
			test_t *t = &tests[i];
			totmsgs += t->replies;
			latency += t->totlat;
		}
		printf("Got %llu replies in %llu ns\n", totmsgs, latency);
		printf("Avg round trip latency: %llu nsec\n",
			latency / totmsgs);
	}
	return (0);
}
