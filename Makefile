# This is a Makefile - it works worth GNUmake on Linux, Solaris, and illumos.
# Use of cmake instaed is recommended, but if you lack cmake, this can help.
# To use legacy (non-GNU) make, do "make UNAME=`uname`"

UNAME		=$(shell uname)

# -pthread needed for MT errno
CFLAGS_COMMON	=-std=gnu99 -Wall -Werror -pthread
CFLAGS_Linux	=-D _GNU_SOURCE -D _XOPEN_SOURCE=700
CFLAGS_SunOS	=-D __EXTENSIONS__ -D _XOPEN_SOURCE=600
CFLAGS		+=$(CFLAGS_COMMON) $(CFLAGS_$(UNAME))

LDFLAGS_Linux	=-lrt -lm
LDFLAGS_SunOS	=-lnsl -lsocket -lm -lrt -lpthread
LDFLAGS		+=$(LDFLAGS_$(UNAME))

all: seqtest

seqtest: seqtest.c
	$(CC) $(CFLAGS) seqtest.c -o $@ $(LDFLAGS)

clean:
	$(RM) seqtest
