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

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void vga_print(const char *s) {
    uint8_t saved_color = color;
    uint8_t cur_color = color;
    while (*s) {
        if (*s == '\\' && *(s+1) == 'x') {
            int h1 = hex_val(*(s+2));
            int h2 = hex_val(*(s+3));
            if (h1 >= 0 && h2 >= 0) {
                cur_color = (h1 << 4) | h2;
                s += 4;
                continue;
            }
        }
        uint8_t old = color;
        color = cur_color;
        vga_putc(*s);
        color = old;
        s++;
    }
    color = saved_color;
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

#define KEY_UP 0x100
#define KEY_DOWN 0x101

static int get_key(void) {
    while (1) {
        uint8_t status = inb(0x64);
        if (status & 1) {
            uint8_t sc = inb(0x60);
            
            if (sc == 0x48) return KEY_UP;
            if (sc == 0x50) return KEY_DOWN;
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

static int in_help = 0;

static void help_cmd(const char *name) {
    if (strcmp(name, "list") == 0) {
        vga_print("Commands: help, clear, version, reboot, poweroff, echo, color, history, clearhistory, search, credits\n");
    } else if (strcmp(name, "help") == 0) {
        vga_print("help - Enter online help mode\n");
        vga_print("Usage: help\n");
    } else if (strcmp(name, "clear") == 0) {
        vga_print("clear - Clear the screen\n");
        vga_print("Usage: clear\n");
    } else if (strcmp(name, "version") == 0) {
        vga_print("version - Show system version\n");
        vga_print("Usage: version\n");
    } else if (strcmp(name, "reboot") == 0) {
        vga_print("reboot - Reboot the system\n");
        vga_print("Usage: reboot\n");
    } else if (strcmp(name, "poweroff") == 0) {
        vga_print("poweroff - Shutdown the system\n");
        vga_print("Usage: poweroff\n");
    } else if (strcmp(name, "echo") == 0) {
        vga_print("echo - Print text to screen\n");
        vga_print("Usage: echo <text>\n");
    } else if (strcmp(name, "color") == 0) {
        vga_print("color - Set text colors\n");
        vga_print("Usage: color <fg> <bg>\n");
        vga_print("Colors: 0-15\n");
    } else if (strcmp(name, "history") == 0) {
        vga_print("history - Show command history\n");
        vga_print("Usage: history\n");
    } else if (strcmp(name, "clearhistory") == 0) {
        vga_print("clearhistory - Clear command history\n");
        vga_print("Usage: clearhistory\n");
    } else if (strcmp(name, "search") == 0) {
        vga_print("search - Search command history\n");
        vga_print("Usage: search <term>\n");
    } else if (strcmp(name, "credits") == 0) {
        vga_print("credits - Show credits\n");
        vga_print("Usage: credits\n");
    } else if (strcmp(name, "exit") == 0) {
        vga_print("exit - Exit help mode\n");
    } else {
        vga_print("Unknown command: ");
        vga_print(name);
        vga_print("\nType 'list' to see all commands\n");
    }
}

static void show_version(void) {
    vga_print("\n========================================\n");
    vga_print("    HBOS - He Bit OS v0.1\n");
    vga_print("========================================\n\n");
    vga_print("A 64-bit command-line operating system primarily written in C\n");
    vga_print("Architecture: x86_64 (Long Mode)\n");
    vga_print("Boot: GRUB Multiboot\n\n");
}

static char cmd[256];
static int cmd_pos;

#define HIST_SIZE 64
static char history[HIST_SIZE][256];
static int hist_count = 0;
static int hist_idx = 0;

static void show_credits(void) {
    vga_print("\\x0Ecredits: \\x07\n");
    vga_print("\\x0Elinpinf\\x07 -- v0.1 building\n");
    vga_print("\\x0CPCJKL(aaamemz)\\x07 -- show the video\n");
    vga_print("\\x0ANuclear weapons\\x07 -- Members\n");
    vga_print("\\x09Future community members\\x07\n");
    vga_print("\\x0EThanks everyone\\x07\n\n");
}

static void show_help(void) {
    in_help = 1;
    vga_print("\n\\x0EHBOS Online Help Mode\\x07\n");
    vga_print("Type command name for detailed help (e.g., 'help')\n");
    vga_print("Type 'list' to see all commands\n");
    vga_print("Type 'exit' or 'quit' to exit help mode\n\n");
    
    while (in_help) {
        vga_print("help> ");
        
        cmd_pos = 0;
        while (1) {
            int c = get_key();
            if (c == '\n') {
                vga_putc('\n');
                cmd[cmd_pos] = 0;
                
                while (cmd_pos > 0 && cmd[cmd_pos-1] == ' ') cmd[--cmd_pos] = 0;
                while (cmd[0] == ' ') {
                    for (int i = 0; cmd[i]; i++) cmd[i] = cmd[i+1];
                    cmd_pos--;
                }
                
                if (cmd_pos == 0) break;
                
                if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "quit") == 0 || strcmp(cmd, "q") == 0) {
                    in_help = 0;
                    vga_print("Exiting help mode\n\n");
                    return;
                }
                
                help_cmd(cmd);
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

static void show_history(void) {
    vga_print("\\x0ECommand History (");
    char buf[16];
    int i = 0;
    int t = hist_count;
    while (t > 0) { buf[i++] = '0' + (t % 10); t /= 10; }
    if (i == 0) buf[i++] = '0';
    while (i > 0) vga_putc(buf[--i]);
    vga_print("/");
    i = 0;
    t = HIST_SIZE;
    while (t > 0) { buf[i++] = '0' + (t % 10); t /= 10; }
    if (i == 0) buf[i++] = '0';
    while (i > 0) vga_putc(buf[--i]);
    vga_print("):\\x07\n\n");
    
    for (int j = 0; j < hist_count; j++) {
        vga_print("  ");
        i = 0;
        t = j + 1;
        while (t > 0) { buf[i++] = '0' + (t % 10); t /= 10; }
        if (i == 0) buf[i++] = '0';
        while (i > 0) vga_putc(buf[--i]);
        vga_print(". ");
        vga_print(history[j]);
        vga_print("\n");
    }
    vga_print("\n");
}

static void clear_history(void) {
    hist_count = 0;
    hist_idx = 0;
    vga_print("\\x0ECommand history cleared\\x07\n");
}

static void search_history(const char *term) {
    if (term[0] == 0) {
        vga_print("Usage: search <term>\\n");
        return;
    }
    
    vga_print("\\x0ESearching for: \\x07");
    vga_print(term);
    vga_print("\\n\\n");
    
    int found = 0;
    for (int j = 0; j < hist_count; j++) {
        const char *pos = history[j];
        while (*pos) {
            const char *p1 = pos;
            const char *p2 = term;
            while (*p1 && *p2 && *p1 == *p2) { p1++; p2++; }
            if (*p2 == 0) {
                vga_print("  ");
                char buf[16];
                int i = 0;
                int t = j + 1;
                while (t > 0) { buf[i++] = '0' + (t % 10); t /= 10; }
                if (i == 0) buf[i++] = '0';
                while (i > 0) vga_putc(buf[--i]);
                vga_print(". ");
                vga_print(history[j]);
                vga_print("\\n");
                found++;
                break;
            }
            pos++;
        }
    }
    
    if (found == 0) {
        vga_print("No matching commands found\\n");
    } else {
        vga_print("\\nFound ");
        char buf[16];
        int i = 0;
        int t = found;
        while (t > 0) { buf[i++] = '0' + (t % 10); t /= 10; }
        if (i == 0) buf[i++] = '0';
        while (i > 0) vga_putc(buf[--i]);
        vga_print(" matches\\n");
    }
}

static void process_cmd(void) {
    cmd[cmd_pos] = 0;
    
    while (cmd_pos > 0 && cmd[cmd_pos-1] == ' ') cmd[--cmd_pos] = 0;
    while (cmd[0] == ' ') {
        for (int i = 0; cmd[i]; i++) cmd[i] = cmd[i+1];
        cmd_pos--;
    }
    
    if (cmd_pos == 0) return;
    
    if (in_help) {
        if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "quit") == 0 || strcmp(cmd, "q") == 0) {
            in_help = 0;
            vga_print("Exiting help mode\n\n");
            return;
        }
        help_cmd(cmd);
        return;
    }
    
    if (hist_count < HIST_SIZE) {
        for (int i = 0; i <= cmd_pos; i++) history[hist_count][i] = cmd[i];
        hist_count++;
    } else {
        for (int i = 1; i < HIST_SIZE; i++) {
            for (int j = 0; j < 256; j++) history[i-1][j] = history[i][j];
        }
        for (int i = 0; i <= cmd_pos; i++) history[HIST_SIZE-1][i] = cmd[i];
    }
    hist_idx = hist_count;
    
    if (strcmp(cmd, "help") == 0) {
        show_help();
    } else if (strcmp(cmd, "clear") == 0) {
        vga_clear();
    } else if (strcmp(cmd, "version") == 0) {
        show_version();
    } else if (strcmp(cmd, "reboot") == 0) {
        reboot();
    } else if (strcmp(cmd, "credits") == 0) {
        show_credits();   
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
    } else if (strcmp(cmd, "history") == 0) {
        show_history();
    } else if (strcmp(cmd, "clearhistory") == 0) {
        clear_history();
    } else if (strncmp(cmd, "search ", 7) == 0) {
        search_history(cmd + 7);
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
        hist_idx = hist_count;
        while (1) {
            int c = get_key();
            if (c == '\n') {
                vga_putc('\n');
                process_cmd();
                break;
            } else if (c == '\b') {
                if (cmd_pos > 0) {
                    cmd_pos--;
                    vga_putc('\b');
                }
            } else if (c == KEY_UP) {
                if (hist_idx > 0) {
                    hist_idx--;
                    while (cmd_pos > 0) { cmd_pos--; vga_putc('\b'); }
                    for (int i = 0; history[hist_idx][i] && i < 255; i++) {
                        cmd[cmd_pos++] = history[hist_idx][i];
                        vga_putc(history[hist_idx][i]);
                    }
                }
            } else if (c == KEY_DOWN) {
                if (hist_idx < hist_count - 1) {
                    hist_idx++;
                    while (cmd_pos > 0) { cmd_pos--; vga_putc('\b'); }
                    for (int i = 0; history[hist_idx][i] && i < 255; i++) {
                        cmd[cmd_pos++] = history[hist_idx][i];
                        vga_putc(history[hist_idx][i]);
                    }
                } else if (hist_idx < hist_count) {
                    hist_idx++;
                    while (cmd_pos > 0) { cmd_pos--; vga_putc('\b'); }
                }
            } else if (c >= ' ' && cmd_pos < 255) {
                cmd[cmd_pos++] = c;
                vga_putc(c);
            }
        }
    }
}