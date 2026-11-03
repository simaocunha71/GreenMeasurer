BACKEND_SOURCE_platform_linux_gpu_nvidia := nvidia.c
NVML_HEADER := $(firstword $(wildcard \
	/usr/include/nvml.h \
	/usr/include/nvidia/gdk/nvml.h \
	/usr/include/nvidia/gpu/nvml.h \
	/usr/include/x86_64-linux-gnu/nvml.h \
	/usr/local/include/nvml.h \
	/usr/local/cuda/include/nvml.h \
	/usr/local/cuda-*/include/nvml.h \
	/opt/cuda/include/nvml.h \
	$(CUDA_HOME)/include/nvml.h \
	$(CUDA_HOME)/targets/*/include/nvml.h \
	$(NVML_ROOT)/include/nvml.h))
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
