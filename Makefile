
all:
	$(MAKE) build-`uname`
build-Darwin:
	$(CC) seqtest.c -o seqtest

build-Linux:
	$(CC) -std=gnu99 seqtest.c -o seqtest -D _GNU_SOURCE -D _XOPEN_SOURCE=700  -lrt -lm

build-SunOS:
	$(CC) -std=gnu99 seqtest.c -o seqtest -D __EXTENSIONS__ -D _XOPEN_SOURCE=600 -lnsl -lsocket -lm -lrt -lpthread
 
clean:
	$(RM) seqtest
