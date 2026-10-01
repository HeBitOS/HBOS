/* tests/host/hbos_string_wrap.c — 把内核 string.c 原文件编进宿主测试。
 * 符号改名由 hbos_shim.h 完成；被测的就是真实的 src/lib/string.c。 */
#include "hbos_shim.h"
#include "../../src/lib/string.c"
