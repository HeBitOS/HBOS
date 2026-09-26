# HPT is a required HBOS user-space component. Normal and no-GUI images both
# include it; HBOS_KERNEL_ONLY=1 is the sole supported opt-out for the
# kernel-only profile.
HPT_DIR ?= HPT
HBOS_KERNEL_ONLY ?= 0

ifeq ($(HBOS_KERNEL_ONLY),0)
# External HIVE builds stage hpt.c in APP_DIR and use the generic HAX rule.
# Native HBOS builds (and HBOS_BUNDLE_APPS=0 no-GUI builds) use the pinned
# top-level HPT submodule through the dedicated rule below.
ifeq ($(and $(filter 1,$(HBOS_BUNDLE_APPS)),$(wildcard $(APP_DIR)/hpt.c)),)
# HAX_CFLAGS 通常由 HAX-Compile/mk/hax-apps.mk 定义，但那份文件只在
# HBOS_BUNDLE_APPS=1 时被 include；no-GUI（BUNDLE_APPS=0）走本文件这条
# 专用规则时它是空的，hpt.c 的 <hax.h> 直接找不到头文件。这里补一个同
# 款 fallback（已被定义时 ?= 不生效，两处定义保持一致）。
HAX_CFLAGS ?= $(USER_CFLAGS) -I$(APP_DIR)/include -I$(APP_DIR)/lib -MMD -MP
HPT_HAX := $(BUILD_DIR)/app/hpt.hax
HAX_ALL_BINS += $(HPT_HAX)

.PHONY: hpt-check
hpt-check:
	@test -f "$(HPT_DIR)/app/hpt.c" && \
		test -f "$(HPT_DIR)/app/include/hpt.h" || \
		{ echo "HPT submodule is missing; run: git submodule update --init --recursive" >&2; exit 2; }

$(HPT_HAX): hpt-check $(USER_LIBC_OBJS) | $(BUILD_DIR)
	@mkdir -p $(@D)
	$(CC) -c $(HAX_CFLAGS) -I$(HPT_DIR)/app $(HPT_DIR)/app/hpt.c \
		-o $(BUILD_DIR)/app/hpt.o
	$(LD) $(USER_LDFLAGS) $(USER_LIBC_OBJS) $(BUILD_DIR)/app/hpt.o -o $@
	@echo "✓ hax app: $@ (HPT)"
endif
endif
