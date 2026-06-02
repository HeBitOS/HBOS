#ifndef HBOS_X25519_H
#define HBOS_X25519_H

#include <stdint.h>

void x25519_public_key(uint8_t out[32], const uint8_t scalar[32]);
void x25519_shared_secret(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);

#endif
