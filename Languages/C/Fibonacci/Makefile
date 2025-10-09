n_times ?= 3
time_out_limit ?= 4
variance ?= 5
sleep_secs ?= 10
input ?= 45
powercap ?= -1
language ?= C
program ?= fibonacci
export n_times
command ?= ./fib $(input)
interval ?= 100

include ../../../config.env

ifndef GCC_PATH
$(error GCC_PATH is not set.)
endif

compile:
	$(GCC_PATH) -pipe -Wall -O3 -fomit-frame-pointer -march=ivybridge fibonacci.c -o fib -lm
	
measure:
	sudo modprobe msr
	sudo ../../../RAPL/main "./fib $(input)" C fib_$(input) $(n_times) $(variance) $(time_out_limit) $(sleep_secs) $(powercap)

perf:
	echo "Language,Program,Powercap,cycles,instructions,cache-misses,branch-misses,stalled-cycles-frontend,stalled-cycles-backend,resource_stalls.any,mem_trans_retired.load_latency_gt_128,cpu-cycles,uops_executed.stall_cycles" > perf_stats.csv
	@for i in $(shell seq 1 $(n_times)); do \
		echo "Run $$i:"; \
		sudo perf stat -e cycles,instructions,cache-misses,branch-misses,stalled-cycles-frontend,stalled-cycles-backend,resource_stalls.any,mem_trans_retired.load_latency_gt_128,cpu-cycles,uops_executed.stall_cycles $(command) 2>&1 | \
		grep -E "cycles|instructions|cache-misses|branch-misses|stalled-cycles-frontend|stalled-cycles-backend|resource_stalls.any|mem_trans_retired.load_latency_gt_128|cpu-cycles|uops_executed.stall_cycles|not supported" | \
		awk '{if ($$1 == "<not") print "not supported"; else print $$1}' | tr '\n' ',' | sed 's/,$$//' | \
		awk -v powercap="$(powercap)" -v language="$(language)" -v program="$(program)" '{print language "," program "," powercap "," $$0}' >> perf_stats.csv; \
	done

overtime:
	sudo ../../../RAPL/main --over-time "$(command)" $(interval) $(language) $(program) -powercap $(powercap) -sleep-secs $(sleep_secs) 

run:
	$(command)

clean:
	rm -f fib
	rm -f out