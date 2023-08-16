ifeq ($(DEBUG),Y)
	CFLAGS+=-ggdb -O0 -Wall -DDEBUG
else
	CFLAGS+=-Wall
endif

LDFLAGS+=-lpulse -lgpiod

.PHONY: clean all install

all: pasuspendgpio

pasuspendgpio: pasuspendgpio.o
	${CC} -o pasuspendgpio pasuspendgpio.o ${LDFLAGS}

pasuspendgpio.o: pasuspendgpio.c
	${CC} -c pasuspendgpio.c ${CFLAGS}

clean:
	rm -f *.o pasuspendgpio

install: pasuspendgpio
	cp pasuspendgpio /usr/local/bin/
