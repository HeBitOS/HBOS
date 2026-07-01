/* ls —— 列出目录内容（TUI 应用，演示 opendir/readdir libc 封装） */
#include <hax.h>
#include <libc/dirent.h>

HAX_APP("ls", "列出目录下的文件与子目录", HAX_KIND_TUI);

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "/";

    DIR *d = opendir(path);
    if (!d) {
        hax_printf("无法打开目录: %s\n", path);
        return 1;
    }

    hax_printf("目录 %s:\n", path);
    struct dirent *ent;
    int files = 0, dirs = 0;
    while ((ent = readdir(d)) != 0) {
        if (ent->d_name[0] == '\0') continue;          /* 跳过空名 */
        if (ent->d_type == DT_DIR) {
            hax_printf("  [DIR]  %s\n", ent->d_name);
            dirs++;
        } else {
            hax_printf("         %s\n", ent->d_name);
            files++;
        }
    }
    closedir(d);

    hax_printf("共 %d 个文件，%d 个目录。\n", files, dirs);
    return 0;
}
