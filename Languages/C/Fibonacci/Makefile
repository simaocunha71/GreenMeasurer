n_times ?= 3
time_out_limit ?= 4
variance ?= 5
sleep_secs ?= 10
input ?= 45
powercap ?= -1
interval_ms ?= 1
language ?= C
program ?= fibonacci
export n_times
command ?= ./fib $(input)

include ../../../config.env

ifndef GCC_PATH
$(error GCC_PATH is not set.)
endif

compile:
	$(GCC_PATH) -pipe -Wall -O3 -fomit-frame-pointer -march=ivybridge fibonacci.c -o fib -lm

generate:
	@test -n "$(CONFIG)" -a -n "$(BUILD_DIR)" || { echo "Set CONFIG and BUILD_DIR."; exit 1; }
	python3 ../../../scripts/generate_config.py "$(CONFIG)" --directory "$(BUILD_DIR)"

run-configured:
	@test -n "$(BUILD_DIR)" || { echo "Set BUILD_DIR."; exit 1; }
	"$(BUILD_DIR)/greenmeasurer"

run:
	$(command)

clean:
	rm -f fib out *.csv
