#ifndef HBOS_USER_LIBC_SYS_MEMFD_H
#define HBOS_USER_LIBC_SYS_MEMFD_H

#define MFD_CLOEXEC       0x0001U
#define MFD_ALLOW_SEALING 0x0002U

int memfd_create(const char *name, unsigned int flags);

#endif
