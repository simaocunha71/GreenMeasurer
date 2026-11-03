BACKEND_SOURCE_platform_linux_cpu_intel := intel.c

INTEL_HEADER_SEARCH_DIRS := /usr/include /usr/local/include
INTEL_LIBRARY_SEARCH_DIRS := /usr/local/lib /usr/local/lib64 /usr/lib /usr/lib64 /usr/lib/x86_64-linux-gnu /lib/x86_64-linux-gnu
POWERCAP_HEADER := $(firstword $(foreach dir,$(INTEL_HEADER_SEARCH_DIRS),$(wildcard $(dir)/powercap/powercap.h)))
RAPLCAP_HEADER := $(firstword $(foreach dir,$(INTEL_HEADER_SEARCH_DIRS),$(wildcard $(dir)/raplcap/raplcap.h)))
POWERCAP_LIBRARY := $(firstword $(foreach dir,$(INTEL_LIBRARY_SEARCH_DIRS),$(wildcard $(dir)/libpowercap.a $(dir)/libpowercap.so)))
RAPLCAP_LIBRARY := $(firstword $(foreach dir,$(INTEL_LIBRARY_SEARCH_DIRS),$(wildcard $(dir)/libraplcap-powercap.a $(dir)/libraplcap-powercap.so)))

BACKEND_AVAILABLE_platform_linux_cpu_intel := $(if $(and $(POWERCAP_HEADER),$(RAPLCAP_HEADER),$(POWERCAP_LIBRARY),$(RAPLCAP_LIBRARY)),yes,no)
BACKEND_SKIP_REASON_platform_linux_cpu_intel := required powercap/raplcap headers or libraries are unavailable
BACKEND_CFLAGS_platform_linux_cpu_intel := -I$(dir $(dir $(POWERCAP_HEADER))) -I$(dir $(dir $(RAPLCAP_HEADER)))
BACKEND_HOST_LDLIBS_platform_linux_cpu_intel := -Wl,--whole-archive $(RAPLCAP_LIBRARY) $(POWERCAP_LIBRARY) -Wl,--no-whole-archive
