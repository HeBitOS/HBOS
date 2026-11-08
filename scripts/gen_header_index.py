#!/usr/bin/env python3
"""头文件索引批量接入工具。

按 docs/wiki/CODE_STYLE.md §3.1 / §3.1.1，为每个头文件补：
  · 模板头：@file/@brief/@layer/@owner/@stable + 依赖 + 被依赖
  · 索引头文件：类型 / 宏 / 函数 三类，名字 + 一句话职责

三条"能不猜就不猜"的原则：
  1. 已有模板头（@layer 等）→ 只补索引，绝不动作者写的正文。
  2. 已有 @file 注释块但没有模板 → 升级那一块（保留原 @brief 的意图）。
  3. 什么都没有 → 插到 #ifndef 之前。

索引内容来自**声明本身**：名字从正文解析，职责优先取：
  ① 同行的 /**< ... */ 或 // 尾注
  ② 声明正上方的注释块
  ③ 都没有 → 留「（待补）」，由人补一句——生成器不编造职责。

因此可以反复运行：新增函数后重跑，索引就同步。

用法：
  python3 scripts/gen_header_index.py --dry-run src/foo.h
  python3 scripts/gen_header_index.py --all
  python3 scripts/gen_header_index.py --all --missing    # 只列（待补）的
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PENDING = "（待补）"

# 上游冻结的头文件（BSD/MIT 许可，改了没法同步上游）。仍补索引——重构也好，
# 查接口也好，都需要知道它导出什么——但 @stable 标 frozen，提醒别动正文。
FROZEN = {
    "src/limine.h", "src/fb.h", "src/fb_private.h", "src/flanterm.h",
    "src/flanterm_private.h", "src/types/stdbool.h", "src/types/stdint.h",
    "src/types/stddef.h", "src/types/stdarg.h", "src/types/stdalign.h",
}
# 三列左对齐：「类型/宏  /函数」统一宽度 PAD，「  」补齐使中文按两格算也对齐。
LBL_T, LBL_M, LBL_F = "类型", "宏  ", "函数"
PAD = 4  # 「类型」占四列，标签与续行都按此宽度对齐

# ───────────── 层判定：目录规则 → 精确到文件名的特例 ─────────────
LAYER_BY_PREFIX = [
    ("src/core/", "core"),
    ("src/crypto/", "crypto"),
    ("src/gui/", "gui"),
    ("src/ui/", "gui"),
    ("src/graphics/", "gui"),
    ("src/net/", "net"),
    ("src/input/", "input"),
    ("src/shell/", "tools"),
    ("src/tools/", "tools"),
    ("src/app/", "app"),
    ("src/sys/", "user"),
    ("src/user/libc/", "user"),
    ("src/user/", "user"),
    ("src/libc/", "user"),
    ("src/lib/", "lib"),
    ("src/types/", "types"),
    ("src/boot/", "boot"),
]

LAYER_BY_FILE = {
    "src/version.h": "boot",
    "src/types.h": "types",
    "src/errno.h": "user",
    "src/fcntl.h": "user",
    "src/unistd.h": "user",
    "src/signal.h": "user",
    "src/syscall.h": "user",
    "src/string.h": "user",
    "src/stdlib.h": "user",
    "src/elf.h": "user",
    "src/linux_compat.h": "user",
    "src/limine.h": "boot",
    "src/ipc.h": "core",
    "src/vfs.h": "fs",
    "src/fs.h": "fs",
    "src/fd.h": "fs",
    "src/devfs.h": "fs",
    "src/block.h": "fs",
    "src/partition.h": "fs",
    "src/smp.h": "core",
    "src/selftest.h": "core",
    "src/tty.h": "input",
    "src/fb.h": "gui",
    "src/gpu.h": "gui",
    "src/flanterm.h": "gui",
    "src/bmp.h": "gui",
    "src/png.h": "gui",
    "src/jpeg.h": "gui",
    "src/inflate.h": "lib",
    "src/wav.h": "lib",
    "src/net.h": "net",
    "src/net6.h": "net",
    "src/tls.h": "net",
    "src/https.h": "net",
    "src/rtc.h": "drivers",
    "src/rtc_tz.h": "drivers",
    "src/pci.h": "drivers",
    "src/acpi.h": "drivers",
}

# 标准 C 库函数的默认职责——这些名字跨平台语义固定，不靠注释猜。
LIBC_DESC = {
    "memset": "填充 n 字节为 c，返回 s",
    "memcpy": "复制 n 字节，区域不得重叠",
    "memmove": "复制 n 字节，允许重叠",
    "memcmp": "比较 n 字节，返回差值符号",
    "memchr": "在 n 字节内找字符 c",
    "strlen": "字符串长度（不含结尾 NUL）",
    "strnlen": "长度上限 n 的 strlen",
    "strcmp": "比较字符串，返回差值符号",
    "strncmp": "比较前 n 个字符",
    "strcpy": "复制字符串（含结尾 NUL）",
    "strncpy": "复制至多 n 字符，不足补 NUL",
    "strcat": "追加字符串",
    "strncat": "追加至多 n 字符",
    "strchr": "找首次出现的字符",
    "strrchr": "找末次出现的字符",
    "strstr": "找子串，返回首次出现位置",
    "strpbrk": "找首个属于字符集的字符",
    "strspn": "前缀中属于字符集的长度",
    "strcspn": "前缀中不属于字符集的长度",
    "strdup": "复制到新分配的堆内存",
    "strtok": "按分隔符切分（有状态）",
    "strerror": "errno 值转错误描述",
    "strcasecmp": "忽略大小写比较",
    "strncasecmp": "忽略大小写比较前 n 字符",
    "atoi": "字符串转 int",
    "strtol": "按进制转 long，endptr 收尾",
    "malloc": "从堆分配 size 字节",
    "calloc": "分配 nmemb×size 并清零",
    "realloc": "调整已有分配块大小",
    "free": "释放堆块",
    "exit": "结束进程，不返回",
    "abs": "绝对值",
    "qsort": "就地快速排序",
    "snprintf": "格式化到缓冲，返回应写长度",
    "sprintf": "格式化到缓冲",
    "printf": "格式化输出到 stdout",
    "fprintf": "格式化输出到流",
    "puts": "输出字符串 + 换行",
    "putchar": "输出一个字符",
    "write": "向文件描述符写",
    "read": "从文件描述符读",
    "open": "打开路径，返回 fd",
    "close": "关闭 fd",
    "ioctl": "设备控制",
    "fork": "复制当前进程",
    "execve": "替换当前进程映像",
    "waitpid": "等待子进程状态变化",
    "brk": "调整数据段末尾",
    "mmap": "映射内存/文件到地址空间",
    "isalpha": "是否字母",
    "isdigit": "是否数字",
    "isspace": "是否空白",
    "tolower": "转小写",
    "toupper": "转大写",
    "assert": "断言失败即中止",
}

# POSIX/标准库名字的固定职责。这些语义由标准决定，不该靠猜，也不该留空。
LIBC_DESC_EXT = {
    # errno
    "EPERM": "操作不允许", "ENOENT": "文件或目录不存在", "ESRCH": "无此进程",
    "EINTR": "调用被信号中断", "EIO": "I/O 错误", "ENXIO": "无此设备或地址",
    "E2BIG": "参数列表过长", "ENOEXEC": "格式错误，无法执行",
    "EBADF": "文件描述符非法", "ECHILD": "无子进程", "EAGAIN": "资源暂时不可用",
    "ENOMEM": "内存不足", "EACCES": "权限不足", "EFAULT": "地址错误",
    "EBUSY": "设备或资源忙", "EEXIST": "已存在", "EXDEV": "跨设备链接",
    "ENODEV": "无此设备", "ENOTDIR": "不是目录", "EISDIR": "是目录",
    "EINVAL": "参数非法", "ENFILE": "系统打开文件数超限",
    "EMFILE": "进程打开文件数超限", "ENOTTY": "不适当的 ioctl",
    "EFBIG": "文件过大", "ENOSPC": "设备空间不足", "ESPIPE": "非法 seek",
    "EROFS": "只读文件系统", "EPIPE": "管道破裂", "ERANGE": "结果超出范围",
    "ENOSYS": "未实现的系统调用", "ENOTEMPTY": "目录非空",
    "ELOOP": "符号链接层级过多", "ENOTSOCK": "不是套接字",
    "EDESTADDRREQ": "需要目标地址", "EMSGSIZE": "消息过长",
    "EPROTOTYPE": "套接字协议类型错误", "ENOPROTOOPT": "协议选项不可用",
    "EPROTONOSUPPORT": "不支持的协议",
    "ESOCKTNOSUPPORT": "不支持的套接字类型",
    "EOPNOTSUPP": "操作不支持", "EPFNOSUPPORT": "不支持的协议族",
    "EAFNOSUPPORT": "地址族不支持", "EADDRINUSE": "地址已被占用",
    "EADDRNOTAVAIL": "地址不可用", "ENETDOWN": "网络已关闭",
    "ENETUNREACH": "网络不可达", "ENETRESET": "连接因重置而中断",
    "ECONNABORTED": "连接被中止", "ECONNRESET": "连接被对端重置",
    "ENOBUFS": "缓冲区空间不足", "EISCONN": "已连接",
    "ENOTCONN": "未连接", "ESHUTDOWN": "套接字已关闭",
    "ETOOMANYREFS": "引用过多", "ETIMEDOUT": "连接超时",
    "ECONNREFUSED": "连接被拒绝", "EHOSTDOWN": "主机已关闭",
    "EHOSTUNREACH": "主机不可达", "EALREADY": "操作正在进行",
    "EINPROGRESS": "操作进行中", "EWOULDBLOCK": "同 EAGAIN",
    # stdio
    "EOF": "文件结束标志（−1）", "BUFSIZ": "标准缓冲大小",
    "FILENAME_MAX": "路径名最大长度", "FOPEN_MAX": "同时打开文件数上限",
    "fopen": "按模式打开流", "fclose": "关闭流并刷新",
    "fflush": "刷新流缓冲", "fread": "从流读一块",
    "fwrite": "向流写一块", "fseek": "设置流位置",
    "ftell": "取流当前位置", "fgetc": "读一个字符",
    "fputc": "写一个字符", "fgets": "读一行到缓冲",
    "fputs": "写字符串到流", "feof": "是否已到流末尾",
    "ferror": "流是否出错", "vprintf": "va_list 版 printf",
    "vfprintf": "va_list 版 fprintf", "vsprintf": "va_list 版 sprintf",
    "vsnprintf": "va_list 版 snprintf", "getchar": "从 stdin 读一字符",
    "getc": "从流读一字符", "putc": "向流写一字符",
    "remove": "删除文件或空目录",
    # stdlib
    "RAND_MAX": "rand() 最大值", "atol": "字符串转 long",
    "strtoul": "按进制转 unsigned long", "abort": "异常终止",
    "rand": "伪随机数", "srand": "设置随机种子",
    "labs": "long 绝对值",
    "EXIT_SUCCESS": "成功退出码（0）", "EXIT_FAILURE": "失败退出码（1）",
    # 类型
    "socklen_t": "套接字地址长度", "ssize_t": "有符号大小",
    "off_t": "文件偏移", "pid_t": "进程号", "mode_t": "文件模式位",
    "uid_t": "用户号", "gid_t": "组号", "dev_t": "设备号",
    "ino_t": "inode 号", "nlink_t": "硬链接数", "time_t": "秒级时间戳",
    "clock_t": "时钟计数", "useconds_t": "微秒数", "key_t": "IPC 键",
    "sockaddr_un": "UNIX 域套接字地址", "sockaddr_in": "IPv4 套接字地址",
    "sockaddr_in6": "IPv6 套接字地址", "sockaddr": "通用套接字地址",
    "in_addr": "IPv4 地址字", "in6_addr": "IPv6 地址字",
    "addrinfo": "地址解析结果", "hostent": "主机表项",
    "tm": "分解时间（年月日时分秒）", "timespec": "秒 + 纳秒",
    "timeval": "秒 + 微秒", "itimerval": "间隔定时器",
    "sigset_t": "信号集", "sigaction": "信号处理动作",
    "fd_set": "select 用描述符集合", "pollfd": "poll 用单个描述符项",
    "epoll_event": "epoll 事件", "iovec": "scatter/gather 缓冲区",
    # POSIX 函数
    "open": "打开路径，返回 fd", "close": "关闭 fd",
    "read": "从 fd 读", "write": "向 fd 写",
    "lseek": "移动 fd 读写位置", "unlink": "删除目录项",
    "rename": "重命名路径", "stat": "取路径元数据",
    "lstat": "取元数据（不跟随符号链接）",
    "fstat": "按 fd 取元数据", "mkdir": "建目录",
    "rmdir": "删空目录", "access": "检查可达性",
    "isatty": "fd 是否为终端", "ftruncate": "按 fd 截断",
    "truncate": "按路径截断", "getcwd": "取当前工作目录",
    "chdir": "切换工作目录", "getpid": "取当前进程号",
    "getppid": "取父进程号", "sleep": "休眠整秒",
    "usleep": "休眠微秒", "geteuid": "取有效用户号",
    "getegid": "取有效组号", "setuid": "设置用户号",
    "pipe": "创建单向管道", "dup": "复制 fd",
    "dup2": "复制 fd 到指定编号", "poll": "等待一组 fd 就绪",
    "select": "等待 fd 集合就绪", "socket": "创建套接字",
    "bind": "绑定地址", "listen": "进入监听",
    "accept": "接受连接", "connect": "发起连接",
    "send": "发送数据", "recv": "接收数据",
    "sendto": "发数据报到指定地址", "recvfrom": "收数据报并记录来源",
    "setsockopt": "设置套接字选项", "getsockopt": "取套接字选项",
    "shutdown": "关闭套接字单向流", "kill": "向进程发信号",
    "signal": "注册信号处理", "sigaction": "注册信号动作",
    "sigprocmask": "屏蔽/放行信号", "raise": "向自身发信号",
    "pthread_create": "创建线程", "pthread_join": "等待线程结束",
    "pthread_mutex_lock": "加互斥锁", "pthread_mutex_unlock": "解互斥锁",
    "nanosleep": "高精度休眠", "clock_gettime": "读某个时钟",
    "gettimeofday": "取当前时间", "time": "取秒级时间",
    "gmtime": "时间戳转 UTC 分解时间", "localtime": "时间戳转本地分解时间",
    "mktime": "分解时间转时间戳", "strftime": "按格式输出时间",
    "opendir": "打开目录流", "readdir": "读一项目录项",
    "closedir": "关闭目录流", "scandir": "列出整目录",
    "dlopen": "打开动态库", "dlsym": "按名取符号",
    "dlclose": "关闭动态库句柄", "dlerror": "取最近一次动态库错误",
    "mmap": "映射内存/文件", "munmap": "解除映射",
    "mprotect": "改映射权限", "madvise": "给内核访问建议",
    "getenv": "取环境变量", "setenv": "设置环境变量",
    "putenv": "以字符串设置环境变量", "unsetenv": "删除环境变量",
    "execv": "以 argv 替换映像", "execvp": "按 PATH 查找并替换映像",
    "fork": "复制进程", "wait": "等任一子进程", "waitpid": "等指定子进程",
    "atol": "字符串转 long", "atof": "字符串转 double",
    "strtod": "按浮点解析字符串", "strtoull": "按进制转 unsigned long long",
    "fabs": "绝对值", "sqrt": "平方根", "pow": "幂",
    "floor": "向下取整", "ceil": "向上取整", "fmod": "浮点取余",
    "sin": "正弦", "cos": "余弦", "tan": "正切",
    "exp": "自然指数", "log": "自然对数", "log2": "以 2 为底对数",
    "log10": "以 10 为底对数", "round": "四舍五入",
    "isnan": "是否 NaN", "isinf": "是否无穷",
    "fmin": "取较小值", "fmax": "取较大值",
    "setjmp": "保存跳转点", "longjmp": "跳回保存点",
    "console_puts": "输出字符串到控制台",
    "console_write": "输出定长字节到控制台",
    "console_putchar": "输出一个字符",
    "console_flush": "刷新控制台缓冲",
    "console_clear": "清屏",
    "console_set_fg": "设置前景色",
    "console_set_bg": "设置背景色",
    "console_get_size": "取控制台行列数",
    "console_is_initialized": "控制台是否已就绪",
    # 常量
    "AT_FDCWD": "以 cwd 为基准解析相对路径",
    "AT_SYMLINK_NOFOLLOW": "不跟随末端符号链接",
    "AT_EMPTY_PATH": "允许空路径操作 fd 自身",
    "EXIT_SUCCESS": "成功退出码（0）",
    "EXIT_FAILURE": "失败退出码（1）",
}

# 常量名里的「噪声词」，直接删掉后再做标识符拆词，避免"内存大小缓冲区上限"
# 这种堆叠。保留的词是有语义的（读/写/端口/中断……）。
NOISE = {"MAX", "MIN", "SIZE", "COUNT", "NUM", "TOTAL", "LIMIT", "LEN",
         "LENGTH", "DEFAULT", "DEF", "CAP", "SHIFT", "BITS", "BIT"}
WORDS = {
    "ADDR": "地址", "BASE": "基址", "PORT": "端口", "IRQ": "中断号",
    "REG": "寄存器", "REGISTER": "寄存器", "ENABLE": "使能", "DISABLE": "禁用",
    "FLAG": "标志", "MASK": "掩码", "STATUS": "状态", "STATE": "状态",
    "BUFFER": "缓冲区", "BUF": "缓冲区", "QUEUE": "队列", "TABLE": "表",
    "ENTRY": "表项", "CHANNEL": "通道", "DESC": "描述符", "RING": "环",
    "PAGE": "页", "BLOCK": "块", "SECTOR": "扇区", "FRAME": "帧",
    "CLUSTER": "簇", "INODE": "inode", "EXTENT": "区段", "NODE": "节点",
    "HANDLE": "句柄", "TOKEN": "令牌", "COOKIE": "标识", "ID": "编号",
    "INDEX": "下标", "TIMEOUT": "超时", "TIME": "时间", "CLOCK": "时钟",
    "TICK": "时钟滴答", "INTERVAL": "间隔", "PERIOD": "周期",
    "WIDTH": "宽度", "HEIGHT": "高度", "DEPTH": "深度", "PITCH": "跨度",
    "STRIDE": "行跨度", "COLOR": "颜色", "FONT": "字体", "GLYPH": "字形",
    "ICON": "图标", "THEME": "主题", "WINDOW": "窗口", "FRAME_RATE": "帧率",
    "TASK": "任务", "THREAD": "线程", "PROCESS": "进程", "PCB": "进程控制块",
    "STACK": "栈", "HEAP": "堆", "MEM": "内存", "MEMORY": "内存",
    "PHYS": "物理", "VIRT": "虚拟", "DMA": "DMA", "MMIO": "MMIO",
    "CPU": "CPU", "CORE": "核心", "SMP": "SMP", "APIC": "APIC",
    "LOCK": "锁", "MUTEX": "互斥量", "SPIN": "自旋", "WAIT": "等待",
    "SIGNAL": "信号", "EVENT": "事件", "MSG": "消息", "PACKET": "报文",
    "SOCKET": "套接字", "PACKET_SIZE": "报文长度", "HEADER": "首部",
    "PAYLOAD": "载荷", "CHECKSUM": "校验和", "CRC": "校验值",
    "VERSION": "版本", "MAJOR": "主", "MINOR": "次", "PATCH": "修订",
    "RETRY": "重试", "RETRIES": "重试次数", "BACKOFF": "退避",
    "SLOT": "槽位", "PARTITION": "分区", "DEVICE": "设备", "DRIVER": "驱动",
    "BUS": "总线", "SLOT_ID": "槽位号", "VENDOR": "厂商", "PRODUCT": "产品",
    "CACHE": "缓存", "LINE": "行", "WAYS": "路", "SET": "组",
    "READ": "读", "WRITE": "写", "ERASE": "擦除", "FORMAT": "格式化",
    "MOUNT": "挂载", "SYNC": "同步", "FLUSH": "刷新", "CLOSE": "关闭",
    "OPEN": "打开", "INIT": "初始化", "DEINIT": "反初始化",
    "START": "启动", "STOP": "停止", "RESET": "复位", "RESTART": "重启",
    "ENABLE_IRQ": "启用中断", "HANDLE_IRQ": "处理中断",
    "ALLOC": "分配", "FREE": "释放", "MAP": "映射", "UNMAP": "解映射",
    "COPY": "复制", "MOVE": "移动", "FIND": "查找", "LOOKUP": "查找",
    "GET": "取", "SET": "设置", "QUERY": "查询", "UPDATE": "更新",
    "CREATE": "创建", "DESTROY": "销毁", "DELETE": "删除", "REMOVE": "移除",
    "ADD": "添加", "PUSH": "压入", "POP": "弹出", "PEEK": "查看",
    "SEND": "发送", "RECV": "接收", "RECEIVE": "接收", "POLL": "轮询",
    "PROBE": "探测", "SCAN": "扫描", "DUMP": "打印", "PRINT": "输出",
    "LOG": "日志", "INFO": "信息", "DEBUG": "调试", "ERROR": "错误",
    "WARN": "警告", "FATAL": "致命", "PANIC": "崩溃处理",
}

# 名字形态 → 职责。这些只描述「这是个什么形状的东西」，不编造语义。
SHAPE_RULES = [
    (r"^limine_\w+_(request|response)$", "Limine 引导协议结构"),
    (r"^\w+_t$", "结构体 / 类型别名"),
    (r"^\(\*\w+\)$", "函数指针"),
]
BRIEF_HINTS = {
    "io.h": "端口 I/O 与 CPU 特权指令的内联封装",
    "cpu.h": "CPU 特征探测、GDT/IDT/TSS 与 MSR 访问",
    "task.h": "任务控制块、上下文切换与调度入口",
    "heap.h": "内核堆：按需映射的 first-fit 分配器",
    "wait.h": "等待队列：阻塞一个任务直到被唤醒",
    "sched_core.h": "调度核心状态查询",
    "vfs.h": "虚拟文件系统：节点、目录操作与挂载",
    "fs.h": "HBFS 文件系统：内存文件表与磁盘布局",
    "elf.h": "ELF64 解析与可执行加载",
    "fd.h": "文件描述符表",
    "block.h": "块设备抽象层：把各类磁盘统一成扇区读写",
    "pci.h": "PCI 配置空间枚举与 BAR 解析",
    "acpi.h": "ACPI 表解析与关机/重启",
    "net.h": "网络栈对外接口（socket 与协议入口）",
    "syscall.h": "系统调用号表与内核侧分发",
    "errno.h": "errno 编号与线程局部 errno",
    "limine.h": "Limine 引导协议结构体（上游冻结，勿改）",
    "version.h": "构建版本字符串",
    "types.h": "<stdint.h>/<stddef.h>/<stdbool.h> 的聚合入口",
    "selftest.h": "启动期自检入口",
    "string.h": "mem 系列与 str 系列的声明（内核与用户态共用）",
    "stdlib.h": "malloc/free/exit 等最常用的 stdlib 声明",
}


def rel(path):
    return os.path.relpath(os.path.abspath(path), ROOT).replace(os.sep, "/")


def layer_of(path):
    r = rel(path)
    if r in LAYER_BY_FILE:
        return LAYER_BY_FILE[r], owner_of(r)
    for prefix, layer in LAYER_BY_PREFIX:
        if r.startswith(prefix):
            return layer, owner_of(r)
    return "drivers", owner_of(r)


def owner_of(r):
    d = os.path.dirname(r)
    return (d + "/") if d else "src/"


# ───────────── 解析 ─────────────
def strip_comments(src):
    out, i, n = [], 0, len(src)
    while i < n:
        if src.startswith("/*", i):
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join("\n" if c == "\n" else " " for c in src[i:j]))
            i = j
        elif src.startswith("//", i):
            j = src.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        else:
            out.append(src[i])
            i += 1
    return "".join(out)


NOT_FUNC = {"if", "for", "while", "switch", "return", "sizeof", "typeof",
            "defined", "__asm__", "asm", "else", "do", "alignof"}

TYPEDEF_TAIL_RE = re.compile(r"^\}\s*([A-Za-z_]\w*)\s*;", re.M)
TYPEDEF_NAME_RE = re.compile(r"^\s*([A-Za-z_]\w*)\s*;$", re.M)
TYPEDEF_ALIAS_RE = re.compile(
    r"^\s*typedef\s+(?:unsigned\s+|signed\s+)?[A-Za-z_][\w \*]*\s+\**"
    r"([A-Za-z_]\w*)\s*(\[[^\]]*\])?\s*;", re.M)
STRUCT_RE = re.compile(r"^\s*struct\s+([A-Za-z_]\w*)\s*\{", re.M)
ENUM_NAMED_RE = re.compile(r"^\s*enum\s+([A-Za-z_]\w*)\s*\{", re.M)
MACRO_RE = re.compile(r"^[ \t]*#[ \t]*define[ \t]+([A-Za-z_]\w*)", re.M)
# 函数：行首是类型（允许 static/const/inline/unsigned），名字后紧跟 '('，
# 结尾 ';'（原型）或 '{'（static inline 定义）。不跨行匹配，避免误吞正文。
# 只认「行首类型 → 名字 → (→ 参数 → ) → 行尾 ]{?」。参数里不许出现 '{'
# （否则就是函数体，说明不是在解析原型），也不许出现 ')' 之外的分号。
# 靠"行尾的 { 或 ;"这一条把 `#define F(x) do {`、`if (...) {` 过滤掉。
# 函数原型：类型在行首、名字紧跟 '('、参数里只有类型/名字/指针/数组、
# 结尾是 ';' 或 '{'。分两段写（左半 + 其余），避免贪婪组把 `*p` 前的类型吃掉。
FUNC_RE = re.compile(
    r"^(?:static\s+|extern\s+|inline\s+|_Noreturn\s+)*"
    r"(?:const\s+|volatile\s+|unsigned\s+|signed\s+|struct\s+|enum\s+)*"
    r"[A-Za-z_][\w \t]*[ \t\*]+"
    r"([A-Za-z_]\w*)[ \t]*\(",
    re.M)

# 参数必须以 ')' 收尾，且整行结束于 ';' 或 '{'——用尾部锚定排除宏与函数体
FUNC_TAIL_RE = re.compile(r"^[^;{()]*\)[ \t]*(?:;|\{)[ \t]*$")


def collect(src, path):
    """返回 (types, macros, funcs)，每项 [name, desc]。"""
    code = strip_comments(src)
    types, macros, funcs = [], [], []
    seen_t, seen_m, seen_f = set(), set(), set()
    guard = ""
    g = re.search(r"#\s*ifndef\s+(\w+)", code)
    if g:
        guard = g.group(1)

    # typedef struct {...} name;  —— 用 TYPEDEF_TAIL 找收尾名
    for m in TYPEDEF_TAIL_RE.finditer(code):
        n = m.group(1)
        if n not in seen_t:
            seen_t.add(n)
            types.append([n, ""])
    for m in STRUCT_RE.finditer(code):
        n = m.group(1)
        if n not in seen_t:
            seen_t.add(n)
            types.append([n, ""])
    for m in ENUM_NAMED_RE.finditer(code):
        n = m.group(1)
        if n not in seen_t:
            seen_t.add(n)
            types.append([n, ""])
    for m in TYPEDEF_ALIAS_RE.finditer(code):
        n = m.group(1)
        if n not in seen_t and n != guard:
            seen_t.add(n)
            types.append([n, ""])

    for m in MACRO_RE.finditer(code):
        n = m.group(1)
        if n == guard or n in seen_t or n in seen_m:
            continue
        if re.match(r"^_[A-Z0-9_]+$", n):
            continue          # __attribute__ 之类
        seen_m.add(n)
        macros.append([n, ""])

    for m in FUNC_RE.finditer(code):
        n = m.group(1)
        # 从 '(' 之后到行尾必须是「参数)」+ ';' 或 '{'。这一条把
        # `#define io_in8(p) (t8(io_in8_port(p)))` 和 `if (x) {` 全挡掉。
        eol = code.find("\n", m.end())
        eol = len(code) if eol < 0 else eol
        if not FUNC_TAIL_RE.match(code[m.end():eol]):
            continue
        if n in NOT_FUNC or n in seen_t or n in seen_m or n in seen_f:
            continue
        seen_f.add(n)
        funcs.append([n, ""])
    return types, macros, funcs


def find_line(lines, name):
    pat = re.compile(r"\b%s\b" % re.escape(name))
    for i, line in enumerate(lines):
        if pat.search(line):
            return i
    return -1


def sanitize(text):
    """注释里不许出现 */、@ 行首、反斜杠等会破坏注释的字符。"""
    if not text:
        return text
    text = text.replace("*/", "*\\/").replace("/*", "/\\*")
    text = text.replace("\\", "/") if "\\" in text else text
    text = text.replace("@", "(at)")
    return text


def clean_desc(text):
    text = re.sub(r"^/\*+<?", "", text)
    text = re.sub(r"\*/$", "", text)
    text = re.sub(r"^//+", "", text)
    text = re.sub(r"^\*+", "", text).strip()
    text = re.sub(r"^@brief\s*", "", text)
    text = re.sub(r"^@param\s.*$", "", text)
    text = re.sub(r"^@return\s*", "返回 ", text)
    text = re.sub(r"^@note\s*", "", text)
    if not text or text.startswith("@"):
        return ""
    for sep in ("。", "；", "，因为", ". "):
        if sep in text:
            text = text.split(sep)[0]
    text = text.strip(" 。.;，")
    if len(text) > 22:
        text = text[:21].rstrip() + "…"
    return text


INLINE_RE = re.compile(r"/\*\*<\s*(.+?)\s*\*/")
TRAIL_RE = re.compile(r"//\s*(.+?)\s*$")


def descs_for(path, events, macros=None):
    global _group_macros
    _group_macros = macros or []
    try:
        lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
    except OSError:
        return
    want = {e[0]: e for e in events}
    for idx, line in enumerate(lines):
        m = INLINE_RE.search(line) or TRAIL_RE.search(line)
        if m:
            for name, ev in want.items():
                if ev[1]:
                    continue
                if re.search(r"\b%s\b" % re.escape(name), line):
                    d = clean_desc(m.group(1))
                    if d:
                        ev[1] = d
    for name, ev in want.items():
        if ev[1]:
            continue
        if name in LIBC_DESC:
            ev[1] = LIBC_DESC[name]
        elif name in LIBC_DESC_EXT:
            ev[1] = LIBC_DESC_EXT[name]
        elif not ev[1]:
            # 还没有任何线索：按名字形态给一个不撒谎的描述
            kind = "宏" if name in set(x[0] for x in _group_macros) else ""
            d = shape_desc(name, kind)
            if d:
                ev[1] = d
    for name, ev in want.items():
        if ev[1]:
            continue
        ln = find_line(lines, name)
        if ln < 0:
            continue
        buf = []
        i = ln - 1
        while i >= 0 and ln - i <= 10:
            s = lines[i].strip()
            if s.startswith(("*", "/**", "/*")):
                buf.append(s)
                i -= 1
                continue
            break
        for s in reversed(buf):
            d = clean_desc(s)
            if d and not re.search(r"文件系统结构$|函数声明$|^==+|^══", d):
                ev[1] = d
                break


def split_words(name):
    """常量名 → 中文短语。只翻译认识的词，剩下的原样保留（不猜）。"""
    parts = [p for p in re.split(r"_+", name) if p]
    out = []
    for p in parts:
        if p in NOISE:
            continue
        w = WORDS.get(p)
        if w:
            out.append(w)
        else:
            out.append(p)
    return "".join(out).strip()


def shape_desc(name, kind):
    """名字形态的兜底描述。宁可说"结构体"，也不编具体语义。"""
    for pat, desc in SHAPE_RULES:
        if re.match(pat, name):
            return desc
    if kind == "宏":
        w = split_words(name)
        if w and any(p in WORDS for p in re.split(r"_+", name)):
            return w
        return "常量 / 特性宏"
    return ""


# ───────────── 生成 ─────────────
def stable_of(path):
    r = rel(path)
    if r in FROZEN:
        return "frozen（上游代码，勿改正文）"
    return "internal"


def brief_of(path, t, m, f):
    b = os.path.basename(path)
    if b in BRIEF_HINTS:
        return BRIEF_HINTS[b]
    return "导出 %d 个接口（%d 类型 / %d 宏 / %d 函数）" % (
        len(t) + len(m) + len(f), len(t), len(m), len(f))


def index_lines(t, m, f):
    """按组输出；列宽取「本组最长名字」，短组不留大片空白。
    最左是「类型/宏/函数」标签，其右是名字，最后是职责——三列左对齐。
    """
    out = []
    names = [n for n, _ in t + m + f]
    col = min(max([len(n) for n in names] + [12]), 28)
    for label, group in ((LBL_T, t), (LBL_M, m), (LBL_F, f)):
        # 最左是「类型/宏/函数」标签，标签宽度按 label 补齐（类型=4 列，
        # 宏与函数=2 列），于是三组名字都从同一列开始。
        gw = min(max([len(n) for n, _ in group] + [10]), col)
        first = True
        for name, desc in group:
            tag = label if first else " " * PAD
            # 标签「类型」「宏  」「函数」均占 4 列，后接 2 空格 → 名字从第 7 列起
            tag = label if first else " " * 4
            out.append(" *   %s  %-*s  %s"
                       % (tag, gw, name, sanitize(desc) or PENDING))
            first = False
    return out


def header_block(path, t, m, f, include_deps=True):
    layer, owner = layer_of(path)
    lines = [
        "/**",
        " * @file    %s" % os.path.basename(path),
        " * @brief   %s" % sanitize(brief_of(path, t, m, f)),
        " *",
        " * @layer   %s" % layer,
        " * @owner   %s" % owner,
        " * @stable  %s" % stable_of(path),
        " *",
        " * 依赖    见文件正文 #include；本文件刻意不引入更高层的头",
        " * 被依赖  见 docs/agents/ 对应子系统地图",
        " *",
        " * 索引头文件",
    ]
    lines += index_lines(t, m, f)
    lines += [" */"]
    return "\n".join(lines) + "\n"


# 保护宏：`#ifndef X` 后跟 `#define X ...`（上游 froid 风格 `#    define` 也算）。
# 注意 backreference 后面不能再跟 \t*/\n 之类的空白断言——上游文件里
# `define X 1` 后面还有值，用"行尾"锚定会漏，这里只要求前缀匹配。
GUARD_RE = re.compile(
    r"^[ \t]*#[ \t]*ifndef[ \t]+(\w+)[ \t]*\n"
    r"[ \t]*#[ \t]*define[ \t]+\1", re.M)
FILE_COMMENT_RE = re.compile(r"\A\s*/\*\*.*?\*/\s*\n", re.S)


def merge_index(src, t, m, f):
    """把索引插进已有注释块（@stable 之后；没有 @stable 则在 */ 之前）。"""
    blk = "\n".join(index_lines(t, m, f))
    m_stable = re.search(r"@stable[^\n]*\n", src)
    if m_stable:
        pos = m_stable.end()
        return src[:pos] + " *\n * 索引头文件\n" + blk + "\n" + src[pos:]
    # 没有 @stable：找第一个注释块的结尾 */
    end = src.find("*/")
    if end < 0:
        return None
    return src[:end] + " *\n * 索引头文件\n" + blk + "\n" + src[end:]


def upgrade_existing_comment(src, path, t, m, f):
    """已有 @file 块 → 补 @layer/@owner/@stable/依赖 + 索引，保留原 @brief。"""
    mo = FILE_COMMENT_RE.match(src)
    if not mo:
        return None
    block = mo.group(0)
    if "@layer" in block:
        return merge_index(src, t, m, f)
    layer, owner = layer_of(path)
    body = block.rstrip()
    assert body.endswith("*/")
    body = body[: -2].rstrip()
    if body.endswith("*"):
        body = body[:-1].rstrip()
    lines = [
        body,
        " *",
        " * @layer   %s" % layer,
        " * @owner   %s" % owner,
        " * @stable  %s" % stable_of(path),
        " *",
        " * 依赖    见文件正文 #include；本文件刻意不引入更高层的头",
        " * 被依赖  见 docs/agents/ 对应子系统地图",
        " *",
        " * 索引头文件",
    ]
    lines += index_lines(t, m, f)
    new_block = "\n".join(lines) + "\n */\n"
    return new_block + src[mo.end():]


def has_template(src):
    head = "\n".join(src.splitlines()[:40])
    return all(k in head for k in ("@layer", "@owner", "@stable"))


def process(path, dry=False):
    src = open(path, encoding="utf-8", errors="replace").read()
    if has_template(src) and "索引头文件" in src:
        return "skip", None
    t, m, f = collect(src, path)
    descs_for(path, t + m + f, m)

    if has_template(src):
        new = merge_index(src, t, m, f)
        st = "merged"
    elif FILE_COMMENT_RE.match(src):
        new = upgrade_existing_comment(src, path, t, m, f)
        st = "upgraded"
    else:
        g = GUARD_RE.search(src)
        if not g:
            return "noguard", None
        new = header_block(path, t, m, f) + src[g.start():]
        new = src[: g.start()] + new
        st = "new"
    if new is None:
        return "fail", None
    if not dry:
        open(path, "w", encoding="utf-8").write(new)
    pending = sum(1 for e in t + m + f if not e[1])
    return st, (len(t), len(m), len(f), pending)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    dry = "--dry-run" in sys.argv
    if "--all" in sys.argv:
        out = subprocess.run(
            ["bash", os.path.join(ROOT, "scripts/check_headers.sh")],
            capture_output=True, text=True, cwd=ROOT).stdout
        args = [l.split("缺模板头")[1].strip() for l in out.splitlines()
                if "缺模板头" in l and l.strip().endswith(".h")]

    summary = {}
    pend_total = 0
    for p in args:
        st, info = process(p, dry)
        summary[st] = summary.get(st, 0) + 1
        if info:
            pend_total += info[3]
            print("%-9s %-46s 类型%2d 宏%2d 函数%2d 待补%2d"
                  % (st, rel(p), info[0], info[1], info[2], info[3]))
        else:
            print("%-9s %s" % (st, rel(p)))
    print()
    print("；".join("%s %d" % (k, v) for k, v in sorted(summary.items())))
    if pend_total:
        print("共 %d 行职责留了「%s」，需要人补一句。" % (pend_total, PENDING))


if __name__ == "__main__":
    main()
