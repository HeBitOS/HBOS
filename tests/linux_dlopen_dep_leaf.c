extern void hbos_record_event(int event);

static int leaf_state;
static __thread int leaf_tls_value = 40;
static __thread int leaf_tls_zero __attribute__((aligned(64)));
__thread int hbos_versioned_tls_v1 = 301;
__thread int hbos_versioned_tls_v2 = 302;

static unsigned short current_cs(void) {
    unsigned short value;
    __asm__ volatile("mov %%cs, %0" : "=r"(value));
    return value;
}

static int hbos_ifunc_ring3_impl(void) {
    return 303;
}

static int hbos_ifunc_ring0_bad(void) {
    return -303;
}

typedef int (*ifunc_fn_t)(void);

static ifunc_fn_t hbos_ifunc_resolver(void) {
    return (current_cs() & 3) == 3 ?
        hbos_ifunc_ring3_impl : hbos_ifunc_ring0_bad;
}

int hbos_ifunc_value(void) __attribute__((ifunc("hbos_ifunc_resolver")));

static int hbos_local_ifunc_impl(void) {
    return 404;
}

static int hbos_local_ifunc_bad(void) {
    return -404;
}

static ifunc_fn_t hbos_local_ifunc_resolver(void) {
    return (current_cs() & 3) == 3 ?
        hbos_local_ifunc_impl : hbos_local_ifunc_bad;
}

static int hbos_local_ifunc(void)
    __attribute__((ifunc("hbos_local_ifunc_resolver")));

int hbos_dep_leaf_local_ifunc(void) {
    return hbos_local_ifunc();
}

void hbos_leaf_dt_init(void) {
    leaf_state |= 1;
    hbos_record_event(10);
}

static void __attribute__((constructor)) hbos_leaf_array_init(void) {
    leaf_state |= 2;
    hbos_record_event(11);
}

static void __attribute__((destructor)) hbos_leaf_array_fini(void) {
    hbos_record_event(14);
    leaf_state &= ~2;
}

void hbos_leaf_dt_fini(void) {
    hbos_record_event(15);
    leaf_state &= ~1;
}

int hbos_dep_leaf_value(void) {
    return leaf_state == 3 ? leaf_tls_value : -1000;
}

int hbos_dep_leaf_bump(void) {
    return leaf_state == 3 ? ++leaf_tls_value : -1000;
}

void hbos_dep_leaf_set(int value) {
    leaf_tls_value = value;
}

int hbos_dep_leaf_zero(void) {
    return leaf_tls_zero;
}

int hbos_dep_leaf_zero_bump(void) {
    return ++leaf_tls_zero;
}

int hbos_versioned_value_v1(void) {
    return 101;
}

int hbos_versioned_value_v2(void) {
    return 202;
}

__asm__(".symver hbos_versioned_value_v1,hbos_versioned_value@HBOS_1.0");
__asm__(".symver hbos_versioned_value_v2,hbos_versioned_value@@HBOS_2.0");
__asm__(".symver hbos_versioned_tls_v1,hbos_versioned_tls@HBOS_1.0");
__asm__(".symver hbos_versioned_tls_v2,hbos_versioned_tls@@HBOS_2.0");
