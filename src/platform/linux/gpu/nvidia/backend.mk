BACKEND_SOURCE_platform_linux_gpu_nvidia := nvidia.c
NVML_HEADER := $(firstword $(wildcard /usr/include/nvml.h /usr/include/nvidia/gdk/nvml.h /usr/local/cuda/include/nvml.h))
NVML_LIBRARY := $(shell ldconfig -p 2>/dev/null | awk '$$1 ~ /^libnvidia-ml[.]so/ { print $$NF; exit }')
BACKEND_AVAILABLE_platform_linux_gpu_nvidia := $(if $(and $(NVML_HEADER),$(NVML_LIBRARY)),yes,no)
BACKEND_CFLAGS_platform_linux_gpu_nvidia := $(if $(NVML_HEADER),-I$(dir $(NVML_HEADER)))
BACKEND_LDLIBS_platform_linux_gpu_nvidia := $(NVML_LIBRARY)
