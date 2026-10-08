CC ?= gcc
CFLAGS ?= -O2

# Detect CPU vendor from /proc/cpuinfo
VENDOR := $(shell grep -m1 'vendor_id' /proc/cpuinfo 2>/dev/null | awk '{print $$3}')

ifeq ($(VENDOR),AuthenticAMD)
    # AMD configuration: exclude raplcap and define CPU_AMD
    CPU_FLAGS := -DCPU_AMD
    LDLIBS := -lm
else
    # Intel configuration
    CPU_FLAGS :=
    RAPLCAP_LIBS := $(shell pkg-config --libs --static raplcap-powercap 2>/dev/null)
    ifeq ($(RAPLCAP_LIBS),)
        # Fallback if raplcap-powercap is not installed on the system/cluster
        CPU_FLAGS += -DNO_POWERCAP
        LDLIBS := -lm
    else
        LDLIBS := $(RAPLCAP_LIBS) -lm
    endif
endif

all: main

main: main.c rapl.o sensors.o
	$(CC) $(CFLAGS) $(CPU_FLAGS) -o main main.c rapl.o sensors.o $(LDLIBS)
	chmod 777 main

rapl.o: rapl.c rapl.h
	$(CC) $(CFLAGS) -c rapl.c -lm

sensors.o: sensors.c sensors.h
	$(CC) $(CFLAGS) -c sensors.c -lm

clean:
	rm -rf main main.o rapl.o sensors.o sensors.so