UNAME		=$(shell uname)

CFLAGS_COMMON	=-std=gnu99 -Wall -Werror
CFLAGS_Linux	=-D _GNU_SOURCE -D _XOPEN_SOURCE=700
CFLAGS_SunOS	=-D __EXTENSIONS__ -D _XOPEN_SOURCE=600
CFLAGS		+=$(CFLAGS_COMMON) $(CFLAGS_$(UNAME))

LDFLAGS_Linux	=-lrt -lm
LDFLAGS_SunOS	=-lnsl -lsocket -lm -lrt -lpthread
LDFLAGS		+=$(LDFLAGS_$(UNAME))

all: seqtest

seqtest: seqtest.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

clean:
	$(RM) seqtest
