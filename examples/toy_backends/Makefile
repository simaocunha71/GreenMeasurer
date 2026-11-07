# Builds the TOY_CPU and TOY_GPU example backends into build/, laid out the way
# the core expects: <root>/cpu/<name>/backend.so and <root>/gpu/<name>/backend.so.
#
#   make -C examples/toy_backends
#   GREENMEASURER_BACKEND_DIR=$PWD/examples/toy_backends/build ./src/main --list-metrics

CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra
CPPFLAGS += -I../../src/include

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    SHARED_FLAGS := -fPIC -dynamiclib -undefined dynamic_lookup
else
    SHARED_FLAGS := -fPIC -shared
endif

all: build/cpu/toy_cpu/backend.so build/gpu/toy_gpu/backend.so

build/cpu/toy_cpu/backend.so: cpu/toy_cpu/toy_cpu.c ../../src/include/energy-backend.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SHARED_FLAGS) -o $@ $<

build/gpu/toy_gpu/backend.so: gpu/toy_gpu/toy_gpu.c ../../src/include/energy-backend.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SHARED_FLAGS) -o $@ $<

clean:
	rm -rf build

.PHONY: all clean
