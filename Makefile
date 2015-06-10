
all:
	$(MAKE) build-`uname`
build-Darwin:
	$(CC) seqtest.c -o seqtest

build-Linux:
	$(CC) seqtest -o seqtest

build-SunOS:
	$(CC) seqtest -o seqtest -lnsl -lsocket
 
clean:
	$(RM) seqtest
