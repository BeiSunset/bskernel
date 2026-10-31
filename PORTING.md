# 移植 / 适配笔记（Porting & device adaptation notes）

本文记录真实设备上踩过的适配坑，供以后适配新机型、新 SoC、新内核版本时对照。
新代码提交前请逐条检查；CI 已把其中可自动检查的部分变成硬性门禁。

## 硬性规则

### 1. 模块内绝对不要出现 per-CPU 变量（最高优先级）

- 禁止：`DEFINE_PER_CPU`、`this_cpu_*`、`per_cpu()`、`alloc_percpu()`。
- 真实事故（2026-10-31，v1.0.6 构建）：
  - `bs_hwb.c` 用 `DEFINE_PER_CPU` 保存被内核占用的调试槽位（536 B）；
  - `udp_capture.c` 用 per-CPU scratch 指针（8 B）。
  - 一加 Ace 6（`6.6.118-android15-8-g93e22c376e7-abogki500782043-4k`）加载时报：
    `bs: Could not allocate 536 bytes percpu data`
    `finit_module` 返回 `-ENOMEM (12)`。
  - 关键点：**模块 init 根本没有执行**，dmesg 里看不到任何模块自己的 `pr_err`，只有内核装载器的这一行。
- 替代做法：`kvzalloc(sizeof(*p) * num_possible_cpus(), GFP_KERNEL)`，
  用 `smp_processor_id()` 索引；访问点必须保证 CPU 不迁移
  （持 spinlock / 关中断，或处于 kprobe、异常上下文）。
- 自检：构建产物不应包含**我们自己的** per-CPU 数据。CI 会检查 `.percpu` 段里的符号：
  - 允许 `*_alloc_tag_cntr`（6.12 内核 `CONFIG_MEM_ALLOC_PROFILING` 自动生成的分配统计计数器）
    和 `$d.*` 零长占位符；
  - 出现其它任何符号即判定为"代码里写了 per-CPU 变量"，直接构建失败。
- 手工复查：`llvm-readelf -S bs.ko | grep -E '\.percpu'`；
  再用 `llvm-readelf -s bs.ko` 看该段内的符号名。

### 2. 日志策略

- 信息类日志在发布构建中编译为空（`bs_info`/`bs_warn` 展开为 no-op），只保留 `pr_err`。
- 但**加载/初始化失败必须有 `pr_err`**，否则远程排障只能靠用户在群里截图猜。
- 现象判断：如果 `dmesg` 里完全没有模块输出，说明失败发生在模块 init 之前
  （模块装载阶段），优先怀疑 per-CPU、签名、vermagic、模块内存。

### 3. 符号与特性

- 一律通过 kallsyms 动态解析，不硬依赖导出符号；缺失时 fail-closed 并打印符号名。
- 依赖 `CONFIG_KALLSYMS_ALL` 的数据符号（`memblock`、`init_mm`、`_stext` 等）
  要做存在性检查，不能用"加载到就算有"。

### 4. 卸载安全

- 内核 text 补丁、`file->f_op` 替换、kprobe：安装期持模块引用，彻底还原后才释放；
  还原失败保持引用（`rmmod` 返回 `-EBUSY`），绝不带悬空跳转卸载。
- 用户侧因此可能出现"注入期间无法卸载"，这是有意的安全取舍，不是 bug。

### 5. 大内存机型

- 线性映射（NC / cached alias）成本随 RAM 线性增长（页表 + 临时数组）。
- 12/16 GB 机型要注意分配失败，失败时应能降级而不是让整个模块加载失败。

## 常见错误码速查

| errno | 常见原因 | 排查 |
|---|---|---|
| `-ENOMEM (12)` | 模块 per-CPU 分配失败（本事故）；或 init 内分配失败 | `dmesg | grep -E "percpu|mem_core|compat|file_io"` |
| `-ENOENT (2)` | 缺符号 / kprobe 注册失败 / 目标不存在 | dmesg `missing required symbol` |
| `-EKEYREJECTED (129)` | 内核强制模块签名 | 需关闭校验或提供签名 |
| `-ENOEXEC (8)` | vermagic / KMI 不匹配 | 换对应 KMI 构建 |
| `-EBUSY (16)` | 旧模块未卸载；hwb 还原失败保持 pin | `rmmod` / 重启 |
| `-EPERM (1)` | SELinux / LSM 拒绝 | `dmesg | grep avc` |

## 新设备适配流程

1. `uname -a` 确认内核版本与 KMI，装对应 KMI 构建；确认 `/sys/module/bs/version`。
2. 失败时：`dmesg -C` → 重新加载 → `dmesg | tail -50`
   （不要直接 `dmesg | tail`，会被充电/相机日志淹没）。
3. 用加载器 `--stage 1/2/3` 二分定位 init 阶段（compat / linearmap / file_io）。
4. 收集内核配置：`zcat /proc/config.gz | grep -E "KPROBES|KALLSYMS|MODULE_SIG|VA_BITS|SMP"`；
   记录 `MemTotal`。
5. 确认构建产物：无 `.percpu` 段、无信息类日志字符串。
6. 把机型、内核字符串、内存大小补进下方"已适配记录"。

## 已适配记录

| 设备 / 内核 | 结果 | 备注 |
|---|---|---|
| `6.6.118-android15-8-gb9cc6ec16bc8-abogki536571621-4k` | OK | 开发机，6.6 |
| OnePlus Ace 6，`6.6.118-android15-8-g93e22c376e7-abogki500782043-4k` | 需 percpu-free 构建 | 旧构建 `-ENOMEM`；去掉模块 per-CPU 后加载成功 |