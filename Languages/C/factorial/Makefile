NTIMES=10
TIME_OUT_LIMIT=4
VARIANCE=5
SLEEP_SECS=60
INPUT=50
export NTIMES

include ../../../config.env

ifndef GCC_PATH
$(error GCC_PATH is not set.)
endif

compile:
	$(GCC_PATH) -pipe -Wall -O3 -fomit-frame-pointer -march=ivybridge factorial.c -o factorial -lm
	
measure:
	sudo modprobe msr
	sudo ../../../RAPL/main "./factorial $(INPUT)" C factorial $(NTIMES) $(VARIANCE) $(TIME_OUT_LIMIT) $(SLEEP_SECS)

Language = C
Program = factorial
command = ./factorial $(INPUT)
perf:
	@echo "Language,Program,Powercap,cycles,instructions,cache-misses,branch-misses,stalled-cycles-frontend,stalled-cycles-backend,resource_stalls.any,mem_trans_retired.load_latency_gt_128,cpu-cycles,uops_executed.stall_cycles" > perf_stat.csv
	@for i in $(shell seq 1 $(NTIMES)); do \
		echo "Run $$i:"; \
		sudo perf stat -e cycles,instructions,cache-misses,branch-misses,stalled-cycles-frontend,stalled-cycles-backend,resource_stalls.any,mem_trans_retired.load_latency_gt_128,cpu-cycles,uops_executed.stall_cycles $(command) 2>&1 | \
		grep -E "cycles|instructions|cache-misses|branch-misses|stalled-cycles-frontend|stalled-cycles-backend|resource_stalls.any|mem_trans_retired.load_latency_gt_128|cpu-cycles|uops_executed.stall_cycles|not supported" | \
		awk '{if ($$1 == "<not") print "not supported"; else print $$1}' | tr '\n' ',' | sed 's/,$$//' | \
		awk -v powercap="$(POWERCAP)" -v language="$(Language)" -v program="$(Program)" '{print language "," program "," powercap "," $$0}' >> perf_stat.csv; \
	done

run:
	./factorial $(INPUT)

mem:
	/usr/bin/time -v ./factorial $(INPUT)

clean:
	rm -f factorial
	rm -f out