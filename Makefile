
all:
	$(MAKE) build-`uname`
build-Darwin:
	$(CC) seqtest.c -o seqtest

build-Linux:
	$(CC) seqtest -o seqtest -D_GNU_SOURCE -D_XOPEN_SOURCE=700  -lrt -lm

build-SunOS:
	$(CC) seqtest -o seqtest -lnsl -lsocket -lm
 
clean:
	$(RM) seqtest
