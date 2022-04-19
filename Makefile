CC = gcc
LIBS = -ltinfo -lpthread
O = o

PREFIX = /usr/local

OBJ = cpu-speed.${O}

all: cpu-speed

cpu-speed: ${OBJ}
	$(CC) -o $@ ${OBJ} ${LIBS}

install: all
	cp cpu-speed ${PREFIX}/bin

uninstall:
	rm -f ${PREFIX}/bin/cpu-speed

clean:
	rm -f *.${O} cpu-speed

