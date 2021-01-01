VERSION = 1.0

BIN = /usr/bin
CC = cc

SRC = src/scroll.c src/utils.c
OBJ = ${SRC:.c=.o}

# DEBUGFLAGS = -g -DDEBUG

XINERAMALIBS = -lXinerama
XINERAMAFLAGS = -DXINERAMA

LIBS = -lm -lX11 -lXinerama -lImlib2
CFLAGS = -std=c99 -D_DEFAULT_SOURCE -Wall -DVERSION=\"${VERSION}\" -DDATE=\""${shell date -R}"\" ${XINERAMAFLAGS} ${DEBUGFLAGS}
LDFLAGS = -s ${LIBS} ${XINERAMALIBS}

.c.o:
	${CC} -c ${CFLAGS} $<

${OBJ}: src/utils.h

scroll: src/scroll.o src/utils.o
	${CC} -o $@ scroll.o utils.o ${LDFLAGS}

clean:
	rm -f scroll.o utils.o scroll

install: scroll
	mkdir -p ${BIN}
	cp -f scroll ${BIN}
	chmod 755 ${BIN}/scroll

uninstall:
	rm -f ${BIN}/scroll

.PHONY: clean install uninstall