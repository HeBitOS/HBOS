#ifndef HBOS_PYTHON_H
#define HBOS_PYTHON_H

#include "cc.h"

void        py_set_gfx(const cc_gfx_t *hooks);
int         py_run_file(const char *path);
const char *py_last_error(void);
int         py_last_error_line(void);
void        tool_python_init(void);

#endif
