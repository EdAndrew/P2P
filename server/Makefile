# Define C compiler to be used
CC=gcc

# Define flags to be passed to the C compiler
CFLAGS=-Wall -pedantic -std=gnu99 -g

# Define flags for libraries to be linked when compiling the application
LDFLAGS=-lpthread -lsqlite3

# Define the name of the output program
PROG=napd

# Define the name of the main configuration header
CFG=config.h

# Define the main of the main module
MAIN=main

# Define the name of the threadpool module
TP=thpool

#---------- MAKEFILE -------------------

${PROG}:	${MAIN}.o ${TP}.o
		${CC} ${MAIN}.o ${TP}.o -o ${PROG} ${LDFLAGS}
		rm *.o

${MAIN}.o:	${MAIN}.c ${CFG}
		${CC} ${CFLAGS} -c ${MAIN}.c -o ${MAIN}.o

${TP}.o:	${TP}.c ${TP}.h
		${CC} ${CFLAGS} -c ${TP}.c -o ${TP}.o

clean:
		rm ${PROG} *.o
