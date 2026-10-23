# Xenoblade Chronicles 3 (异度神剑3) iOS 原生移植修复与构建指南

本文档总结《异度神剑3》（Xenoblade Chronicles 3，Title ID `010074F013262000`，版本 1.0.0，SDK 12.3.9）在 `nxnative` 项目下的原生移植现状、此前黑屏的根本原因、修复方案与原理，以及完整的构建与验证步骤。

---

## 一、 黑屏根本原因深度分析（Root Cause）

### 1. 现象回顾
- 游戏在真机上平稳运行（30 fps，无任何崩溃或死锁，48kHz 音频 0 欠载）。
- 但第 4 帧绘制完全屏纯黑渐变淡入四边形（Fade Quad）后，自第 14 帧起至第 2400+ 帧：
  - 每一帧照常调用 `CommandBufferSetViewport`、`CommandBufferSetScissor`、`CommandBufferBindDepthStencilState`、`CommandBufferClearColor` 等 30 种 NVN 函数（共 191 次调用）。
  - **但绘制调用完全为零（`DrawArrays=0`, `DrawElements=0`）**，画面被持续 Clear 为纯黑。

### 2. 核心诱因：显存池（MemoryPool）Guest 侧分配失败返回 NULL
在设备运行日志中观察到如下警告：
```
[WARN] MemoryPoolInitialize: non-virtual pool has no storage (flags=0x64 size=0x19a00000)! Fallback allocating on host
[WARN] MemoryPoolInitialize: non-virtual pool has no storage (flags=0x64 size=0x1a400000)! Fallback allocating on host
```
通过逆向反汇编分析 `main.bin` 内部显存分配流程：
1. **主纹理堆分配（410 MB）**：
   - 地址 `0x44140c` 调用 `aligned_alloc`（内部通过 `0x11030` -> `0xf542a0` -> `0xf55764`）。
   - 随即将分配结果写入内存池创建描述符，调用 `0xf5cdac`（`nvnMemoryPoolBuilderSetStorage`）。
2. **渲染目标池分配（420 MB / 手持模式 350 MB）**：
   - 地址 `0xbd1874` 调用 `0xf9e1f8` -> `0xf9d0f0` -> 虚函数 `0xfa1b3c` -> `0xf542a0` -> `0xf55764`。
   - 分配结果存储在 `[x19, #0x418]`。随后在 `0xbaa820` / `0xbacf48` 中将 `x1 = [x19, #0x418]` 作为 storage 传给 `nvnMemoryPoolBuilderSetStorage`。
3. **因内存耗尽导致返回 NULL**：
   - 游戏内部 `malloc` / `aligned_alloc` / 内存管理器分配失败，返回 `x0 = 0`。
   - 虽然 Host 端在 `hle_nvn.c` 中做了 fallback malloc 保证 NVN 调用不崩溃，但**游戏引擎内部记录了 `[x19, #0x418] == NULL`**。
   - 游戏渲染管线在每帧准备阶段检查该动态缓冲与渲染纹理句柄，判定资源未成功分配，因此直接跳过所有 3D 模型的 Mesh 提交与渲染命令，导致后续每帧 0 Draw Calls！

### 3. 为什么 Guest 侧分配器会返回 NULL？（内存空间分割缺陷）
Xenoblade 3 与传统的 Horizon 游戏（如超级马力欧奥德赛）不同，它**不使用 `svcSetHeapSize`**，而是采用现代 Switch 游戏的 **VirtualAddressMemory（虚拟地址内存，位于 Alias 区域）** 架构。

在原始 Switch 代码中：
- `0xfa188c` 保留了 **60 GB（0xf00000000）** 的虚拟地址空间。
- `0xf55000` 从中划分 **4.0 GB（0x100020000）** 作为小对象动态分配竞技场（Arena，处理 <= 64 KB 的小内存）。
- 剩余空间专供 `0xf55fb4` 处理 > 64 KB 的大块分配（包含尺寸达 512 MB 级的 GPU MemoryPool 分配块）。

**此前 AI 的错误修改**：
1. 前任 AI 将总虚拟地址空间直接截断至 **3.0 GB（0xc0000000）**。
2. 同时将 Arena 内部容量截断为 **2.0 GB（0x80020000）**。
3. 导致大块分配（`0xf55fb4`）可用的总空间只剩下：
   $$3.0\text{ GB} - 2.0\text{ GB} = 1.0\text{ GB (1023.875 MB)}$$
4. 游戏中的两个超大 GPU 内存池：
   - 第 1 个内存池 410 MB（`0x19a00000`），落入 Size Class 47，对应 Chunk 尺寸为 **512 MB**（`0x20000000`）。
   - 第 2 个内存池 420 MB（`0x1a400000`），同样落入 Size Class 47，需要另一个 **512 MB** 的 Chunk。
   - 两个 Chunk 所需空间为 $512\text{ MB} + 512\text{ MB} = 1024\text{ MB}$！
   - $1024\text{ MB} > 1023.875\text{ MB}$，再加上启动阶段其他大块堆的占用，**第 2 个（乃至第 1 个）显存分配必然超出 3.0 GB 上限，在 `0xf56144`（`cmp x11, x15; b.hs #0xf56234`）直接触发 OOM 失败返回 NULL**！

---

## 二、 修复方案与实施细节

针对上述根因，采用系统级的四重协同修复方案：

### 1. 扩容虚拟地址空间总上限（3.0 GB → 3.25 GB）
- 在 `dm/tools/patch_nso.py` 的 `0xfa188c` 处：
  - 机器码：`0000b8d2`（3.0 GB）→ `0000bad2`（3.25 GB）。
  - iOS 端 Guest Alias 区域总容量为 4.375 GB（`0x118000000`），扣除 SDK 节点区及 1 GB 对齐后，3.25 GB 能够安全入驻且留有充裕余量。

### 2. 精确缩减小对象 Arena 容量（2.0 GB → 1.0 GB）
小对象（<= 64 KB）在游戏启动和常驻阶段仅需 200~400 MB，此前给予 2.0 GB 属于严重过度占用。
- 将 Arena 容量调整为 1.0 GB（`0x40020000`）：
  - `0xf55010`: `movz x9, #0xffff` (`e9ff9fd2`)
  - `0xf55020`: `movk x9, #0x4002, lsl #16` (`4900a8f2`)
  - `0xf55078`: `movn x9, #0x4001, lsl #16` (`2900a892`)
  - `0xf5507c`: `movk x9, #0, lsl #0` (`090080f2`)
  - `0xf55084`: `movz x8, #0x4002, lsl #16` (`4800a8d2`)
  - `0xf55088`: `nop` (`1f2003d5`)
- **效果**：
  大块分配（包含两个 512 MB 显存池 Chunk）可支配空间由原来的 1.0 GB 瞬间扩大至：
  $$3.25\text{ GB} - 1.0\text{ GB} = \mathbf{2.25\text{ GB}}$$
  完全包容两个 512 MB Chunk（合计仅 1.0 GB），剩余高达 1.25 GB 裕量！

### 3. 旁路堆容量硬编码断言（Bypass Artificial Heap Cap）
在 `0xfa1b70` 处，游戏会对当前 Heap 已分配总量与 `[x19, #0x40]` 进行校验，超限时若标志位匹配即强制返回 NULL：
```arm64
0xfa1b70  cmp   x8, x9
0xfa1b74  b.ls  #0xfa1b94
0xfa1b78  ldrb  w8, [x19, #0x4c]
0xfa1b7c  cbz   w8, #0xfa1b94
0xfa1b80  mov   x0, xzr   ; 失败返回 NULL
```
- 修改为直接跳转：`0xfa1b70: b #0xfa1b94`（机器码 `09000014`）。
- **效果**：解除应用层堆容量硬编码拦截，统一由底层具备 2.25 GB 余量的 `0xf542a0` 进行安全分配。

### 4. 启用手持模式优化（Handheld Mode）
- 在 `game.tsv` 中新增配置：`op_mode handheld`。
- 重新生成 `game_profile.h`（`#define DM_GAME_OPMODE 0`）。
- **效果**：
  - 触发 `main.bin` 内部 `0xbe8e54-0xbe8e60` 分支，第二显存池尺寸由 420 MB 缩减至 **350 MB**（`0x15e00000`）。
  - 目标渲染分辨率由 1080p 降至 720p，大幅减少 Metal 纹理开销与 A/M 芯片显存带宽压力。

---

## 三、 代码修改与配置文件汇总

| 文件 | 修改项 | 作用 |
| :--- | :--- | :--- |
| `dm/tools/patch_nso.py` | `0xfa188c`, `0xf55010~0xf55088`, `0xfa1b70` | 扩容虚拟地址总线、释放 2.25 GB 大块显存空间、解除 Heap Cap |
| `game.tsv` (Profile) | `op_mode handheld` | 启用 720p 手持模式，第二显存池自 420MB 降至 350MB |
| `game_profile.h` | 由 `game.py` 重新生成 | 同步编译期宏定义 `DM_GAME_OPMODE 0` |

---

## 四、 构建与打包命令

### 1. 构建轻量调试包（Slim IPA，推荐联机测试）
不包含庞大的 RomFS 资源，打包极快（约 30 秒），适合反复验证逻辑与渲染帧：
```bash
PATH="/opt/local/bin:/opt/local/sbin:$PATH" python3 scripts/build_ipa_from_nsp.py \
  ~/Documents/nsp \
  -k ~/Documents/keys/prod.keys \
  --no-heap-check \
  --no-romfs \
  -o ~/Desktop/Xenoblade3_slim.ipa
```

### 2. 构建完整运行包（Full IPA，含完整 RomFS）
包含全量音频、场景与模型数据：
```bash
PATH="/opt/local/bin:/opt/local/sbin:$PATH" python3 scripts/build_ipa_from_nsp.py \
  ~/Documents/nsp \
  -k ~/Documents/keys/prod.keys \
  --no-heap-check \
  -o ~/Desktop/Xenoblade3.ipa
```

---

## 五、 启动时序与初始黑屏深入解析

### 1. 为什么第 4 帧绘制后持续黑屏？
- **第 4 帧的淡入黑屏 Quad**：
  在第 4 帧，游戏引擎提交了一次 `CommandBufferDrawArrays=1`，使用着色器对 `vs=58e2e00d24c6f3ea_b` + `fs=5d36d4144f49ec9a_b`，绘制了覆盖全屏（1280x720）且颜色为纯黑 `(0, 0, 0, 1)` 的淡入/淡出 Quad。
  绘制完成后，引擎内部在 `0xf610c8: str wzr, [x19, #0x140]` 清空了 2D 顶点计数。
- **第 5 帧起 3D 网格尚未提交的原因**：
  在 Monolith Soft 引擎的渲染管线（`0xc10508`）中，3D 网格的遍历和提交受 `[x19, #0x2b8]`（当前视野中加载完毕的 3D 实体/网格数量）门控。
  若 `[x19, #0x2b8] == 0`，管线判定当前无可见网格，直接跳过所有 3D Mesh 提交，因此每帧仅保留 6 个 Pass 的设置与清屏操作。
- **后台异步加载流**：
  - 第 7、11、12、18 帧：游戏持续初始化了 1,873 个 Shader Program 并加载了 516+ 张纹理。
  - 第 14 帧：Wwise 音频引擎成功启动并播放背景音乐（2000 个回调 0 欠载）。
  - 后台 44 个工作线程正在持续流式解包读取 `RomFS/bf3.ard`（8.98 GB 大文件）。
- **实机冷启动时间基准**：
  在 Nintendo Switch 原机上，《异度神剑3》从桌面冷启动加载至 Monolith Soft Logo 及标题画面需要 **30 ~ 60 秒**（相当于 30 fps 下的 **900 ~ 1800 帧**）。
  此前测试仅在前台运行了约 10 秒（318 帧），且中间多次切入后台导致挂起，游戏实际上仍处于初期的后台资源流式解包阶段。

---

## 七、 2026-10-23 最新黑屏诊断与修复更新

### 1. 现象与线程剖析（7,019 帧 / 4 分钟实机测试）
- 游戏主线程每帧持续循环占用 ~995 ms/s CPU（近 100% 性能核心），每帧执行 2607 次 `SleepThread(0)`（耗时 0.44 ms），其余 32.8 ms 在 Guest 代码中进行轮询。
- 41 个 Worker 线程 CPU 时间均为 0 ms/s（处于挂起等待状态，未收到任何后台解包或渲染 Task）。
- 画面每一帧执行 191 次 NVN 调用但持续 0 Draw Calls。

### 2. 本次修复实施
1. **HID 控制器绑定与样式更新修复 (`dm/host/hle_hid.c`)**：
   - Switch SDK 中部分标题（如《塞尔达传说：织梦岛》和《异度神剑3》）在 `BindNpadStyleSetUpdateEvent` 事件触发前不会开始轮询控制器输入，导致无法穿透开屏界面。
   - 修复 `connected_id()` 与 `style_of()`：使 `NPAD_HANDHELD` 和 `NPAD_NO1` 均被视作已连接，无论游戏以手持还是 Player 1 轮询均能正确获取状态。
   - 修复 `BindNpadStyleSetUpdateEvent`：对 `NPAD_HANDHELD` 与 `NPAD_NO1` 均触发信号通知。
2. **全局文件访问追踪 (`dm/host/hle_fs.c`)**：
   - 过去缺失文件（`ENOENT`）仅在 `DM_FS=1` 时记录，导致资源包缺失或路径错误被完全掩盖。
   - 修改为无论是否配置 `DM_FS`，任何文件找不到均直接以 `[RED] fs not found: ...` 写入日志。
   - 前 100 次文件打开操作默认输出日志。
3. **自动化全线程回溯转储 (`dm/gfx/nvn/hle_nvn.c`)**：
   - 在第 60 帧（启动约 2 秒）与第 300 帧（启动约 10 秒）自动调用 `svc_thread_dump()`。
   - 自动在 `log.txt` 中输出全部 44 个线程的 PC、寄存器和调用栈，精准定位主线程卡滞的具体指令。
4. **内置自动化按键配置 (`--env-file`)**：
   - 脚本通过 `--env-file` 将 `DM_KEYS` 自动按键序列编译进 IPA Bundle，自动点击 A 键及 Plus 键以穿透可能存在的无视觉提示按键等待。


