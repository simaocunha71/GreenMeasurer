n_times ?= 20
time_out_limit ?= 4
variance ?= 5
sleep_secs ?= 60
input ?= 40
powercap ?= -1
export n_times

include ../../../config.env

ifndef GCC_PATH
$(error GCC_PATH is not set.)
endif

compile:
	$(GCC_PATH) -pipe -Wall -O3 -fomit-frame-pointer -march=ivybridge factorial.c -o factorial -lm
	
measure:
	sudo modprobe msr
	sudo ../../../RAPL/main "./factorial $(input)" C factorial $(n_times) $(variance) $(time_out_limit) $(sleep_secs) $(powercap)

run:
	./factorial $(input)

clean:
	rm -f factorial
	rm -f out

	