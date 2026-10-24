# Xenoblade Chronicles 3 (异度神剑3) iOS 原生移植进展与交接文档

> **项目目标**：在 Apple Silicon (iOS / Metal) 上通过 `nxnative` 纯原生运行《异度神剑3》（Title ID `010074F013262000`，版本 v1.0.0，SDK 12.3.9）。

---

## 一、 当前运行状态与症状

1. **游戏能够正常引导并以稳定的 30 fps 运行**：
   - 音频引擎（Wwise）正常加载并播放背景音乐（BGM）和音效（SE），设备回调 40,000+ 次无 underrun。
   - NVN 图形管线正常循环，每帧执行 30 个 NVN 函数（191 次调用）。
2. **当前唯一阻碍：画面持续全屏纯黑（0 Draw Calls）**：
   - 第 4 帧提交了全屏淡入淡出的纯黑 Quad（`vs=58e2e00d24c6f3ea_b`, `fs=5d36d4144f49ec9a_b`）。
   - 从第 5 帧起，所有 3D 和 2D 绘制完全没有提交（`CommandBufferDrawArrays=0`, `CommandBufferDrawElements=0`），每帧仅清屏为黑色 `(0, 0, 0, 1)`。

---

## 二、 关键问题根因与已解决项目

### 1. 已解决：显存池（MemoryPool）Guest 侧 OOM 分配失败
- **原因**：XC3 使用现代 Switch 的 **VirtualAddressMemory** 架构（在 Alias 区域分配），原 Switch 代码申请 60 GB，此前被裁剪为 3.0 GB 总量、2.0 GB Arena，导致处理 >64 KB 的大块分配空间仅剩 1.0 GB。游戏初始需要分配两个 512 MB 显存池 Chunk（410 MB 主纹理堆 + 420 MB 渲染目标池），直接超出 1.0 GB 触发 OOM 返回 NULL。
- **已实施修复**（`dm/tools/patch_nso.py`）：
  - `0xfa188c`: 虚拟地址空间总容量扩大至 3.25 GB (`mov x0, #0xd0000000`)。
  - `0xf55010~0xf55088`: Arena 小对象容量从 2.0 GB 缩减至 1.0 GB (`0x40020000`)，将大块空间释放至 **2.25 GB**。
  - `0xfa1b70`: `b #0xfa1b94` 绕过人为设置的 Heap Cap 校验。
  - `game.tsv`: 设置 `op_mode handheld`，触发游戏自带优化，将第二显存池由 420 MB 降至 350 MB。
- **结果**：Guest 内存池完全分配成功，不再有 `[WARN] MemoryPoolInitialize: non-virtual pool has no storage`。

---

### 2. 当前核心阻碍：语言索引错误导致资源文件找不到（`RES_PATH_NOT_FOUND`）

#### (1) 现象（取自真机运行日志）：
```text
[dm] fs path not found: rom:/bdat/us/game/autotalk.bdat
[dm] fs path not found: rom:/bdat/us/game/battle.bdat
[dm] fs path not found: rom:/bdat/us/game/field.bdat
[dm] fs path not found: rom:/bdat/us/game/menu.bdat
[dm] fs path not found: rom:/bdat/us/game/quest.bdat
[dm] fs path not found: rom:/bdat/us/game/system.bdat
[dm] fs path not found: rom:/bdat/us/sortkey.bin
...
[dm] fs path not found: rom:/menu/font/bf3_us.wisty
```

#### (2) 根因：
1. **游戏数据包内根本不存在美版英语（`us`）资源**：
   - 提取并分析 `control.nacp` 与 `bf3.arh`/`bf3.ard`：XC3 全球版只支持 9 种语言：
     - `ja`（日语）、`en-GB`（英式英语）、`fr`、`de`、`es`、`it`、`ko`、`zh-Hant`（繁体中文）、`zh-Hans`（简体中文）。
     - **完全没有 `en-US` / `us`！**
   - 归档内对应的字体是 `bf3_gb.wisty`、`bf3_cn.wisty` 等，数据库是 `bdat/gb/...`、`bdat/cn/...` 等。
2. **Monolith Soft 引擎的语言索引映射**：
   - 游戏内部全局语言变量位于指针 `[0x1774ca0]`：
     - 索引 `0` = `jp`
     - 索引 `1` = `us`（游戏内部硬编码的兜底默认值，但 XC3 没有 `us` 资源！）
     - 索引 `6` / `15` = `cn`（简体中文）
     - 索引 `11` / `16` = `tw`（繁体中文）
     - 索引 `12` = `gb`（英式英语）
   - 字符串表位于 `0x17989d0 + index * 0x14`。
3. **为什么变成 `us`**：
   - 游戏启动初期，语言变量被直接赋予默认值 `1`（`us`）。
   - 游戏尝试读取 `rom:/bdat/us/...` 与 `rom:/menu/font/bf3_us.wisty`，由于文件不存在，Monolith 引擎的 BDAT 数据库和字体系统初始化失败。
   - 字体与系统表未加载，导致所有 UI 和 3D 逻辑无法向下执行，管线不提交任何 Mesh，陷入死循环清屏（每帧 0 Draw Calls）。

---

## 三、 当前代码改动与未生效原因分析

### 1. 已有改动
1. **`dm/host/hle_oe.c`**：
   - 增强了 `_ZN2nn2oe18GetDesiredLanguageEv`，支持读取 `DM_LANG` 环境变量，或自动读取 iOS 系统的语言首选项（`CFLocaleCopyPreferredLanguages`）。
   - 针对 XC3（`0x010074F013262000`），将英语映射为 `en-GB`，中文映射为 `zh-Hans`。
2. **`dm/games/010074F013262000/game.tsv`**：
   - 添加了 `default_lang zh-Hans`。
3. **`dm/tools/patch_nso.py`**：
   - 此前尝试在 `main` 打补丁：
     ```python
     (0x36e7c, '53a33c94', '1ebf3c94', 'language init: bl 0xf5fbc8 → bl 0xf66af4 (call GetDesiredLanguage directly)', 'host-only'),
     (0xf618, 'e80100b4', '179e0094', 'language notification: cbz x8,0xf654 → bl 0x36e74 (unconditional language init)', 'host-only'),
     (0xf61c, '169e0094', 'c80100b4', 'language notification: bl 0x36e74 → cbz x8,0xf654 (branch after language init)', 'host-only'),
     ```

### 2. 为什么上述改动在最新测试中仍然请求 `rom:/bdat/us/...`？
**核心发现**：
1. 日志中没有任何 `[dm] language:` 打印，说明 **`_ZN2nn2oe18GetDesiredLanguageEv` 根本没有被调用到**！
2. 分析 `0xf618`：该函数属于通知 15（系统设置改变）的回调处理逻辑，在游戏启动初期 `[0x1798450]` 尚未绑定，导致该分支在启动阶段并未执行，或者执行时全局指针 `0x1774ca0` 还未初始化。
3. **最直接、最根本的修复切入点**：
   找到读取语言索引或返回语言字符串的函数，或者直接硬编码语言索引！
   - 函数 `0x36ed8`：
     ```arm64
     0x36ed8: adrp x8, #0x1774000
     0x36edc: ldr  x8, [x8, #0xca0]
     0x36ee0: ldr  w0, [x8]
     0x36ee4: ret
     ```
     这是全局获取语言索引的 Getter！无论哪里调用它获取当前语言，如果将 `0x36ed8` 直接 patch 为返回简体中文（`mov w0, #6; ret`）或英式英语（`mov w0, #12; ret`），或者日语（`mov w0, #0; ret`），整个引擎读取语言的地方将全局强制变为对应的合法语言！
   - 或者函数 `0x36f28`（路径字符串解析器，返回 `"cn"` / `"gb"` / `"jp"`）。

---

## 四、 建议接手方案（Action Plan）

### 方案 A：直接 Patch `0x36ed8`（最稳妥、最高效）
在 `dm/tools/patch_nso.py` 中，针对 `main`（Build ID `60887a25b66529910275a1066003a002`）：
将 `0x36ed8`（语言索引 Getter）直接替换为硬编码返回值：
- 返回 `6`（简体中文，对应 `"cn"` 资源）：
  ```arm64
  0x36ed8: mov w0, #6     ; 机器码: c0008052
  0x36edc: ret            ; 机器码: c0035fd6
  ```
  或者返回 `12`（英式英语，对应 `"gb"` 资源）：
  ```arm64
  0x36ed8: mov w0, #12    ; 机器码: 80018052
  0x36edc: ret            ; 机器码: c0035fd6
  ```
  或者返回 `0`（日文，对应 `"jp"` 资源）：
  ```arm64
  0x36ed8: mov w0, #0     ; 机器码: 00008052
  0x36edc: ret            ; 机器码: c0035fd6
  ```
- **效果**：
  引擎所有模块在调用 `0x36ed8` 时，得到的语言 ID 将永远是有效支持的语言（如 `6`），寻找的资源将从 `rom:/bdat/us/...` 变为 `rom:/bdat/cn/...`，字体变为 `rom:/menu/font/bf3_cn.wisty`。
  从 `bf3.ard` 归档解密读取将全部命中，系统表与字体成功加载，渲染管线开始提交 Draw Calls！

### 方案 B：RomFS 软链接 / 资产重定向兜底
在 `hle_fs.c` 的 `fs::OpenFile` 中，若请求包含 `/us/` 或 `_us.`，自动重定向到 `/cn/`（或 `/gb/`）：
```c
if (strstr(path, "/bdat/us/")) {
    // 重定向到 /bdat/cn/ 或 /bdat/gb/
}
if (strstr(path, "bf3_us.wisty")) {
    // 重定向到 bf3_cn.wisty 或 bf3_gb.wisty
}
```

---

## 五、 构建与验证环境说明

1. **环境与关键路径**：
   - 仓库路径：`/Users/mjc626/Documents/nxnative`
   - NSP 路径：`/Users/mjc626/Documents/nsp`
   - Keys 路径：`/Users/mjc626/Library/Mobile Documents/com~apple~CloudDocs/Downloads/ProdKeys.NET-v22.5.0/prod.keys`
   - 输出 IPA 路径：`/Users/mjc626/Desktop/Xenoblade3_slim.ipa`
2. **打包命令**：
   ```bash
   PATH="/opt/local/bin:/opt/local/sbin:$PATH" python3 scripts/build_ipa_from_nsp.py \
     /Users/mjc626/Documents/nsp \
     -k "/Users/mjc626/Library/Mobile Documents/com~apple~CloudDocs/Downloads/ProdKeys.NET-v22.5.0/prod.keys" \
     --no-heap-check \
     --no-romfs \
     -o /Users/mjc626/Desktop/Xenoblade3_slim.ipa
   ```
   > 注：打包前若修改了 `patch_nso.py`，需删除 `/Users/mjc626/Documents/nsp/010074F013262000_build/patched_v0.key` 或删除 `patched_v0/` 目录以强制重新执行 rewrite。
3. **测试验证要点**：
   - 观察 `Documents/log.txt` 中是否不再出现 `rom:/bdat/us/...` 和 `rom:/menu/font/bf3_us.wisty` 报错。
   - 观察是否成功从 `bf3.ard` 加载 `bdat/cn/...` 和 `bf3_cn.wisty`。
   - 观察第 5 帧之后 `CommandBufferDrawArrays` / `CommandBufferDrawElements` 是否开始大量出现，画面开始显示 3D/UI 内容。

