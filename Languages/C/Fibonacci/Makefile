n_times ?= 3
time_out_limit ?= 4
variance ?= 5
sleep_secs ?= 10
input ?= 55
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
	@events=$$(grep -o '"[^"]\+"' ../../../perf_events.json | tr -d '"' | grep -v events | tr '\n' ',' | sed 's/,$$//'); \
	echo "[INFO] perf events to collect: $$events"; \
	\
	echo "Language,Program,Powercap,$$(echo $$events | tr ',' ',')" > perf_stats.csv; \
	\
	for i in $$(seq 1 $(n_times)); do \
		echo "Run $$i:"; \
		results=$$(sudo perf stat --field-separator , -e $$events $(command) 2>&1 | \
			awk -v events_str="$$events" -F, '\
				BEGIN { n = split(events_str, evs, ",") } \
				/^[[:space:]]*[0-9]/ { \
					val=$$1; gsub(/^[[:space:]]+|[[:space:]]+$$/, "", val); \
					event=$$3; gsub(/^cpu_(core|atom)\//, "", event); gsub(/\/$$/, "", event); \
					data[event]=val; \
				} \
				/<not supported>/ { \
					event=$$3; gsub(/^cpu_(core|atom)\//, "", event); gsub(/\/$$/, "", event); \
					data[event]="not supported"; \
				} \
				END { \
					for (i=1; i<=n; i++) { \
						v=data[evs[i]]; \
						if (v == "" || v == 0) v="not supported"; \
						printf "%s,", v; \
					} \
				}' | sed 's/,$$//'); \
		echo "$(language),$(program),$(powercap),$$results" >> perf_stats.csv; \
	done



overtime:
	sudo modprobe msr
	sudo ../../../RAPL/main --over-time "$(command)" $(interval) $(language) $(program) -powercap $(powercap) -sleep-secs $(sleep_secs) -variance $(variance)

run:
	$(command)

clean:
	rm -f fib
	rm -f out