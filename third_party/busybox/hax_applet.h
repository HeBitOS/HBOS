#ifndef HBOS_BUSYBOX_HAX_APPLET_H
#define HBOS_BUSYBOX_HAX_APPLET_H

/* Mirrors the minimal HAX_APP metadata block used in
 * third_party/tinycc/tcc.c — avoids pulling in app/include/hax.h's full
 * GUI/SDK surface for these TUI-only programs. Each BusyBox applet gets
 * its own tiny "entry_<name>.c" that declares one of these plus
 * #include "<name>.c" (the vendored, otherwise-unmodified upstream file)
 * and a main() shim calling <name>_main(argc, argv) — see README.md
 * "Why one .hax per applet, not a multi-call binary". */
typedef struct {
    unsigned int magic, kind;
    char name[32];
    char desc[64];
} hbos_hax_meta_t;

#define HBOS_HAX_KIND_TUI 1u

#define HBOS_APPLET_META(varname, appname, appdesc) \
    __attribute__((section(".haxmeta"), used)) \
    static const hbos_hax_meta_t varname = { \
        0x4D584148u, HBOS_HAX_KIND_TUI, appname, appdesc }

#endif
