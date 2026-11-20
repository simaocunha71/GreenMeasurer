# Device libraries: one directory per <os>/<kind>/<vendor>, each with a .c and a .h exposing
# no-argument getters (for example intel_cpu_get_package). To support new hardware or another OS,
# add a directory and a library rule. Libraries whose development headers are missing are skipped.
CC ?= gcc
CFLAGS ?= -O2 -Wall
INCLUDES = -I../include -Ilinux

HEADER_DIRS := /usr/include /usr/local/include
have_header = $(firstword $(foreach dir,$(HEADER_DIRS),$(wildcard $(dir)/$(1))))
NVML_HEADER := $(firstword $(wildcard $(NVML_ROOT)/include/nvml.h /usr/include/nvml.h \
	/usr/local/cuda/include/nvml.h /opt/cuda/include/nvml.h /usr/include/nvidia/gdk/nvml.h))

LIBS = libgm_intel_gpu.a libgm_system.a libgm_intel_cpu.a
ifneq ($(call have_header,sensors/sensors.h),)
LIBS += libgm_intel_cpu_temperature.a
endif
ifneq ($(and $(call have_header,powercap/powercap.h),$(call have_header,raplcap/raplcap.h)),)
LIBS += libgm_intel_cpu_powercap.a
endif
ifneq ($(NVML_HEADER),)
LIBS += libgm_nvidia_gpu.a
linux/gpu/nvidia/nvidia_gpu.o: CFLAGS += -I$(dir $(NVML_HEADER))
endif

all: $(LIBS)
	@echo "Built: $(LIBS)"

libgm_intel_cpu.a: linux/cpu/intel/intel_cpu.o linux/msr.o
libgm_intel_cpu_temperature.a: linux/cpu/intel/intel_cpu_temperature.o
libgm_intel_cpu_powercap.a: linux/cpu/intel/intel_cpu_powercap.o
libgm_intel_gpu.a: linux/gpu/intel/intel_gpu.o linux/msr.o
libgm_nvidia_gpu.a: linux/gpu/nvidia/nvidia_gpu.o
libgm_system.a: linux/system.o

%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(LIBS):
	ar rcs $@ $^

clean:
	find . -name '*.o' -delete
	rm -f *.a

.PHONY: all clean
