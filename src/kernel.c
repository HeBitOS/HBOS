#include "types.h"

typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long uint64_t;

#define VGA ((volatile uint16_t *)0xB8000)
#define VGA_WIDTH 80
#define VGA_HEIGHT 25

static int cursor = 0;
static uint8_t color = 0x0F;

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static void update_cursor(void) {
    outb(0x3D4, 14);
    outb(0x3D5, cursor >> 8);
    outb(0x3D4, 15);
    outb(0x3D5, cursor & 0xFF);
}

static void vga_clear(void) {
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        VGA[i] = (color << 8) | ' ';
    }
    cursor = 0;
    update_cursor();
}

static void vga_scroll(void) {
    for (int i = 0; i < (VGA_HEIGHT - 1) * VGA_WIDTH; i++) {
        VGA[i] = VGA[i + VGA_WIDTH];
    }
    for (int i = (VGA_HEIGHT - 1) * VGA_WIDTH; i < VGA_HEIGHT * VGA_WIDTH; i++) {
        VGA[i] = (color << 8) | ' ';
    }
    cursor -= VGA_WIDTH;
    update_cursor();
}

static void vga_putc(char c) {
    if (c == '\n') {
        cursor = (cursor / VGA_WIDTH + 1) * VGA_WIDTH;
    } else if (c == '\r') {
        cursor = (cursor / VGA_WIDTH) * VGA_WIDTH;
    } else if (c == '\b') {
        if (cursor > 0) {
            cursor--;
            VGA[cursor] = (color << 8) | ' ';
        }
    } else if (c == '\t') {
        cursor = (cursor / 8 + 1) * 8;
    } else {
        VGA[cursor] = (color << 8) | c;
        cursor++;
    }
    
    if (cursor >= VGA_WIDTH * VGA_HEIGHT) {
        vga_scroll();
    }
    update_cursor();
}

static void vga_print(const char *s) {
    while (*s) vga_putc(*s++);
}

static int strcmp(const char *s1, const char *s2) {
    while (*s1 && *s1 == *s2) { s1++; s2++; }
    return *(unsigned char *)s1 - *(unsigned char *)s2;
}

static int strncmp(const char *s1, const char *s2, int n) {
    while (n > 0 && *s1 && *s1 == *s2) { s1++; s2++; n--; }
    return n == 0 ? 0 : *(unsigned char *)s1 - *(unsigned char *)s2;
}

static void poweroff(void) {
    vga_print("\nShutting down HBOS...\n");
    outw(0x604, 0x2000);
    outw(0x4004, 0x3400);
    while(1) __asm__ volatile("cli; hlt");
}

static void reboot(void) {
    vga_print("\nRebooting HBOS...\n");
    outb(0x64, 0xFE);
    while(1) __asm__ volatile("cli; hlt");
}

static const char scancode_map[128] = {
    0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', 0,
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\',
    'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '
};

static const char shift_map[128] = {
    0, 0, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b', 0,
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0,
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|',
    'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' '
};

static int shift_pressed = 0;
static int caps_lock = 0;

static char get_key(void) {
    while (1) {
        uint8_t status = inb(0x64);
        if (status & 1) {
            uint8_t sc = inb(0x60);
            
            if (sc == 0x2A || sc == 0x36) { shift_pressed = 1; continue; }
            if (sc == 0xAA || sc == 0xB6) { shift_pressed = 0; continue; }
            if (sc == 0x3A) { caps_lock = !caps_lock; continue; }
            if (sc & 0x80) continue;
            
            if (sc < 128) {
                char c = scancode_map[sc];
                if (c == 0) continue;
                
                if (c >= 'a' && c <= 'z') {
                    if (shift_pressed ^ caps_lock) {
                        c = c - 'a' + 'A';
                    }
                } else {
                    if (shift_pressed) {
                        c = shift_map[sc];
                    }
                }
                return c;
            }
        }
    }
}

static void show_help(void) {
    vga_print("\nHBOS Commands:\n\n");
    vga_print("  help     - Show this help\n");
    vga_print("  clear    - Clear screen\n");
    vga_print("  version  - Show version\n");
    vga_print("  reboot   - Reboot system\n");
    vga_print("  poweroff - Shutdown system\n");
    vga_print("  echo     - Print text\n");
    vga_print("  color FG BG - Set colors (0-15)\n");
    vga_print("\n");
}

static void show_version(void) {
    vga_print("\n========================================\n");
    vga_print("    HBOS - He Bit OS v0.1\n");
    vga_print("========================================\n\n");
    vga_print("A 64-bit command-line operating system primarily written in C\n");
    vga_print("Architecture: x86_64 (Long Mode)\n");
    vga_print("Boot: GRUB Multiboot\n\n");
    vga_print("by linpinf \n");
}

static char cmd[256];
static int cmd_pos;

static void process_cmd(void) {
    cmd[cmd_pos] = 0;
    
    while (cmd_pos > 0 && cmd[cmd_pos-1] == ' ') cmd[--cmd_pos] = 0;
    while (cmd[0] == ' ') {
        for (int i = 0; cmd[i]; i++) cmd[i] = cmd[i+1];
        cmd_pos--;
    }
    
    if (cmd_pos == 0) return;
    
    if (strcmp(cmd, "help") == 0) {
        show_help();
    } else if (strcmp(cmd, "clear") == 0) {
        vga_clear();
    } else if (strcmp(cmd, "version") == 0) {
        show_version();
    } else if (strcmp(cmd, "reboot") == 0) {
        reboot();
    } else if (strcmp(cmd, "poweroff") == 0 || strcmp(cmd, "shutdown") == 0) {
        poweroff();
    } else if (strncmp(cmd, "echo ", 5) == 0) {
        vga_print(cmd + 5);
        vga_print("\n");
    } else if (strncmp(cmd, "color ", 6) == 0) {
        int fg = 7, bg = 0;
        char *p = cmd + 6;
        fg = 0;
        while (*p >= '0' && *p <= '9') { fg = fg * 10 + (*p - '0'); p++; }
        if (*p == ' ') {
            p++;
            bg = 0;
            while (*p >= '0' && *p <= '9') { bg = bg * 10 + (*p - '0'); p++; }
        }
        if (fg >= 0 && fg < 16 && bg >= 0 && bg < 16) {
            color = (bg << 4) | fg;
            vga_clear();
            vga_print("Color changed\n");
        } else {
            vga_print("Color range: 0-15\n");
        }
    } else {
        vga_print("Unknown command: ");
        vga_print(cmd);
        vga_print("\nType 'help' for commands\n\n");
    }
}

void kmain(void *mbi) {
    (void)mbi;
    
    vga_clear();
    
    color = 0x0E;
    vga_print("========================================\n");
    vga_print("         HBOS - He Bit OS\n");
    vga_print("       64-bit Operating System\n");
    vga_print("========================================\n\n");
    
    color = 0x07;
    vga_print("System started!\n");
    vga_print("Type 'help' for commands\n\n");
    
    while (1) {
        color = 0x0A;
        vga_print("hbos# ");
        color = 0x07;
        
        cmd_pos = 0;
        while (1) {
            char c = get_key();
            if (c == '\n') {
                vga_putc('\n');
                process_cmd();
                break;
            } else if (c == '\b') {
                if (cmd_pos > 0) {
                    cmd_pos--;
                    vga_putc('\b');
                }
            } else if (c >= ' ' && cmd_pos < 255) {
                cmd[cmd_pos++] = c;
                vga_putc(c);
            }
        }
    }
}
