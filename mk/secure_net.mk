# Verified HTTPS and IPv6 transport.
#
# Mbed TLS 4.1.0 and lwIP 2.2.1 are pinned submodules.  Mbed TLS is built
# out-of-tree so generated TF-PSA files and objects never dirty the submodule.

MBEDTLS_DIR := third_party/mbedtls
LWIP_DIR := third_party/lwip
SECURE_BUILD_DIR := $(BUILD_DIR)/secure
MBEDTLS_BUILD_DIR := $(SECURE_BUILD_DIR)/mbedtls

C_SRCS += \
	$(SRC_DIR)/net6.c \
	$(SRC_DIR)/secure_https.c \
	$(SRC_DIR)/net/mbedtls_port.c

LWIP_SRCS := \
	$(LWIP_DIR)/src/core/def.c \
	$(LWIP_DIR)/src/core/inet_chksum.c \
	$(LWIP_DIR)/src/core/init.c \
	$(LWIP_DIR)/src/core/ip.c \
	$(LWIP_DIR)/src/core/mem.c \
	$(LWIP_DIR)/src/core/memp.c \
	$(LWIP_DIR)/src/core/netif.c \
	$(LWIP_DIR)/src/core/pbuf.c \
	$(LWIP_DIR)/src/core/raw.c \
	$(LWIP_DIR)/src/core/stats.c \
	$(LWIP_DIR)/src/core/sys.c \
	$(LWIP_DIR)/src/core/tcp.c \
	$(LWIP_DIR)/src/core/tcp_in.c \
	$(LWIP_DIR)/src/core/tcp_out.c \
	$(LWIP_DIR)/src/core/timeouts.c \
	$(LWIP_DIR)/src/core/ipv6/ethip6.c \
	$(LWIP_DIR)/src/core/ipv6/icmp6.c \
	$(LWIP_DIR)/src/core/ipv6/inet6.c \
	$(LWIP_DIR)/src/core/ipv6/ip6.c \
	$(LWIP_DIR)/src/core/ipv6/ip6_addr.c \
	$(LWIP_DIR)/src/core/ipv6/ip6_frag.c \
	$(LWIP_DIR)/src/core/ipv6/mld6.c \
	$(LWIP_DIR)/src/core/ipv6/nd6.c \
	$(LWIP_DIR)/src/netif/ethernet.c

LWIP_OBJS := $(patsubst $(LWIP_DIR)/src/%.c,$(SECURE_BUILD_DIR)/lwip/%.o,$(LWIP_SRCS))
LWIP_CFLAGS := $(CFLAGS) -I$(SRC_DIR)/net -I$(LWIP_DIR)/src/include

MBEDTLS_CONFIG := $(abspath $(SRC_DIR)/net/mbedtls_config.h)
PSA_CONFIG := $(abspath $(SRC_DIR)/net/psa_crypto_config.h)
MBEDTLS_PORT_HEADER := $(abspath $(SRC_DIR)/net/mbedtls_port.h)
MBEDTLS_SHIM := $(abspath $(SRC_DIR)/net/mbedtls_shim)
MBEDTLS_FREESTANDING_CFLAGS := \
	$(filter-out -MMD -MP,$(CFLAGS)) \
	-I$(MBEDTLS_SHIM) -I$(abspath $(SRC_DIR)) \
	-include $(MBEDTLS_PORT_HEADER)
MBEDTLS_APP_CFLAGS := $(CFLAGS) \
	-I$(MBEDTLS_SHIM) \
	-I$(MBEDTLS_DIR)/include \
	-I$(MBEDTLS_DIR)/tf-psa-crypto/include \
	-I$(MBEDTLS_DIR)/tf-psa-crypto/drivers/builtin/include \
	-DMBEDTLS_CONFIG_FILE='"net/mbedtls_config.h"' \
	-DTF_PSA_CRYPTO_CONFIG_FILE='"net/psa_crypto_config.h"' \
	-include $(SRC_DIR)/net/mbedtls_port.h

MBEDTLS_LIBS := \
	$(MBEDTLS_BUILD_DIR)/library/libmbedtls.a \
	$(MBEDTLS_BUILD_DIR)/library/libmbedx509.a \
	$(MBEDTLS_BUILD_DIR)/tf-psa-crypto/core/libtfpsacrypto.a

SECURE_NET_OBJS := $(LWIP_OBJS) $(MBEDTLS_LIBS)

.PHONY: secure-net-check
secure-net-check:
	@test -f "$(LWIP_DIR)/src/core/ipv6/ip6.c" && \
		test -f "$(MBEDTLS_DIR)/library/ssl_client.c" && \
		test -f "$(MBEDTLS_DIR)/tf-psa-crypto/core/psa_crypto.c" || \
		{ echo "secure networking submodules are missing; run: git submodule update --init --recursive" >&2; exit 2; }

$(SECURE_BUILD_DIR)/lwip/%.o: $(LWIP_DIR)/src/%.c | $(BUILD_DIR)
	@mkdir -p $(@D)
	$(CC) -c $(LWIP_CFLAGS) $< -o $@

$(BUILD_DIR)/net6.o: $(SRC_DIR)/net6.c | $(BUILD_DIR)
	$(CC) -c $(LWIP_CFLAGS) $< -o $@

$(BUILD_DIR)/secure_https.o: $(SRC_DIR)/secure_https.c | $(BUILD_DIR)
	$(CC) -c $(MBEDTLS_APP_CFLAGS) $< -o $@

$(BUILD_DIR)/net/mbedtls_port.o: $(SRC_DIR)/net/mbedtls_port.c | $(BUILD_DIR)
	@mkdir -p $(@D)
	$(CC) -c $(MBEDTLS_APP_CFLAGS) $< -o $@

$(MBEDTLS_LIBS) &: secure-net-check \
		$(SRC_DIR)/net/mbedtls_config.h \
		$(SRC_DIR)/net/psa_crypto_config.h \
		$(SRC_DIR)/net/mbedtls_port.h
	cmake -S "$(MBEDTLS_DIR)" -B "$(MBEDTLS_BUILD_DIR)" \
		-DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF \
		-DUSE_SHARED_MBEDTLS_LIBRARY=OFF \
		-DUSE_STATIC_MBEDTLS_LIBRARY=ON \
		-DMBEDTLS_CONFIG_FILE="$(MBEDTLS_CONFIG)" \
		-DTF_PSA_CRYPTO_CONFIG_FILE="$(PSA_CONFIG)" \
		-DCMAKE_C_FLAGS="$(MBEDTLS_FREESTANDING_CFLAGS)" \
		-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY
	cmake --build "$(MBEDTLS_BUILD_DIR)" --parallel

-include $(LWIP_OBJS:.o=.d)
