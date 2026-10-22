CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -pthread -Iinclude
LDFLAGS ?= -pthread -lm

UNAME_S := $(shell uname -s)

# Base source files common to all environments
SRCS = main.c sensors/sensors.c

# ========================================================
# 1. LINUX PLATFORM
# ========================================================
ifeq ($(UNAME_S), Linux)
    PLATFORM_DIR = platform/linux

    # CPU Vendor Detection
    CPU_VENDOR := $(shell grep -m1 'vendor_id' /proc/cpuinfo 2>/dev/null | awk '{print $$3}')

    ifeq ($(CPU_VENDOR), GenuineIntel)
        $(info [BUILD] Target CPU: Intel)
        SRCS += $(PLATFORM_DIR)/cpu/intel/intel.c $(PLATFORM_DIR)/cpu/intel/sensors.c
        CFLAGS += -I/usr/local/include/raplcap -I/usr/local/include/powercap
        LDFLAGS += -L/usr/local/lib -lraplcap-powercap -lpowercap
    else ifeq ($(CPU_VENDOR), AuthenticAMD)
        $(info [BUILD] Target CPU: AMD Zen)
        SRCS += $(PLATFORM_DIR)/cpu/amd/amd.c $(PLATFORM_DIR)/cpu/amd/sensors.c
    else
        $(warning [BUILD] Unknown CPU vendor '$(CPU_VENDOR)', compiling Intel backend as default fallback)
        SRCS += $(PLATFORM_DIR)/cpu/intel/intel.c $(PLATFORM_DIR)/cpu/intel/sensors.c
        CFLAGS += -I/usr/local/include/raplcap -I/usr/local/include/powercap
        LDFLAGS += -L/usr/local/lib -lraplcap-powercap -lpowercap
    endif

    # GPU Detection: Dedicated NVIDIA vs Dedicated AMD Radeon vs Integrated
    HAS_NVIDIA := $(shell (grep -qi "10de" /sys/bus/pci/devices/*/vendor 2>/dev/null || [ -e /dev/nvidiactl ] || [ -x /usr/bin/nvidia-smi ] || /usr/bin/lspci 2>/dev/null | grep -qi nvidia) && echo yes)
    HAS_AMD_GPU := $(shell (lspci 2>/dev/null | grep -iE 'vga|3d|display' | grep -i amd || which rocm-smi 2>/dev/null) && echo yes)

    ifeq ($(HAS_NVIDIA), yes)
        $(info [BUILD] Target GPU: Dedicated NVIDIA detected)
        SRCS += $(PLATFORM_DIR)/gpu/nvidia/nvidia.c $(PLATFORM_DIR)/gpu/nvidia/sensors.c
        CFLAGS += -I/usr/include
        LDFLAGS += -lnvidia-ml
    else ifeq ($(HAS_AMD_GPU), yes)
        $(info [BUILD] Target GPU: Dedicated AMD Radeon is not supported)
    else
        $(info [BUILD] Target GPU: Integrated Graphics / CPU Fallback)
    endif

# ========================================================
# 2. MACOS / APPLE SILICON PLATFORM
# ========================================================
else ifeq ($(UNAME_S), Darwin)
    $(error [BUILD] Target OS: macOS / Apple Silicon is not supported)

# ========================================================
# 3. UNSUPPORTED OPERATING SYSTEM
# ========================================================
else
    $(error [BUILD] Unsupported operating system: $(UNAME_S))
endif

OBJS = $(SRCS:.c=.o)
TARGET = main

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	chmod 755 $@

# Pattern rule with automatic nested directory creation for .o files
%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET)
	rm -f $(OBJS)
	find platform/ -name "*.o" -delete 2>/dev/null || true

.PHONY: all clean
