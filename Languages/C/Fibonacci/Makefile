n_times ?= 3
time_out_limit ?= 4
variance ?= 5
sleep_secs ?= 10
input ?= 45
powercap ?= -1
export n_times

include ../../../config.env

ifndef GCC_PATH
$(error GCC_PATH is not set.)
endif

compile:
	$(GCC_PATH) -pipe -Wall -O3 -fomit-frame-pointer -march=ivybridge fibonacci.c -o fib -lm
	
measure:
	sudo modprobe msr
	sudo ../../../RAPL/main "./fib $(input)" C fib_$(input) $(n_times) $(variance) $(time_out_limit) $(sleep_secs) $(powercap)

run:
	./fib $(input)

clean:
	rm -f fib
	rm -f out

