CFLAGS=-Wall -g -I../dist/include
LDFLAGS=-L../dist/lib
LDLIBS=-logg -ltheoradec

all: streamsplit

clean:
	@rm -f streamsplit streamsplit.o

streamsplit: streamsplit.o

.PHONY: all clean
