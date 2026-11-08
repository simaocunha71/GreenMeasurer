CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra

adapter.so: adapter.c processes.c processes.h ../../../include/metric-adapter.h
	$(CC) $(CFLAGS) -fPIC -shared -o $@ adapter.c processes.c

.PHONY: all clean
all: adapter.so
clean:
	rm -f adapter.so
