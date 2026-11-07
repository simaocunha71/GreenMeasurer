n_times ?= 3
time_out_limit ?= 4
variance ?= 5
sleep_secs ?= 10
input ?= 45
powercap ?= -1
interval_us ?= 1000
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

standard:
	-modprobe msr 2>/dev/null || true
	../../../src/main --standard -command "$(command)" -language $(language) -program $(program)_$(input) -n_times $(n_times) -variance $(variance) -time_out_limit $(time_out_limit) -sleep_time $(sleep_secs) -powercap $(powercap)

overtime:
	-modprobe msr 2>/dev/null || true
	../../../src/main --overtime -command "$(command)" -language $(language) -program $(program) -powercap $(powercap) -sleep_time $(sleep_secs) -variance $(variance) -interval-us $(interval_us)

run:
	$(command)

clean:
	rm -f fib out *.csv
