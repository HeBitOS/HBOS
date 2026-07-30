#ifndef HBOS_LWIP_ARCH_CC_H
#define HBOS_LWIP_ARCH_CC_H

#include <stdint.h>

#define BYTE_ORDER LITTLE_ENDIAN
#define LWIP_NO_UNISTD_H 1
#define LWIP_NO_CTYPE_H 1
#define SOCKLEN_T_DEFINED 1
typedef long ssize_t;

#define LWIP_PLATFORM_DIAG(x) do { (void)0; } while (0)
void hbos_lwip_assert(const char *message);
#define LWIP_PLATFORM_ASSERT(x) hbos_lwip_assert(x)

uint32_t hbos_lwip_rand(void);
#define LWIP_RAND() hbos_lwip_rand()

typedef unsigned long sys_prot_t;

#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_END
#define PACK_STRUCT_STRUCT __attribute__((packed))
#define PACK_STRUCT_FIELD(x) x

#endif
