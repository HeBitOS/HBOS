extern int hbos_dep_leaf_value(void);
extern int hbos_versioned_value_v1(void);
extern __thread int hbos_versioned_tls_v1;
extern int hbos_ifunc_value(void);
extern void hbos_record_event(int event);

extern void *dlopen(const char *path, int flags);
extern int dlclose(void *handle);

__asm__(".symver hbos_versioned_value_v1,hbos_versioned_value@HBOS_1.0");
__asm__(".symver hbos_versioned_tls_v1,hbos_versioned_tls@HBOS_1.0");

static int root_state;

void hbos_root_dt_init(void) {
    root_state |= 1;
    hbos_record_event(20);
}

static void __attribute__((constructor)) hbos_root_array_init(void) {
    root_state |= 2;
    hbos_record_event(21);
    void *leaf = dlopen("/lib/liblinux_dep_leaf.so", 2);
    if (!leaf || dlclose(leaf) != 0) root_state |= 4;
}

static void __attribute__((destructor)) hbos_root_array_fini(void) {
    hbos_record_event(24);
    root_state &= ~2;
}

void hbos_root_dt_fini(void) {
    hbos_record_event(25);
    root_state &= ~1;
}

int hbos_dep_root_answer(void) {
    return root_state == 3 ? hbos_dep_leaf_value() + 2 : -2000;
}

int hbos_dep_root_versioned(void) {
    return root_state == 3 ? hbos_versioned_value_v1() : -2000;
}

int hbos_dep_root_versioned_tls(void) {
    return root_state == 3 ? hbos_versioned_tls_v1 : -2000;
}

int hbos_dep_root_ifunc(void) {
    return root_state == 3 ? hbos_ifunc_value() : -2000;
}
