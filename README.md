# bskernel

基于 [enenH/enkernel](https://github.com/enenH/enkernel) 改名（`enkernel` → `bskernel`）的 Android 内核模块项目，附带 GitHub Actions 自动编译。

> **重要：本仓库不是完整的 Android 内核源码树**，而是一个 *out-of-tree* 内核模块 `dfm.ko`（obj-m，见根目录 `Makefile`）和配套的 Android arm64 SDK。
> 你参考的那次 Actions 运行（[enenH 例子](https://github.com/enenH/enkernel/actions/runs/40145185222/job/121024888969)）实际编译的是 **`dfm.ko` 模块**，不是 `boot.img`。模块面向 **Android 15（GKI 6.6）** 内核，加载时需要 root。

---

## 一、构建产物长什么样

| 产物 | 在哪下载 | 用途 |
| --- | --- | --- |
| `dfm-android15-6.6.ko` | Actions 的 Artifact `dfm-kernel-android15-6.6`；或 Release 里的 `dfm-android15-6.6.ko.zip` | 加载到 Android 15 / 6.6 GKI 内核的模块 |
| `dfm-android15-6.6.ko.sha256` | 同上 | 校验文件完整性 |
| `bskernel-arm64-v8a.tar.gz` | Release | 全部 6 个 KMI 的模块 + SDK（loader/示例/头文件） |
| `bskernel-sdk-arm64-v8a.tar.gz` | Release | 仅 SDK |

Actions 的 matrix 会并行编 6 个 KMI（android12-5.10 ~ android17-6.18），整个运行约 **2~3 分钟**，只取你需要的 android15-6.6 那个 Artifact 即可。

---

## 二、把源码部署到 GitHub（三选一）

GitHub Actions 必须跑在 GitHub 仓库里，所以先把源码传上去。仓库 Public / Private 都可以（Public 的 Actions 分钟数免费）。

### 方式 A：GitHub Desktop（推荐，能完整保留 `.github` 隐藏目录）

1. 安装 [GitHub Desktop](https://desktop.github.com/download/)，登录 GitHub 账号。
2. 菜单 `File` → `Add local repository…`，选择源码目录（例如 `D:\bskernel-main`）。
3. 提示 "This directory does not appear to be a Git repository" 时点 **create a repository**。
4. 点右上角 **Publish repository**，填仓库名（例如 `bskernel`），需要公开就取消勾选 `Keep this code private`，点 **Publish repository**。
5. 上传完成后，以后改代码只需在 GitHub Desktop 里填 Summary → **Commit to main** → **Push origin**。

### 方式 B：网页上传（不装任何软件，推荐分两步，避免漏掉 `.github`）

1. GitHub 右上角 `+` → **New repository**，填仓库名（例如 `bskernel`），Public/Private 随意，**不要**勾选 "Add a README file"，点 **Create repository**。
2. 在仓库页点 **Add file** → **Upload files**，把源码目录里**除 `.github` 文件夹以外**的所有文件和文件夹（`sdk` 整个目录、`Makefile`、`*.c`、`*.h`、`CMakeLists.txt`、`.gitignore`、`README.md` 等）拖进去，点 **Commit changes**。
   - 根目录的 `.gitignore` 也是隐藏文件：先在资源管理器里打开"显示隐藏的项目"，否则容易漏传。
   - 网页一次最多 100 个文件、单文件 25 MB，本仓库只有约 40 个文件，没问题。
3. 再点 **Add file** → **Create new file**，文件名填：

   ```
   .github/workflows/build-kernel.yml
   ```

   把本仓库 `.github/workflows/build-kernel.yml` 的**全部内容**粘贴进去（本地可用记事本打开复制），点 **Commit changes**。
   - 这一步提交后，Actions 会自动开始构建（因为工作流配置了 `on: push`）。
   - 顺序很重要：先把源码传完，最后再创建 workflow 文件，这样只会触发一次有效构建。

### 方式 C：Git 命令行

安装 [Git for Windows](https://git-scm.com/download/win)，然后在源码目录执行：

```bat
cd /d D:\bskernel-main
git init
git add -A
git commit -m "Initial bskernel source"
git branch -M main
git remote add origin https://github.com/<你的用户名>/bskernel.git
git push -u origin main
```

首次 push 会弹出浏览器让你登录 GitHub。之后改了代码：`git add -A && git commit -m "update" && git push`。

---

## 三、让 Actions 能发布 Release（一般不用设置）

工作流的 `package` 任务在 job 级声明了 `permissions: contents: write`，GitHub 允许用它创建 Release，所以**正常情况下不用改任何设置**。注意：2023 年后新建仓库的 `GITHUB_TOKEN` 默认只读，但工作流里的 job 级权限会覆盖默认值（仅 fork 来的 PR 会被强制降为只读，不影响你自己 push 触发的构建）。

只有在 **`package` 任务报 403 / `Resource not accessible by integration`** 时才需要手动打开：

1. 仓库 `Settings` → `Actions` → `General`；
2. 找到 **Workflow permissions**，选择 **Read and write permissions**；
3. 点 **Save**。

另外：

- 自建仓库默认启用 Actions，一般不用动；
- 如果你是 **Fork** 别人的仓库，经验上需要在 `Actions` 标签页点 **I understand my workflows, go ahead and enable them** 才能跑；
- 如果只想拿 Artifact、不需要 Release，也可以把 `package` 任务删掉（不推荐，Release 里下载 `.ko` 最方便）。

---

## 四、运行构建并下载 android15-6.6 模块

1. 打开仓库的 **Actions** 标签页；
2. 左侧选 **Build and release kernel and SDK**；
3. 第一次可点右侧 **Run workflow** → 分支选 `main` → **Run workflow**（如果刚 push 过，已经自动跑起来了，不用再点）；
4. 等运行变成绿色，点进去找到名字为 **Build dfm.ko for android15-6.6** 的任务，底部 **Artifacts** 里下载 `dfm-kernel-android15-6.6.zip`；
5. 或者等 `Package and publish release` 完成后，到仓库 **Releases** 页下载 `dfm-android15-6.6.ko.zip`（里面就是 `dfm-android15-6.6.ko`，旁边有 `.sha256`）。

> 每个 Artifact 是一个 zip，GitHub 会再包一层，解压后才是 `dfm-android15-6.6.ko`。

---

## 五、本地编译（可选，不依赖 GitHub Actions）

本模块是 out-of-tree 编译：`make -C $KDIR M=$PWD modules`。需要 Linux 内核头文件（KDIR）和 clang。最省事的办法是用 DDK 容器镜像（CI 用的就是它），Windows 上装 Docker Desktop 即可。

### Windows + Docker Desktop

1. 安装 [Docker Desktop](https://www.docker.com/products/docker-desktop/)，安装时启用 WSL2 后端，启动后等鲸鱼图标变绿；
2. 打开 PowerShell，执行：

```powershell
# 拉取 Android 15 / 6.6 的 DDK 镜像（压缩下载约 2 GB，只需一次）
docker pull ghcr.io/ylarod/ddk:android15-6.6-20260828

# 进源码目录编译
cd D:\bskernel-main
docker run --rm --privileged -v "${PWD}:/build" -w /build `
  ghcr.io/ylarod/ddk:android15-6.6-20260828 `
  bash -lc 'make -j$(nproc) KDIR=$KERNEL_SRC && ls -lh dfm.ko'
```

编译成功后，`D:\bskernel-main\dfm.ko` 就是 Android 15 / 6.6 的模块；需要的话自己再按 KMI 命名（`dfm-android15-6.6.ko`）。

### Linux / WSL2 Ubuntu

```bash
cd /path/to/bskernel
docker run --rm --privileged -v "$PWD:/build" -w /build \
  ghcr.io/ylarod/ddk:android15-6.6-20260828 \
  bash -lc 'make -j$(nproc) KDIR=$KERNEL_SRC && ls -lh dfm.ko'
```

### 说明

- `KERNEL_SRC` 是 DDK 镜像里预置的 6.6 内核头文件目录，镜像里已 export，不用自己设；
- 镜像 tag 的规则是 `<KMI>-<日期>`，CI 固定用 `20260828`，本地也可以直接用 `android15-6.6`（跟随最新）；换 KMI 就换 tag；
- 国内访问 ghcr.io 慢/失败时，DDK 官方 README 提供了国内镜像源 `docker.cnb.cool/ylarod/ddk/ddk`（见 [DDK 文档](https://github.com/Ylarod/ddk)），拉下来后把上面的镜像名替换掉即可；CNB 镜像的具体 tag 未逐一验证，若同名 tag 不存在就选镜像页上的最新可用 tag；
- 源码路径尽量不含空格/中文（例如放在 `D:\bskernel\`），避免 Docker 挂载/路径转义问题；
- 没有 Docker 也不想装：直接用 GitHub Actions 编，效果一样，还不用配环境。

IDE 里写代码需要索引时，可参考 DDK 的 Host 模式装到 Linux 的 `/opt/ddk`。

---

## 六、如果你要的是"完整 Android 15 内核"

本仓库**不包含**内核源码，无法编译 `boot.img`。要编完整 Android 15 GKI 6.6 内核，走 AOSP 官方流程（需要 Linux、约 100 GB 磁盘、repo + bazel）：

```bash
mkdir android15-6.6 && cd android15-6.6
repo init -u https://android.googlesource.com/kernel/manifest -b common-android15-6.6
repo sync -c -j8
tools/bazel build //common:kernel_aarch64_dist
```

以官方文档为准：<https://source.android.com/docs/setup/build/building-kernels>。
只有当你需要改内核本身（而不是加载 `dfm.ko`）时才需要这一步。

---

## 七、把模块装到手机（可选）

前提：手机已 root，系统内核是 **6.6 GKI** 且 KMI 匹配，内核允许加载未签名模块（部分官方内核会强制模块签名，需要第三方内核/ROM）。

```bash
adb push dfm-android15-6.6.ko /data/local/tmp/
adb shell su -c "insmod /data/local/tmp/dfm-android15-6.6.ko"
adb shell su -c "dmesg | tail -n 40"
```

卸载：`adb shell su -c "rmmod dfm"`。也可以用 Release 里 SDK 自带的 `dfm_loader` 加载。

> DDK 官方明确说明：用 DDK 头文件编译的模块**不保证 100% 兼容**所有同版本内核，遇到 `Invalid module format` / `version magic` 不一致时，需要用你设备内核对应的完整源码/头文件重新编译。

---

## 八、本次改名修复说明

上游 `enkernel` 全套改名为 `bskernel` 时，有一个**文件名**没改到：`sdk/cmake/enkernelConfig.cmake.in`。而 `sdk/CMakeLists.txt` 已经改成引用 `cmake/bskernelConfig.cmake.in`，两者对不上会让 `sdk` 任务在 CMake 配置阶段报"找不到 bskernelConfig.cmake.in"。

本仓库已修复：文件已重命名为 `sdk/cmake/bskernelConfig.cmake.in`，内容里的 `bskernelTargets.cmake` / `check_required_components(bskernel)` 也已一致。除新增的 `README.md` 和改名后的这个文件外，其余文件与上游逐字节一致（仅 `enkernel`↔`bskernel` 文本替换）。

---

## 九、常见问题

| 现象 | 原因 / 处理 |
| --- | --- |
| `package` 任务失败，403 / `Resource not accessible by integration` | 仓库 Settings → Actions → General → Workflow permissions 改成 **Read and write** |
| 看不到 Actions 运行 | 仓库是 fork，去 Actions 页点启用；或检查 `.github/workflows/build-kernel.yml` 是否真的传上去了（路径不能错） |
| `sdk` 任务报找不到 `bskernelConfig.cmake.in` | 你用的还是改名前的老源码，用本仓库版本（已修复） |
| `Initialize containers` 卡住/失败 | Runner 拉取 `ghcr.io/ylarod/ddk:...` 失败，重试即可；自建 runner 需能访问 ghcr |
| 只想要 android15-6.6 | 从对应 job 的 Artifact 下载即可；不要随便删 matrix，`package` 任务固定校验 6 个 KMI，删了会导致打包失败 |
| 模块加载报 `Invalid module format` | KMI/内核版本不匹配或模块签名限制，需用设备内核源码重编，见第七节说明 |
| 编译出来的 `dfm.ko` 没有调试信息 | 正常，CI 会执行 `llvm-strip --strip-debug` 后再发布 |

---

## 十、构建原理（为什么这样能编）

- `Makefile`：`obj-m := dfm.o`，通过 `make -C $KDIR M=$PWD modules` 把 `.c` 编成 `dfm.ko`；
- DDK 镜像内置了对应 KMI 的**预编译内核头文件**、clang/llvm 工具链，并导出 `KERNEL_SRC`，所以不需要自己下载完整内核源码；
- CI 用 matrix 把 6 个 KMI 各跑一次（每个 job 一个容器），产物统一收集后打包、生成 sha256、发布 Release；
- SDK 是 Android NDK 编译的 arm64 用户态库/工具（`dfm_loader`、`dfm_example`），与模块分开构建。
