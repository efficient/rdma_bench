CFLAGS  := -O3 -Wall -Werror -Wno-unused-result -Wno-unused-function
LD      := gcc
LDFLAGS := ${LDFLAGS}

APPS    := test

all: ${APPS}

test: test.o alloc.o
	${LD} -o $@ $^ ${LDFLAGS}

PHONY: clean
clean:
	rm -f *.o ${APPS}
