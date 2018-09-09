CFLAGS  := -O3 -Wall -Werror -Wno-unused-result -I ../libhrd/
LD      := gcc -O3
LDFLAGS := ${LDFLAGS} -lrt -lpthread -lnuma -libverbs -lmemcached

APPS    := test

all: ${APPS}

test: ../libhrd/hrd_conn.o ../libhrd/hrd_util.o mica.o city.o test.o
	${LD} -o $@ $^ ${LDFLAGS}

PHONY: clean
clean:
	rm -f *.o ../libhrd/*.o ${APPS}
