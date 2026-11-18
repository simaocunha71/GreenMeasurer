CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -pthread -Iinclude
LDFLAGS ?= -pthread -lm

# Static over-time sample capacity (rows). Lower it to shrink the BSS reservation.
OT_MAX_SAMPLES ?=
ifneq ($(OT_MAX_SAMPLES),)
CFLAGS += -DOT_MAX_SAMPLES=$(OT_MAX_SAMPLES)
endif

.DEFAULT_GOAL := all

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
    PLATFORM := linux
    SHARED_FLAGS := -fPIC -shared
    HOST_EXPORT_FLAGS := -rdynamic
    DL_LIBS := -ldl
else
    $(error [BUILD] Unsupported operating system: $(UNAME_S))
endif

SRCS = main.c platform/$(PLATFORM)/memory.c platform/$(PLATFORM)/system_metrics.c metric_adapters/metric_adapter_loader.c
OBJS = $(SRCS:.c=.o)
TARGET = main
BACKEND_DIRS := $(shell find platform/$(PLATFORM) -mindepth 2 -maxdepth 2 -type d 2>/dev/null | sort)

backend_key = $(subst /,_,$(1))

INTEL_HEADER_SEARCH_DIRS := /usr/include /usr/local/include
INTEL_LIBRARY_SEARCH_DIRS := /usr/local/lib /usr/local/lib64 /usr/lib /usr/lib64 /usr/lib/x86_64-linux-gnu /lib/x86_64-linux-gnu
POWERCAP_HEADER := $(firstword $(foreach dir,$(INTEL_HEADER_SEARCH_DIRS),$(wildcard $(dir)/powercap/powercap.h)))
RAPLCAP_HEADER := $(firstword $(foreach dir,$(INTEL_HEADER_SEARCH_DIRS),$(wildcard $(dir)/raplcap/raplcap.h)))
SENSORS_HEADER := $(firstword $(foreach dir,$(INTEL_HEADER_SEARCH_DIRS),$(wildcard $(dir)/sensors/sensors.h)))
POWERCAP_LIBRARY := $(firstword $(foreach dir,$(INTEL_LIBRARY_SEARCH_DIRS),$(wildcard $(dir)/libpowercap.a $(dir)/libpowercap.so)))
RAPLCAP_LIBRARY := $(firstword $(foreach dir,$(INTEL_LIBRARY_SEARCH_DIRS),$(wildcard $(dir)/libraplcap-powercap.a $(dir)/libraplcap-powercap.so)))
SENSORS_LIBRARY := $(firstword \
	$(shell ldconfig -p 2>/dev/null | awk '$$1 ~ /^libsensors[.]so/ { print $$NF; exit }') \
	$(foreach dir,$(INTEL_LIBRARY_SEARCH_DIRS),$(wildcard $(dir)/libsensors.so $(dir)/libsensors.so.*)))
BACKEND_AVAILABLE_platform_linux_cpu_intel := $(if $(and $(POWERCAP_HEADER),$(RAPLCAP_HEADER),$(SENSORS_HEADER),$(POWERCAP_LIBRARY),$(RAPLCAP_LIBRARY),$(SENSORS_LIBRARY)),yes,no)
BACKEND_SKIP_REASON_platform_linux_cpu_intel := required powercap, raplcap or libsensors development files are unavailable
BACKEND_CFLAGS_platform_linux_cpu_intel := -I$(dir $(dir $(POWERCAP_HEADER))) -I$(dir $(dir $(RAPLCAP_HEADER))) -I$(dir $(dir $(SENSORS_HEADER)))
BACKEND_HOST_LDLIBS_platform_linux_cpu_intel := -Wl,--whole-archive $(RAPLCAP_LIBRARY) $(POWERCAP_LIBRARY) -Wl,--no-whole-archive
BACKEND_LDLIBS_platform_linux_cpu_intel := $(SENSORS_LIBRARY)

NVML_HEADER := $(firstword $(wildcard \
	/usr/include/nvml.h /usr/include/nvidia/gdk/nvml.h /usr/include/nvidia/gpu/nvml.h \
	/usr/include/x86_64-linux-gnu/nvml.h /usr/local/include/nvml.h \
	/usr/local/cuda/include/nvml.h /usr/local/cuda-*/include/nvml.h \
	/opt/cuda/include/nvml.h $(CUDA_HOME)/include/nvml.h \
	$(CUDA_HOME)/targets/*/include/nvml.h $(NVML_ROOT)/include/nvml.h))
NVML_LIBRARY := $(firstword \
	$(shell ldconfig -p 2>/dev/null | awk '$$1 ~ /^libnvidia-ml[.]so/ { print $$NF; exit }') \
	$(wildcard /usr/lib/libnvidia-ml.so /usr/lib/libnvidia-ml.so.* \
		/usr/lib64/libnvidia-ml.so /usr/lib64/libnvidia-ml.so.* \
		/usr/lib/*/libnvidia-ml.so /usr/lib/*/libnvidia-ml.so.* \
		/usr/local/lib/libnvidia-ml.so /usr/local/lib/libnvidia-ml.so.* \
		/usr/local/cuda/lib64/libnvidia-ml.so /usr/local/cuda/lib64/libnvidia-ml.so.* \
		/opt/cuda/lib64/libnvidia-ml.so /opt/cuda/lib64/libnvidia-ml.so.* \
		$(CUDA_HOME)/lib64/libnvidia-ml.so $(CUDA_HOME)/lib64/libnvidia-ml.so.* \
		$(CUDA_HOME)/targets/*/lib/libnvidia-ml.so \
		$(NVML_ROOT)/lib/libnvidia-ml.so $(NVML_ROOT)/lib64/libnvidia-ml.so))
BACKEND_AVAILABLE_platform_linux_gpu_nvidia := $(if $(and $(NVML_HEADER),$(NVML_LIBRARY)),yes,no)
BACKEND_CFLAGS_platform_linux_gpu_nvidia := $(if $(NVML_HEADER),-I$(dir $(NVML_HEADER)))
BACKEND_LDLIBS_platform_linux_gpu_nvidia := $(NVML_LIBRARY)
BACKEND_SKIP_REASON_platform_linux_gpu_nvidia := NVML development header or runtime library is unavailable
BACKEND_EXTRA_SOURCES_platform_linux_gpu_intel := platform/linux/gpu/intel/sensors.c

GREENMEASURER_CPUINFO ?= /proc/cpuinfo
GREENMEASURER_PCI_DEVICES ?= /sys/bus/pci/devices

define detect_cpu_vendor
awk -F: -v vendor='$(1)' \
	'$$1 ~ /^[[:space:]]*vendor_id[[:space:]]*$$/ { seen = 1; gsub(/^[[:space:]]+|[[:space:]]+$$/, "", $$2); if ($$2 == vendor) found = 1 } END { if (!seen) exit 2; exit !found }' \
	"$(GREENMEASURER_CPUINFO)"
endef

define detect_pci_gpu_vendor
root="$(GREENMEASURER_PCI_DEVICES)"; errors=0; \
if [ ! -d "$$root" ] || [ ! -r "$$root" ] || [ ! -x "$$root" ]; then \
	printf '[WARNING] Cannot inspect PCI devices in %s\n' "$$root" >&2; exit 2; \
fi; \
for device in "$$root"/*; do \
	[ -e "$$device" ] || continue; \
	if ! vendor=$$(cat "$$device/vendor") || ! class=$$(cat "$$device/class"); then \
		errors=1; continue; \
	fi; \
	if ! printf '%s\n' "$$vendor" | grep -Eq '^0x[[:xdigit:]]{4}$$' || \
	   ! printf '%s\n' "$$class" | grep -Eq '^0x[[:xdigit:]]{6}$$'; then \
		printf '[WARNING] Invalid PCI device information in %s\n' "$$device" >&2; \
		errors=1; continue; \
	fi; \
	if [ "$$vendor" = '$(1)' ] && [ "$${class#0x03}" != "$$class" ]; then exit 0; fi; \
done; \
if [ "$$errors" -ne 0 ]; then exit 2; fi; exit 1
endef

BACKEND_DETECT_platform_linux_cpu_intel = $(call detect_cpu_vendor,GenuineIntel)
BACKEND_DETECT_platform_linux_gpu_intel = $(call detect_pci_gpu_vendor,0x8086)
BACKEND_DETECT_platform_linux_gpu_nvidia = $(call detect_pci_gpu_vendor,0x10de)

define backend_source
$(if $(BACKEND_SOURCE_$(call backend_key,$(1))),$(1)/$(BACKEND_SOURCE_$(call backend_key,$(1))),$(firstword $(filter-out %/sensors.c,$(wildcard $(1)/*.c))))
endef

define backend_available
$(if $(BACKEND_AVAILABLE_$(call backend_key,$(1))),$(BACKEND_AVAILABLE_$(call backend_key,$(1))),yes)
endef

define backend_probe
$(if $(strip $(BACKEND_DETECT_$(call backend_key,$(1)))),$(shell ( $(BACKEND_DETECT_$(call backend_key,$(1))) ) >/dev/null; status=$$?; if [ "$$status" -eq 0 ]; then printf yes; elif [ "$$status" -ne 1 ]; then printf '[WARNING] Hardware detector for %s failed (exit %s).\n' '$(1)' "$$status" >&2; fi),$(warning [BUILD] Backend $(1) skipped: no hardware detector configured))
endef

$(foreach dir,$(BACKEND_DIRS),$(eval BACKEND_DETECTED_$(call backend_key,$(dir)) := $(call backend_probe,$(dir))))

define backend_eligible
$(if $(and $(filter yes,$(call backend_available,$(1))),$(filter yes,$(BACKEND_DETECTED_$(call backend_key,$(1))))),yes,no)
endef

BACKEND_TARGETS := $(foreach dir,$(BACKEND_DIRS),$(if $(filter yes,$(call backend_eligible,$(dir))),$(dir)/backend.so))
ALL_BACKEND_TARGETS := $(foreach dir,$(BACKEND_DIRS),$(dir)/backend.so)
BACKEND_HOST_LDLIBS := $(foreach dir,$(BACKEND_DIRS),$(if $(filter yes,$(call backend_eligible,$(dir))),$(BACKEND_HOST_LDLIBS_$(call backend_key,$(dir)))))
$(if $(VERBOSE),$(foreach dir,$(BACKEND_DIRS),$(if $(filter no,$(call backend_eligible,$(dir))),$(info [BUILD] Backend $(dir) skipped: $(if $(filter yes,$(BACKEND_DETECTED_$(call backend_key,$(dir)))),$(or $(BACKEND_SKIP_REASON_$(call backend_key,$(dir))),optional dependencies are unavailable),hardware not detected)))))

define backend_rule
$(1)/backend.so: $(call backend_source,$(1)) $$(BACKEND_EXTRA_SOURCES_$(call backend_key,$(1))) $(wildcard $(1)/*.h) Makefile include/energy-backend.h include/energy-counter.h
	$$(CC) $$(CFLAGS) $$(BACKEND_CFLAGS_$(call backend_key,$(1))) $$(SHARED_FLAGS) -o $$@ $$< $$(BACKEND_EXTRA_SOURCES_$(call backend_key,$(1))) $$(LDFLAGS) $$(BACKEND_LDLIBS_$(call backend_key,$(1)))
endef

$(foreach dir,$(BACKEND_DIRS),$(if $(filter yes,$(call backend_eligible,$(dir))),$(eval $(call backend_rule,$(dir)))))

all: $(TARGET) metric-adapter-check list-metrics greenmeasurer-engine

$(TARGET): launcher.o list-metrics $(BACKEND_TARGETS)
	$(CC) $(CFLAGS) -o $@ launcher.o

main.o: overtime.inc standard.inc include/energy-backend.h include/metric-guidance.h include/system_metrics.h metric_adapters/metric_adapter_loader.h

tests/legacy-engine.o: main.c overtime.inc standard.inc include/energy-backend.h include/metric-guidance.h include/system_metrics.h metric_adapters/metric_adapter_loader.h
	$(CC) $(CFLAGS) -DGREENMEASURER_TEST_ENGINE -c main.c -o $@

tests/legacy-engine: tests/legacy-engine.o $(filter-out main.o,$(OBJS)) $(BACKEND_TARGETS)
	$(CC) $(CFLAGS) $(HOST_EXPORT_FLAGS) -o $@ tests/legacy-engine.o $(filter-out main.o,$(OBJS)) $(LDFLAGS) $(BACKEND_HOST_LDLIBS) $(DL_LIBS)

list-metrics: list_metrics.c main.c overtime.inc standard.inc include/energy-backend.h include/metric-guidance.h include/system_metrics.h metric_adapters/metric_adapter_loader.h $(filter-out main.o,$(OBJS)) $(BACKEND_TARGETS)
	$(CC) $(CFLAGS) $(HOST_EXPORT_FLAGS) -o $@ list_metrics.c $(filter-out main.o,$(OBJS)) $(LDFLAGS) $(BACKEND_HOST_LDLIBS) $(DL_LIBS)

greenmeasurer-engine: engine.c main.c overtime.inc standard.inc include/energy-backend.h include/metric-guidance.h include/system_metrics.h metric_adapters/metric_adapter_loader.h $(filter-out main.o,$(OBJS)) $(BACKEND_TARGETS)
	$(CC) $(CFLAGS) $(HOST_EXPORT_FLAGS) -o $@ engine.c $(filter-out main.o,$(OBJS)) $(LDFLAGS) $(BACKEND_HOST_LDLIBS) $(DL_LIBS)

metric-adapter-check: metric_adapters/metric_adapter_check.c metric_adapters/metric_adapter_loader.c metric_adapters/metric_adapter_loader.h include/metric-adapter.h include/metric-guidance.h
	$(CC) $(CFLAGS) -o $@ metric_adapters/metric_adapter_check.c metric_adapters/metric_adapter_loader.c $(LDFLAGS) $(DL_LIBS)

print-configured-host-libs:
	@printf '%s\n' '$(BACKEND_HOST_LDLIBS)'

%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) list-metrics greenmeasurer-engine metric-adapter-check $(OBJS) launcher.o tests/legacy-engine.o tests/legacy-engine tests/intel-powercap-test tests/intel-temperature-test tests/nvidia-metric-test tests/intel-gpu-sensors-test tests/intel-gpu-counter-test $(ALL_BACKEND_TARGETS)
	find platform/ -name "*.o" -delete 2>/dev/null || true

ifeq ($(BACKEND_AVAILABLE_platform_linux_cpu_intel),yes)
tests/intel-powercap-test: tests/intel_powercap_test.c platform/linux/cpu/intel/intel.c platform/linux/cpu/intel/intel.h include/energy-backend.h include/energy-counter.h
	$(CC) $(CFLAGS) $(BACKEND_CFLAGS_platform_linux_cpu_intel) -ffunction-sections -fdata-sections -Wl,--gc-sections -o $@ $< $(LDFLAGS) $(SENSORS_LIBRARY)
INTEL_POWERCAP_TEST := tests/intel-powercap-test
tests/intel-temperature-test: tests/intel_temperature_test.c platform/linux/cpu/intel/intel.c platform/linux/cpu/intel/intel.h include/energy-backend.h include/energy-counter.h
	$(CC) $(CFLAGS) $(BACKEND_CFLAGS_platform_linux_cpu_intel) -ffunction-sections -fdata-sections -Wl,--gc-sections -Wl,--wrap=sensors_init -Wl,--wrap=sensors_cleanup -Wl,--wrap=sensors_get_detected_chips -Wl,--wrap=sensors_get_features -Wl,--wrap=sensors_get_label -Wl,--wrap=sensors_get_subfeature -Wl,--wrap=sensors_get_value -o $@ $< $(LDFLAGS)
INTEL_TEMPERATURE_TEST := tests/intel-temperature-test
endif

tests/nvidia-metric-test: tests/nvidia_metric_test.c tests/fixtures/nvml/nvml.h platform/linux/gpu/nvidia/nvidia.c include/energy-backend.h
	$(CC) $(CFLAGS) -Itests/fixtures/nvml -o $@ $< $(LDFLAGS)

tests/intel-gpu-sensors-test: tests/intel_gpu_sensors_test.c platform/linux/gpu/intel/sensors.c platform/linux/gpu/intel/intel.h include/energy-backend.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

tests/intel-gpu-counter-test: tests/intel_gpu_counter_test.c platform/linux/gpu/intel/intel.c platform/linux/gpu/intel/intel.h include/energy-backend.h include/energy-counter.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

test: all tests/legacy-engine tests/nvidia-metric-test tests/intel-gpu-sensors-test tests/intel-gpu-counter-test $(INTEL_POWERCAP_TEST) $(INTEL_TEMPERATURE_TEST)
	@bash tests/run_tests.sh
	@./tests/nvidia-metric-test
	@./tests/intel-gpu-sensors-test
	@./tests/intel-gpu-counter-test
	$(if $(INTEL_POWERCAP_TEST),@./$(INTEL_POWERCAP_TEST),@echo "[TEST] Intel powercap mock tests skipped: development headers unavailable")
	$(if $(INTEL_TEMPERATURE_TEST),@./$(INTEL_TEMPERATURE_TEST),@echo "[TEST] Intel temperature sensor tests skipped: development headers unavailable")
	@python3 -m unittest discover -s tests -p 'test_*.py' -v

.PHONY: all clean test print-configured-host-libs
