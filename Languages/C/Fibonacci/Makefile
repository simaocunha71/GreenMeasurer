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
	@events_all=$$(grep -o '"[^"]\+"' ../../../perf_events.json | tr -d '"' | grep -v events | tr '\n' ',' | sed 's/,$$//'); \
	echo "[INFO] Events listed in JSON: $$events_all"; \
	\
	events_supported=""; \
	for ev in $$(echo $$events_all | tr ',' ' '); do \
		if perf list 2>/dev/null | grep -w "$$ev" >/dev/null; then \
			events_supported="$$events_supported,$$ev"; \
		else \
			echo "[WARN] Skipping unsupported event: $$ev"; \
		fi; \
	done; \
	events_supported=$${events_supported#,}; \
	echo "[INFO] Supported events that will be measured: $$events_supported"; \
	\
	echo "Language,Program,Powercap,$$(echo $$events_all | tr ',' ',')" > perf_stats.csv; \
	\
	for i in $$(seq 1 $(n_times)); do \
		echo "Run $$i:"; \
		results=$$(sudo perf stat --field-separator , -e $$events_supported $(command) 2>&1 | \
			awk -v events_str="$$events_all" -F, '\
				BEGIN { n = split(events_str, evs, ",") } \
				/^[[:space:]]*[0-9]/ { \
					val=$$1; gsub(/^[[:space:]]+|[[:space:]]+$$/, "", val); \
					event=$$3; gsub(/^[[:space:]]+|[[:space:]]+$$/, "", event); \
					gsub(/^cpu(_(core|atom))?\//, "", event); gsub(/\/$$/, "", event); \
					data[event]=val; \
				} \
				/<not supported>/ { \
					event=$$3; gsub(/^[[:space:]]+|[[:space:]]+$$/, "", event); \
					gsub(/^cpu(_(core|atom))?\//, "", event); gsub(/\/$$/, "", event); \
					data[event]="<not supported>"; \
				} \
				END { \
					for (i=1; i<=n; i++) { \
						v=data[evs[i]]; \
						if (v == "" || v == 0) v="<not supported>"; \
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