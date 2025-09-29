# Makefile for CAB403 Major Project

# ---- Variables ----
# CC is the C compiler we will use.
CC = gcc
# CFLAGS are the flags passed to the compiler.
# -Wall shows all warnings, -g adds debug info.
CFLAGS = -Wall -g

# ---- Targets ----

# [cite_start]The 'all' target is the default and builds all 5 required components. [cite: 134]
all: car controller call internal safety

# [cite_start]Rule for building the 'car' executable. [cite: 135]
# -lpthread links the POSIX threads library.
# -lrt links the real-time library (for shared memory).
car: car.c
	$(CC) $(CFLAGS) -o car car.c -lpthread -lrt

# [cite_start]Rule for building the 'controller' executable. [cite: 136]
# It needs the threads library to handle multiple clients.
controller: controller.c
	$(CC) $(CFLAGS) -o controller controller.c -lpthread

# [cite_start]Rule for building the 'call' executable. [cite: 136]
call: call.c
	$(CC) $(CFLAGS) -o call call.c

# [cite_start]Rule for building the 'internal' executable. [cite: 137]
# It needs the real-time library for shared memory.
internal: internal.c
	$(CC) $(CFLAGS) -o internal internal.c -lrt

# [cite_start]Rule for building the 'safety' executable.[cite: 138]
# It also needs the real-time library.
safety: safety.c
	$(CC) $(CFLAGS) -o safety safety.c -lrt

# [cite_start]A 'clean' target to remove all compiled files. [cite: 139]
clean:
	rm -f car controller call internal safety