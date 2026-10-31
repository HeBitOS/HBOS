# R32 · Wayland 服务端字节级只读审计

对象：HBOS 自研 Wayland 服务端（`src/gui/wayland/`）与 Debian Chromium（Ozone/Wayland）连上后
**收下 252 字节响应、一个 bind 都不发、随后 `ppoll` 静止**这一现象。

范围严格限定两件事：**A 六个/七个 `wl_registry.global` 事件的逐字段字节审计**，
**B 发送路径与"真正抵达客户端"的审计**。其它接口不在本次范围内。

标签约定：`确定正确` / `确定错误` / `存疑`，每条附 `文件:行号`。

---

## 0. 先说结论

| 项 | 判定 | 一句话 |
|---|---|---|
| A · global 事件字节 | **确定正确** | 252 字节 = 8 条消息（7 global + done），逐字段无一处偏差，无填充、无截断、无越界 |
| A · version 是否过高 | **确定正确** | 全部 ≤ 客户端接口上限；即使过高也不会导致"零 bind"（见 §2.4） |
| B · 部分写（short write） | **确定正确** | `while (written < out.length)` 正确推进，不会丢尾字节 |
| B · `EAGAIN` 处理 | **确定错误** | 一遇 `EAGAIN` 丢弃剩余字节并**关闭连接**（`wl_server.c:192-202`） |
| B · 输出缓冲溢出 | **确定错误** | `wl_out_begin` 失败不置 `in_error`，溢出时损坏已写事件的 header（`wl_wire.c:93-94`） |
| B · `sync done` 与 global 顺序 | **确定正确** | 不可能倒置（见 §3.4） |
| B · 对象 id 撞号 | **确定正确** | 服务端从不分配 id，`wl_client_new_id` 是死代码 |
| B · `wl_display.delete_id` | **确定错误** | 定义了但**从不发送**，callback 槽位永久泄漏 |
| B · `wl_display.error` | **确定错误** | 全实现**从不发送**，所有失败都是静默的 |
| **"零 bind" 的根因** | **不在此次审计范围内** | A 与 B 均被证伪，嫌疑必须移出我们的序列化层 |

**一句话结论：服务端交给内核的那 252 字节在协议上是完全正确的，A、B 两处都不存在"自研客户端能容忍、
真 libwayland 不能容忍"的字节级缺陷；"零 bind" 的根因必须到客户端侧或内核 fd 语义里去找。**

---

## 1. 审计方法与证据基础

### 1.1 用真实代码复现字节，而不是心算

在宿主机直接编译仓库里的 `wl_wire.c`（未改动），按 `wl_iface_display.c:73-88`（get_registry）
与 `wl_iface_display.c:61-71`（sync）的真实调用顺序产生事件，然后把产物逐字段解析回来。
脚本落在 `/tmp`，仓库内零改动。

```
TOTAL=252  in_error=0  align4=1
messages=8 consumed=252 trailing=0
```

三个数字同时成立，已经排除了"多余填充/截断/尾部脏字节"这一整类嫌疑：
`consumed == trailing == TOTAL`，说明**每条消息的 header.size 恰好首尾相接铺满 252 字节**。

### 1.2 252 这个数字本身就是交叉验证

`jindu.md:480`（R31）记录：补接口时 `flush` 从 **136 → 168 → 252 字节**。用 §2 的字节模型反推：

- 4 个 global（compositor/shm/seat/xdg_wm_base）= 36+28+28+32 = 124，+ done 12 = **136** ✓
- +`wl_output`(32) = **168** ✓
- +`wl_data_device_manager`(44) + `wl_subcompositor`(40) = **252** ✓

**→ 252 = 7 个 global + done，不是 6 个。** 任务书里"6 个 global"是少算了一个（`flush` 的字节数和
`wl_server_start` 注册的 7 个全局对象完全自洽，`wl_server.c:553-566`）。这个订正有实际意义：
它证明**全部 7 条 global 都成功进了 socket，没有一条被丢弃或截断**。

> 判定：**确定正确**。`wl_server.c:553-566` 注册 7 个，`wl_iface_display.c:80-86` 全部发出。

---

## 2. A · global 事件逐字段字节审计

以下偏移量、size、strlen 字段全部来自 §1.1 的真实产物。事件目标是 `registry_id`（客户端在
`get_registry` 里自选的 new_id，此处为 2），代码见 `wl_iface_display.c:81` 的
`wl_event_begin(client, registry_id, WL_REGISTRY_E_GLOBAL)`。

### 2.1 逐字段对照表

| # | 偏移 | size | object_id | opcode | name | strlen 字段 | 实际 strlen | 含 NUL | 填充 | version | 结束偏移 |
|---:|---:|---:|---:|---:|---:|---:|---:|:--:|---:|---:|---:|
| 0 | 0 | 36 | 2 | 0 | 1 | 14 | 13 | ✓ | 2 | 4 | 36 |
| 1 | 36 | 28 | 2 | 0 | 2 | 7 | 6 | ✓ | 1 | 1 | 64 |
| 2 | 64 | 28 | 2 | 0 | 3 | 8 | 7 | ✓ | 0 | 5 | 92 |
| 3 | 92 | 32 | 2 | 0 | 4 | 12 | 11 | ✓ | 0 | 2 | 124 |
| 4 | 124 | 32 | 2 | 0 | 5 | 10 | 9 | ✓ | 2 | 3 | 156 |
| 5 | 156 | 44 | 2 | 0 | 6 | 23 | 22 | ✓ | 1 | 3 | 200 |
| 6 | 200 | 40 | 2 | 0 | 7 | 17 | 16 | ✓ | 3 | 1 | 240 |
| 7 | 240 | 12 | 3 | 0 | — | — | — | — | — | — | 252 |

第 7 条是 `wl_callback.done`（`object_id` = 客户端给 callback 的 new_id 3，`serial` = 1，
`wl_iface_display.c:68-70`）。

接口名与 version 的对应关系（`wl_server.c:553-566`）：
`1=wl_compositor@4  2=wl_shm@1  3=wl_seat@5  4=xdg_wm_base@2  5=wl_output@3
6=wl_data_device_manager@3  7=wl_subcompositor@1`

### 2.2 message header：`object_id` / `size` / `opcode`

真实首字节（第 0 条）：

```
02 00 00 00 | 00 00 24 00
object_id   | size<<16 | opcode
```

- `object_id = 2`：等于客户端 `get_registry` 的 new_id，**正确**。`wl_registry` 不是固定 id，
  只能是客户端给的号；写死 1 才是错的。
- 第二个字 `0x00240000` → `opcode = 低 16 位 = 0`，`size = 高 16 位 = 0x24 = 36`。
  与 wayland 协议 `size<<16 | opcode` 的排布一致。
- `size = 8 + payload` 且 4 字节对齐：全部 8 条满足（36/28/28/32/32/44/40/12 皆 4 的倍数）。
- `opcode = 0`：`WL_REGISTRY_E_GLOBAL = 0`（`wl_protocol.h:38`），与 wayland.xml 一致。

> 判定：**确定正确**。`wl_wire.c:91-101` 写 header、`wl_wire.c:148-160` 回填 size。

### 2.3 `interface` 字符串：length 字段、NUL、4 字节对齐

这是本次审计最值得逐字节确认的一项，结论是**全对**：

- `strlen 字段 = strlen+1`（含结尾 NUL）：8 条全部满足（14 vs 13、7 vs 6、8 vs 7、12 vs 11、
  10 vs 9、23 vs 22、17 vs 16）。实现见 `wl_wire.c:124-125`。
- **NUL 真的写进去了**：逐条检查字符串末字节为 `0x00`（表中"含 NUL"列全 ✓），实现见
  `wl_wire.c:134-138`。
- **对齐填充正确**：填充量在 0..3 之间且把总长对齐到 4（表中"填充"列）。
  实现见 `wl_wire.c:139-145` 的 `while (out->length & 3)`。

人眼可核的一例（第 0 条，`wl_compositor` 13 字符）：

```
0e 00 00 00                                      ← length 字段 = 14 = 13+1  ✓
77 6c 5f 63 6f 6d 70 6f 73 69 74 6f 72 00        ← "wl_compositor" + NUL  ✓
00 00                                            ← 填充 2 字节到 4 对齐   ✓
```

> 判定：**确定正确**。`wl_wire.c:117-146`。

### 2.4 `version`：语义是"服务端支持的最高版本"

**这是本次审计最重要的定性结论，也是任务书点名要回答的第 ② 问。**

`wl_registry.global.version` 的官方语义是**服务端该接口支持的最高版本**，客户端 bind 时自己用
`min(global.version, 自己实现的上限)` 取小。因此：

- **version 偏低 = 安全**。客户端只是用不到新语义。
- **version 偏高 = 危险**，但危险的方式不是"不发 bind"。libwayland 不会因为 global.version 高就
  拒绝；客户端照样 `min()` 后 bind，然后**按它 bind 到的版本去发请求**——如果那个版本里有我们没实现
  的 opcode，服务端会走到 `wl_server.c:89-91` 的 `default: break`（静默丢弃），
  客户端等一个永远不来的事件。**症状是"binding 之后卡住"，不是"一个 bind 都不发"。**

所以即使我们把 version 报高，也**不可能**造成"零 bind"。

再把实际值逐条比对（Debian/Chromium 所用 libwayland 与 wayland-protocols 的接口上限）：

| 接口 | 我们报 | 上游上限 | 判定 |
|---|---:|---:|---|
| wl_compositor | 4 | 6 | 确定正确（Chromium 硬要求 ≥4，正好满足） |
| wl_shm | 1 | 2 | 确定正确 |
| wl_seat | 5 | 9 | 确定正确（Chromium 要求 ≥5，正好满足） |
| xdg_wm_base | 2 | 6 | 确定正确 |
| wl_output | 3 | 4 | 确定正确（Chromium 要求 ≥2） |
| wl_data_device_manager | 3 | 3 | 确定正确（等于上限） |
| wl_subcompositor | 1 | 1 | 确定正确（等于上限） |

> 判定：**确定正确**（`wl_protocol.h:23-26,166,170,182`）。**没有任何一个 version 高于客户端会拒绝的值**，
> 而且 §2.4 的推理说明"高于"这个方向本身就不会产生零 bind。第 ② 问答案：**没有**。

### 2.5 `name`：与 bind 用的号一致、且不与会话中的对象号冲突

- `name` = 注册序号 1..7（`wl_server.c:70` 的 `wl_global_count + 1`），与客户端 bind 时回填的
  `name` 参数一一对应，`wl_server_global_by_name`（`wl_server.c:76-80`）按同一套号查找。
- **`name` 不是 object id**，它与客户端 new_id 空间是两个独立命名空间，**不存在撞号问题**。
  这里 `name` 出现 1，而 object id 1 是 `wl_display`——这**不是**冲突，是正常的。

> 判定：**确定正确**。`wl_server.c:70`、`wl_protocol.h:19-21`。

### 2.6 事件之间的多余填充 / 截断

`consumed == trailing == 252`（§1.1）。8 条消息首尾严格相接，**事件之间没有任何多余填充，
没有截断，没有残留半条消息**。

> 判定：**确定正确**。

### 2.7 A 节总结

**A 节没有任何"确定错误"。** 7 条 global 的每一个字段（header、name、string 长度/NUL/对齐、
version）都与 wayland.xml 和 libwayland 的 marshal 规则逐字节一致。
`jindu.md:486-487` 的推测"问题在 libwayland 的事件分发侧"在**字节层面得到了支持**——
至少我们的字节没有给 libwayland 任何拒绝的理由。

---

## 3. B · 发送路径与"真正抵达客户端"的审计

### 3.1 部分写（short write）：确定正确

`wl_server.c:184-217`：

```c
uint32_t written = 0;
uint32_t total = client->out.length;
while (written < client->out.length) {
    long result = linux_compat_unix_send(client->fd, &client->out.buffer[written],
                                         client->out.length - written, 0x4000);
    ...
    written += (uint32_t)result;
}
```

循环条件用 `written` 推进、每次只发剩下的 `out.length - written`，**短写会被下一次循环补齐，
不会丢尾字节**。

顺带确认发送标志没被吞掉：`0x4000` = `LINUX_MSG_NOSIGNAL`，而 `linux_compat.c:68` 有
`#define LINUX_MSG_IGNORED (LINUX_MSG_NOSIGNAL)`，因此能通过 `linux_compat.c:1954-1955` 的
掩码检查，不会被误判成 `-EOPNOTSUPP`。

> 判定：**确定正确**。

### 3.2 `EAGAIN` / `EWOULDBLOCK` 时静默丢字节 —— **确定错误**

`wl_server.c:192-202`：

```c
if (result <= 0) {
    console_puts("[WL] flush failed at "); ...
    client->out.length = 0;   /* ← 剩余字节被丢弃 */
    return -1;
}
```

问题有两层，而且**第二层比丢字节严重得多**：

1. `result == -EAGAIN`（-11）会走进这个分支，**未发出的剩余字节被 `out.length = 0` 直接丢弃**。
2. 返回 `-1` 后，`wl_server.c:445`（`if (wl_client_flush(client) < 0) return -1;`）把它升级成
   "连接已坏"，`wl_server_poll_once` 在 `wl_server.c:502` 调用 `wl_client_release` ，
   **直接 `close(fd)` 掉线**。

**这条路径是可达的，不是理论问题**：`linux_compat.c:1896-1899` 的 `compat_fd_alloc(..., O_RDWR |
(listener->flags & (LINUX_SOCK_NONBLOCK | LINUX_SOCK_CLOEXEC)))` 让 **accept 出来的连接 fd 继承了
监听 fd 的 `SOCK_NONBLOCK`**（监听 fd 在 `wl_server.c:511-512` 就是用 `WL_SOCK_NONBLOCK` 建的）。
再配合 `linux_compat.c:1966-1970`——对端缓冲满且 fd 非阻塞就返回 `-EAGAIN`——
一次瞬时背压就会变成"丢尾巴 + 掉线"。

**正确写法**：`EAGAIN` 时保留未发送的剩余字节（把 `out.buffer[written..]` memmove 到开头、
`out.length -= written`）并返回 0 表示"以后再来"；只有真正的连接错误（`-EPIPE` 等）才返回 -1 掉线。

> 本次启动握手不会触发：`UNIX_SOCKET_BUFFER = 4096`（`linux_compat.c:72`），252 字节远未填满对端
> 缓冲。所以这条**不是**"零 bind"的成因，但是真 bug。

### 3.3 输出缓冲溢出会损坏已写事件的 header —— **确定错误**

`wl_wire.c:91-101`：

```c
if (out->in_error || out->length + WL_MESSAGE_HEADER_SIZE > out->capacity)
    return 0;                       /* ← 只返回 0，没有置 in_error */
```

缓冲放不下 8 字节头时，`wl_out_begin` **既不置 `in_error`，也不更新 `event_offset`**，只返回 0。
而唯一的调用方 `wl_event_begin`（`wl_server.c:160-162`）**忽略返回值**，接着的
`wl_out_u32` / `wl_out_string` 仍然成功往缓冲里追加**没有 header 的裸参数**；
最后 `wl_out_end`（`wl_wire.c:148-160`）用的是**上一条事件遗留的 `event_offset`**，
把上一条事件的 size 改写成"跨到新垃圾里"的长度——**字节流被静默损坏**，
而不是报错后整体丢弃。

**正确写法**：在 `wl_out_begin` 容量不足的分支里先置 `out->in_error = 1` 再返回 0，
让后续所有写入和被 `wl_client_flush`（`wl_server.c:213,216`）识别为整批失败并丢弃，
而不是半写状态。另外 `wl_server.c:215` 无条件 `out.length = 0` 而 `in_error` 从不复位，
一旦溢出该连接之后每次 flush 都会失败——这点也要一起处理。

> 触发条件是单批事件超过 `WL_MAX_OUTPUT = 8192`（`wl_server.h:21`）。启动握手 252 字节远未达到，
> 但一帧里 surface commit + frame callback + 广播 global 叠加就可能越过。

### 3.4 发送顺序：`sync done` 会不会先于 global 发出 —— **确定正确（不会倒置）**

这是任务书点名要核的重点，结论是**顺序不可能倒置**，理由在分派结构：

1. 客户端的 `get_registry` 与 `sync` 在**同一次 write** 里到达（24 字节），
   `wl_client_consume`（`wl_server.c:369-389`）**按字节流原顺序**逐条解析并立即分派，
   不会重排。
2. `get_registry` 先被分派 → `wl_iface_display.c:80-86` 的循环把 7 条 global 追加进 `out`。
3. `sync` 后被分派 → `wl_iface_display.c:68-70` 把 done 追加进 `out`。
4. 两者都写完后才在 `wl_server.c:445` 统一 flush。

所以 `out` 里的顺序天然是"7 条 global 在前、done 在后"，与 §1.1 实测的偏移完全一致
（done 在偏移 240，是第 8 条、也是最后一条）。`wl_client_flush` 中间也不会被插入（`out` 是单缓冲，
派发在同一线程内串行完成）。

**没有独立的 flush 点能在 done 之后补发 global，也没有任何路径能在 global 之前发 done。**

唯一"global 出现在 done 之后"的合法路径是 `wl_server_broadcast_global`（`wl_server.c:219-235`），
它只在**后续**注册新全局时使用，且该函数目前**没有任何调用者**（死代码），启动阶段不参与。

> 判定：**确定正确**。第 ① 问答案：**不会倒置**。

### 3.5 服务端 object id 与客户端 new_id 空间撞号 —— **确定正确（不存在）**

- 服务端**从不自己分配 id**：`wl_client_new_id`（`wl_server.c:102-115`）在整个仓库里
  **没有任何调用者**（`grep` 只命中定义处），是死代码。
- 所有对象号都来自客户端请求体里的 new_id，由 `wl_client_adopt_id`（`wl_server.c:125-135`）认领，
  已占用则返回 NULL。
- `name` 是独立的 registry 命名空间（§2.5），与 id 无关。

> 判定：**确定正确**。不存在撞号。

### 3.6 `wl_display.delete_id` 从不发送 + callback 槽位永久泄漏 —— **确定错误**

- `WL_DISPLAY_E_DELETE_ID` 在 `wl_protocol.h:33` 定义，但**整个实现从未发出过它**（`grep` 只命中定义）。
- 每次 `wl_display.sync` 都在 `wl_iface_display.c:66` 用 `wl_client_adopt_id` 把 callback_id
  永久占坑，而 `wl_server.c:286-288` 对 `WL_OBJ_CALLBACK` 的处理是空的（`break`），
  **没有任何地方销毁 callback 对象**。
- 后果：每次 sync 永久消耗一个槽位，`WL_MAX_OBJECTS = 128`（`wl_server.h:19`）用完后
  `wl_client_adopt_id` 返回 NULL，`wl_iface_display.c:66-67` **静默返回、done 不再发出**，
  客户端 roundtrip 永久阻塞。`wl_iface_compositor.c:111` 的 frame callback 同理。

**正确写法**：在发出 `wl_callback.done` 之后立即销毁该对象，并补发
`wl_display.delete_id`（否则客户端不会回收该 id，双方 id 空间只增不减）。

> 这是**结构上最能解释"客户端静止"的一类缺陷**，但需要约 125 次 sync 才会触发，启动阶段不太可能。

### 3.7 `wl_display.error` 从不发送：所有失败都静默 —— **确定错误（诊断盲区）**

`WL_DISPLAY_EV_ERROR` 同样只在 `wl_protocol.h:32` 定义，**实现里从不发送**。
配合分派层的多处 `return;`：

- `wl_iface_display.c:65,67,75,77,97,100,103,106` —— 8 处静默返回
- `wl_iface_display.c:140-144` —— 不认识的接口把 id 还回去，**不报错**
- `wl_server.c:271` —— `if (!object) return;` 未知 object id，**不报错**
- `wl_server.c:89-91` —— 未知 opcode `default: break`，**不报错**

意味着：**只要有一处我们对请求的处理和客户端不一致，表现就是"客户端等一个永远不来的事件、
ppoll 静止、没有任何错误信息"**——与观察到的现象形状完全吻合。这是审计后续排查最大的障碍，
建议优先级排在所有功能修复之前。

**正确写法**：任一分派失败都发 `wl_display.error(object_id, code, message)` 并断开连接，
让客户端主动报错，而不是让它挂死。

---

## 4. 两个问题的明确回答

**① `sync done` 与 global 的发送顺序是否可能倒置？**
**不会。** 分派严格按字节流顺序（`wl_server.c:369-389`），`get_registry` 先于 `sync` 被处理，
global 先入 `out`、done 后入 `out`，且只有一个 flush 点（`wl_server.c:445`）。
实测 done 位于偏移 240、是最后一条消息（§1.1）。详见 §3.4。

**② 我们声明的 global version 有没有高于客户端会拒绝的值？**
**没有。** 7 个接口的 version 全部 ≤ 上游上限（§2.4 表）。
更要紧的是**这个方向本身不可能造成"零 bind"**：`global.version` 的语义是"服务端支持的最高版本"，
libwayland 不据此拒绝，客户端自己 `min()`；version 偏高只会让客户端按更高版本发请求，
撞上我们没实现的 opcode 后**在 bind 之后**卡住，而不会一个 bind 都不发。

---

## 5. "客户端一个 bind 都不发"的首要嫌疑排序

A、B 两节的字节级嫌疑已被**实测证伪**（§2.7、§3.4、§3.5），所以排序必须移出序列化层。

**1. 客户端侧的 registry 回调从未被调用（字节已到，事件未分发）**
支撑证据：252 字节 = 7 global + done，字节完全正确（§1.1 实测 `messages=8 consumed=252
trailing=0`）；`jindu.md:482` 记录 `ppoll nfds=1 to=∞ ret=1` —— 等到了可读、读了就静止。
字节没有问题，那"读了却没回调"就只能是分发侧。与 `jindu.md:486-487` 的判断一致，但现在
有了字节层面的正面证据。**下一步该做的是在客户端侧取证，不是继续改服务端序列化。**

**2. 版本组合让 Chromium 在 bind 之前就判定"环境不可用"并提前返回**
支撑证据：版本数值本身全部合规（§2.4），但"合规"不等于"Chromium 认可"——
`wl_compositor` 报 4（Chromium 硬要求 ≥4，卡在下限）、`wl_seat` 报 5（同样卡在下限），
只要实际那个 Chromium 构建要求更高（例如 compositor ≥5 / seat ≥7 的较新分支），
就会在 registry 回调里 `return` 而**不 bind、也不报错**，恰好表现为零 bind。
`jindu.md:466-471` 反复出现"缺失时初始化会静默停住"的同类记录，说明这条路径在本项目上已经发生过多次。
需要现场确认目标 Chromium 的版本要求。判定：**存疑**，但排除成本极低（把 version 提到 5/6 试一次）。

**3. 客户端与我们在**别的** fd / 连接上不一致（`ppoll nfds=1` 这条线索）**
支撑证据：`jindu.md:482` 的 `nfds=1` 极不正常——一个真正的 Chromium 浏览器进程通常同时等
X11、D-Bus、timerfd、eventfd、signal fd。只等 1 个 fd 说明这可能不是主事件循环，
或者它等的**不是我们这条连接**。另有一个已知的语义差异值得顺手排掉：
我们 accept 出的连接 fd 是**非阻塞**的（`linux_compat.c:1896-1899` 继承 `SOCK_NONBLOCK`），
而真实 Linux 的 `accept()` **不继承** `O_NONBLOCK`；libwayland 预期的是"阻塞 fd + `poll` 驱动"，
若它对 `EAGAIN` 的处理与我们不同，行为会分叉。判定：**存疑**，需要 strace 确认 `nfds=1` 的那个 fd 是谁。

---

## 6. 交给下一轮的落地建议（按性价比排序）

1. **先补 `wl_display.error`（§3.7）**。零风险的诊断设施，一旦有任何协议不一致，客户端会直接报错，
   比现在"静止"好查一个数量级。
2. **修 `EAGAIN` 丢字节 + 掉线（§3.2）**。真实 Linux 语义下不改，以后一定会咬人。
3. **修 `wl_out_begin` 溢出损坏 header（§3.3）**。加一行 `in_error = 1` 即可。
4. **补 `delete_id` + 销毁 callback（§3.6）**。不做的话，跑几分钟就会把 128 个槽位耗光。
5. **排掉嫌疑 2**：把 `wl_compositor` 提到 5、`wl_seat` 提到 7 各试一次，成本极低。
6. **排掉嫌疑 3**：对客户端 strace，确认 `nfds=1` 等的是哪个 fd；必要时把 accept 出的 fd 的
   `O_NONBLOCK` 去掉以贴合 Linux 语义。

---

## 附：本次审计的边界与不做的事

- 只读审计，未修改 `src/gui/wayland/` 下任何实现；唯一写入的文件是本文件。
- 未运行 QEMU、`make bios-iso`、`make run*`、`make wl-proto-test`、`make mouse-test`。
- 字节复现只用宿主机 `gcc` 编译了 `src/gui/wayland/core/wl_wire.c`（未改动），脚本在 `/tmp`，未入库。
- 未审计 `wl_compositor` / `wl_shm` / `wl_seat` / `xdg_*` / backend 等接口的请求语义，按 YAGNI 排除。
