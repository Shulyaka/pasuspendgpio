all: pasuspendgpio

pasuspendgpio: pasuspendgpio.o
	gcc -o pasuspendgpio pasuspendgpio.o -lpulse

pasuspendgpio.o: pasuspendgpio.c
	gcc -c pasuspendgpio.c -Wall -ggdb

clean:
	rm -f *.o pasuspendgpio
