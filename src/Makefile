CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra
PYTHON ?= python3

.DEFAULT_GOAL := all

all:
	$(MAKE) -C devices all

tests/energy-counter-test: tests/energy_counter_test.c include/energy-counter.h
	$(CC) $(CFLAGS) -o $@ $<

test: tests/energy-counter-test
	./tests/energy-counter-test
	PYTHONDONTWRITEBYTECODE=1 $(PYTHON) -m unittest discover -s tests -p 'test_*.py' -v

clean:
	$(MAKE) -C devices clean
	rm -f tests/energy-counter-test

.PHONY: all test clean
