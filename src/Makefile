CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -pthread -Iinclude
LDFLAGS ?= -pthread -lm

.DEFAULT_GOAL := all

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
    PLATFORM := linux
    SHARED_FLAGS := -fPIC -shared
    HOST_EXPORT_FLAGS := -rdynamic
    DL_LIBS := -ldl
else ifeq ($(UNAME_S),Darwin)
    PLATFORM := macos
    SHARED_FLAGS := -fPIC -dynamiclib -undefined dynamic_lookup
    HOST_EXPORT_FLAGS := -Wl,-export_dynamic
    DL_LIBS :=
else
    $(error [BUILD] Unsupported operating system: $(UNAME_S))
endif

SRCS = main.c utils/memory.c
OBJS = $(SRCS:.c=.o)
TARGET = main
BACKEND_DIRS := $(shell find platform/$(PLATFORM) -mindepth 2 -maxdepth 2 -type d 2>/dev/null | sort)

backend_key = $(subst /,_,$(1))

-include $(wildcard $(addsuffix /backend.mk,$(BACKEND_DIRS)))

define backend_source
$(if $(BACKEND_SOURCE_$(call backend_key,$(1))),$(1)/$(BACKEND_SOURCE_$(call backend_key,$(1))),$(firstword $(filter-out %/sensors.c,$(wildcard $(1)/*.c))))
endef

define backend_available
$(if $(BACKEND_AVAILABLE_$(call backend_key,$(1))),$(BACKEND_AVAILABLE_$(call backend_key,$(1))),yes)
endef

BACKEND_TARGETS := $(foreach dir,$(BACKEND_DIRS),$(if $(filter yes,$(call backend_available,$(dir))),$(dir)/backend.so))
ALL_BACKEND_TARGETS := $(foreach dir,$(BACKEND_DIRS),$(dir)/backend.so)
BACKEND_HOST_LDLIBS := $(foreach dir,$(BACKEND_DIRS),$(if $(filter yes,$(call backend_available,$(dir))),$(BACKEND_HOST_LDLIBS_$(call backend_key,$(dir)))))
$(if $(VERBOSE),$(foreach dir,$(BACKEND_DIRS),$(if $(filter no,$(call backend_available,$(dir))),$(info [BUILD] Backend $(dir) skipped: $(or $(BACKEND_SKIP_REASON_$(call backend_key,$(dir))),optional dependencies are unavailable)))))

define backend_rule
$(1)/backend.so: $(call backend_source,$(1)) $(wildcard $(1)/backend.mk)
	$$(CC) $$(CFLAGS) $$(BACKEND_CFLAGS_$(call backend_key,$(1))) $$(SHARED_FLAGS) -o $$@ $$< $$(LDFLAGS) $$(BACKEND_LDLIBS_$(call backend_key,$(1)))
endef

$(foreach dir,$(BACKEND_DIRS),$(if $(filter yes,$(call backend_available,$(dir))),$(eval $(call backend_rule,$(dir)))))

all: $(TARGET)

$(TARGET): $(OBJS) $(BACKEND_TARGETS)
	$(CC) $(CFLAGS) $(HOST_EXPORT_FLAGS) -o $@ $(OBJS) $(LDFLAGS) $(BACKEND_HOST_LDLIBS) $(DL_LIBS)

%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) $(OBJS) $(ALL_BACKEND_TARGETS)
	find platform/ utils/ -name "*.o" -delete 2>/dev/null || true

test: all
	@bash tests/run_tests.sh

.PHONY: all clean test
