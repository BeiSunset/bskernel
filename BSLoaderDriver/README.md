# BSLoADERDriver

`BSLoaderDriver` 是加载 `bs-*.ko` 内核驱动的手机端脚本。仓库里保存的是**源码模板和构建脚本**，最终的 `BSLoaderDriver.sh` 由 GitHub Actions 在打包阶段自动生成并作为 Release 附件发布，不在本地维护成品。

## 目录

```
BSLoaderDriver/
├── BSLoADERDriver.template.sh   # 加载器 UI/逻辑模板（不含驱动二进制）
└── build_single.py              # 把 bs-*.ko 用 base64 内嵌进模板，生成单文件 .sh
```

## 本地生成

前提：已经通过构建产出 `.ko`（例如 Actions 的 Artifact，或本地 Docker 构建）。

```bash
python3 BSLoADERDriver/build_single.py out/bskernel-arm64-v8a/modules BSLoADERDriver.sh
chmod 755 BSLoADERDriver.sh
```

生成后把 `BSLoaderDriver.sh` 传到手机，root 下运行：

```bash
su -c "sh /sdcard/Download/BSLoaderDriver.sh"
```

## 在手机上执行后的行为

- 释放内置驱动到 `/data/local/tmp/bskernel.<pid>/`；
- 校验 sha256（`6/6`）；
- 从仓库 `version.txt` 读取版本号（默认 `1.0.0`，支持 GitHub Raw 和 jsDelivr 回退）；
- 按菜单选择 KMI 后 `insmod`，默认对 6.6 使用 `bs_stage=6`（跳过会重启的 syscall hook）；
- 加载成功或失败后立即删除临时目录；启动时也会清理旧的 `/data/local/tmp/bskernel.*` 残留。

## 由 CI 生成

`.github/workflows/build-kernel.yml` 的 `package` job 会：

1. 下载 6 个 KMI 的 `.ko`；
2. 运行本目录的 `build_single.py`；
3. 把生成的 `BSLoaderDriver.sh` 和 `.sha256` 上传到 Release。

因此每次改完源码 push 后，Release 里都会自动出现最新的加载器，本地不需要保留源码或成品。
