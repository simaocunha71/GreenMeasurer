n_times ?= 3
time_out_limit ?= 4
variance ?= 5
sleep_secs ?= 10
input ?= 35
powercap ?= -1
interval_ms ?= 1
language ?= Python
program ?= fibonacci
export n_times
command ?= python3 fibonacci.py $(input)

compile:
	python3 -m py_compile fibonacci.py

generate:
	@test -n "$(CONFIG)" -a -n "$(BUILD_DIR)" || { echo "Set CONFIG and BUILD_DIR."; exit 1; }
	python3 ../../../scripts/generate_config.py "$(CONFIG)" --directory "$(BUILD_DIR)"

run-configured:
	@test -n "$(BUILD_DIR)" || { echo "Set BUILD_DIR."; exit 1; }
	"$(BUILD_DIR)/greenmeasurer"

run:
	$(command)

clean:
	rm -rf __pycache__ *.csv
