# HBOS user apps

Drop new built-in user apps in this directory as `*.c`.

Minimal template:

```c
#include "../user/app.h"
#include "../user/syscall.h"

static int app_main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    hbos_puts("hello\n");
    return 0;
}

HBOS_APP("myapp", "Short description", app_main);
```

Build with `make all`, then run inside HBOS with:

```text
apps
run myapp
```

Use the `hbos_*` functions in `src/user/syscall.h` rather than calling kernel helpers directly. The current runtime is built-in and cooperative; the ABI boundary is already `int 0x80`, so the same app-facing API can later move to real ring3 processes.
