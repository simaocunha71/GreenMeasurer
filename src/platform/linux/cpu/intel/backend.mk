BACKEND_SOURCE_platform_linux_cpu_intel := intel.c
BACKEND_CFLAGS_platform_linux_cpu_intel := -I/usr/local/include/raplcap -I/usr/local/include/powercap
BACKEND_HOST_LDLIBS_platform_linux_cpu_intel := -L/usr/local/lib -Wl,--whole-archive -lraplcap-powercap -lpowercap -Wl,--no-whole-archive
