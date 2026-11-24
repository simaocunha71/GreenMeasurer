CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra
PYTHON ?= python3

.DEFAULT_GOAL := all

all:
	$(MAKE) -C devices all

clean:
	$(MAKE) -C devices clean

.PHONY: all clean