CFLAGS  := -O3 -Wall -Werror -Wno-unused-result
LD      := gcc
LDFLAGS := ${LDFLAGS} -libverbs -lrt -lpthread -lmemcached -lnuma

APPS    := main

all: ${APPS}

main: hrd_conn.o hrd_util.o main.o
	${LD} -o $@ $^ ${LDFLAGS}

PHONY: clean
clean:
	rm -f *.o ${APPS}
