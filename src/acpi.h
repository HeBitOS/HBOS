#ifndef HBOS_ACPI_H
#define HBOS_ACPI_H

#include <stdint.h>

void acpi_init(void *mbi);
int acpi_poweroff(void);

#endif
