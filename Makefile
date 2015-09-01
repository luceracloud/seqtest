
all:
	$(MAKE) build-`uname`
build-Darwin:
	$(CC) seqtest.c -o seqtest

build-Linux:
	$(CC) -std=gnu99 seqtest.c -o seqtest -D_GNU_SOURCE -D_XOPEN_SOURCE=700  -lrt -lm

build-SunOS:
	$(CC) -std=gnu99 seqtest.c -o seqtest -D_XOPEN_SOURCE=700 -lnsl -lsocket -lm -lrt -lpthread
 
clean:
	$(RM) seqtest
