n_times ?= 3
time_out_limit ?= 4
variance ?= 5
sleep_secs ?= 10
input ?= 35
powercap ?= -1
interval_us ?= 1000
language ?= Python
program ?= fibonacci
export n_times
command ?= python3 fibonacci.py $(input)

compile:
	python3 -m py_compile fibonacci.py

standard:
	-modprobe msr 2>/dev/null || true
	../../../src/main --standard -command "$(command)" -language $(language) -program $(program)_$(input) -n_times $(n_times) -variance $(variance) -time_out_limit $(time_out_limit) -sleep_time $(sleep_secs) -powercap $(powercap)

overtime:
	-modprobe msr 2>/dev/null || true
	../../../src/main --overtime -command "$(command)" -language $(language) -program $(program) -powercap $(powercap) -sleep_time $(sleep_secs) -variance $(variance) -interval-us $(interval_us)

run:
	$(command)

clean:
	rm -rf __pycache__ *.csv
