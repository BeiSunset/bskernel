#!/system/bin/sh
# ============================================================
#  BSKernel 1.0 Driver Loader
#  6 个内核版本的驱动已经以 base64 内置在本脚本里，
#  运行时自动释放到临时目录，校验后加载，不需要额外文件。
#
#  用法（需要 root）：
#    su -c "sh /sdcard/Download/BSLoaderDriver1.0.sh"
# ============================================================

VERSION="1.0"
CURRENT_VERSION="1.0.0"
# 版本号来源：QQ 分享页（返回 JSON 里的 html_content 就是版本号）
VERSION_URL="${VERSION_URL:-https://sharechain.qq.com/c3cd1e95d9d22ec396533906479e4495?qq_aio_chat_type=2}"

# 你的 TG 频道（可选）：例如 TG_URL="https://t.me/xxx"；留空则界面不显示
TG_URL="${TG_URL:-https://t.me/BAIFRE}"

MODULE_LIST="@@MODULE_LIST@@"
FAMILIES="5.10 5.15 6.1 6.6 6.12 6.18"
MODULES_DIR=""

# 确保能找到 Android 系统自带的 insmod/rmmod/dmesg/base64 等 toybox 命令
PATH="/system/bin:/system/xbin:$PATH"
export PATH

# 脚本自身的绝对路径（用于非 root 时通过 su 重新运行）
case "$0" in
    /*) SELF="$0" ;;
    *)  SELF="$(pwd)/$0" ;;
esac

ESC=$(printf '\033')
RST="${ESC}[0m"
CYAN="${ESC}[1;36m"
GREEN="${ESC}[1;32m"
YELLOW="${ESC}[1;33m"
RED="${ESC}[1;31m"
MAGENTA="${ESC}[1;35m"
TEAL="${ESC}[38;2;110;195;198m"
WARNRED="${ESC}[38;2;210;120;112m"

info() { printf '%b[-] %s%b\n' "$YELLOW" "$*" "$RST"; }
ok()   { printf '%b[√] %s%b\n' "$GREEN" "$*" "$RST"; }
warn() { printf '%b[!] %s%b\n' "$YELLOW" "$*" "$RST"; }
err()  { printf '%b[x] %s%b\n' "$RED" "$*" "$RST"; }

have() { command -v "$1" >/dev/null 2>&1; }

banner() {
    clear 2>/dev/null || printf '\033[2J\033[H'
    printf '%b<<<Welcome to BSKernel %s Driver!>>>%b\n\n' "$YELLOW" "$VERSION" "$RST"
    printf '%b' "$MAGENTA"
    cat <<'BANNER'
   ██████╗ ███████╗██╗██╗     ██╗   ██╗ ██████╗
   ██╔══██╗██╔════╝██║██║     ██║   ██║██╔═══██╗
   ██████╔╝█████╗  ██║██║     ██║   ██║██║   ██║
   ██╔══██╗██╔══╝  ██║██║     ██║   ██║██║   ██║
   ██████╔╝███████╗██║███████╗╚██████╔╝╚██████╔╝
   ╚═════╝ ╚══════╝╚═╝╚══════╝ ╚═════╝  ╚═════╝
BANNER
    printf '%b' "$RST"
    printf '\n%b<<<欢迎使用 BSKernel 驱动加载>>>%b\n\n' "$YELLOW" "$RST"
    if [ -n "$TG_URL" ]; then
        printf '%b[!] 唯一官方 TG 地址如下，除以下地址外均为非官方：%b\n' "$MAGENTA" "$RST"
        printf '%b    %s%b\n' "$TEAL" "$TG_URL" "$RST"
        printf '%b[!] 请仔细核对用户名，谨防相似名称的仿冒频道。%b\n\n' "$WARNRED" "$RST"
    fi
    printf '%b[!] 本人无任何收费项目，请不要轻易相信陌生人。%b\n\n' "$WARNRED" "$RST"
}

usage() {
    cat <<EOF
BSKernel $VERSION 驱动加载

用法:
  sh BSLoaderDriver1.0.sh            交互式选择内核版本并加载
  sh BSLoaderDriver1.0.sh --auto     自动按当前内核选择最接近的方案并加载
  sh BSLoaderDriver1.0.sh --list     列出内置的驱动模块
  sh BSLoADERDriver1.0.sh --stage 1     分阶段诊断（1=仅兼容层 2=+内存 3=+文件 4=+hooks）
  sh BSLoaderDriver1.0.sh --no-verify 跳过内置驱动的 sha256 校验
  sh BSLoaderDriver1.0.sh --verbose   开启内核详细日志（默认静默，仅调试用）
  sh BSLoaderDriver1.0.sh --help     显示本帮助

说明: 驱动已内置在本 .sh 文件内部，运行时自动释放到临时目录，无需解压。
需要 root 权限；运行时会自动检测并提示。
EOF
}

hash_file() {
    if have sha256sum; then
        sha256sum "$1" 2>/dev/null | awk '{print $1}'
    elif have busybox; then
        busybox sha256sum "$1" 2>/dev/null | awk '{print $1}'
    elif have shasum; then
        shasum -a 256 "$1" 2>/dev/null | awk '{print $1}'
    elif have openssl; then
        openssl dgst -sha256 "$1" 2>/dev/null | awk '{print $NF}'
    else
        printf ''
    fi
}

module_vermagic() {
    [ -f "$1" ] || return 0
    tr '\000' '\n' < "$1" 2>/dev/null | sed -n 's/^vermagic=//p' | head -n1
}

decode_to() {
    out=$1
    if have base64; then
        base64 -d > "$out"
    elif have busybox; then
        busybox base64 -d > "$out"
    elif have openssl; then
        openssl base64 -d > "$out"
    else
        return 127
    fi
}

make_workdir() {
    if [ -n "${BSKERNEL_WORKDIR:-}" ]; then
        MODULES_DIR="$BSKERNEL_WORKDIR"
        mkdir -p "$MODULES_DIR" || exit 1
        return 0
    fi
    for base in /data/local/tmp "${TMPDIR:-/tmp}" /dev; do
        [ -d "$base" ] || continue
        if have mktemp; then
            dir=$(mktemp -d "$base/bskernel.XXXXXX" 2>/dev/null) || dir=""
            if [ -n "$dir" ]; then
                MODULES_DIR="$dir"
                return 0
            fi
        fi
        dir="$base/bskernel.$$"
        if mkdir -p "$dir" 2>/dev/null; then
            MODULES_DIR="$dir"
            return 0
        fi
    done
    err "无法创建临时目录（请用 root 运行）"
    exit 1
}

cleanup() {
    if [ -n "${MODULES_DIR:-}" ] && [ -d "$MODULES_DIR" ]; then
        rm -rf "$MODULES_DIR" 2>/dev/null
    fi
}

cleanup_leftovers() {
    for d in /data/local/tmp/bskernel.*; do
        [ -d "$d" ] || continue
        case "$d" in
            */bskernel.$$) continue ;;
        esac
        rm -rf "$d" 2>/dev/null
    done
}

@@EXTRACT_MODULES@@

verify_modules() {
    if [ "$SKIP_VERIFY" -eq 1 ]; then
        warn "已跳过驱动完整性校验（--no-verify）"
        return 0
    fi
    total=0
    bad=0
    while read -r hash name; do
        [ -z "$hash" ] && continue
        total=$((total + 1))
        file="$MODULES_DIR/$name"
        if [ ! -f "$file" ]; then
            err "缺少文件：$name"
            bad=$((bad + 1))
            continue
        fi
        actual=$(hash_file "$file")
        if [ -z "$actual" ]; then
            err "系统里找不到可用的 sha256 校验工具"
            exit 1
        fi
        if [ "$actual" != "$hash" ]; then
            err "校验失败：$name"
            bad=$((bad + 1))
        fi
    done <<'EOF_HASHES'
@@HASH_LINES@@
EOF_HASHES
    if [ "$bad" -ne 0 ]; then
        err "驱动文件损坏或被修改（$bad/$total）"
        exit 1
    fi
    ok "驱动完整性验证通过（$total/$total）"
}

check_root() {
    [ "$(id -u)" -eq 0 ] && return 0
    if have su; then
        info "需要 root 权限，正在通过 su 重新运行..."
        exec su -c "sh '$SELF'"
    fi
    err "需要 root 权限才能加载内核模块"
    printf '    请用 root 运行，例如：\n'
    printf "      su -c \"sh '%s'\"\n" "$SELF"
    exit 1
}

check_environment() {
    if [ "$(uname -m)" != "aarch64" ]; then
        err "当前架构为 $(uname -m)，本脚本只适用于 arm64/aarch64 设备"
        exit 1
    fi
    if ! have insmod; then
        err "当前环境没有 insmod 命令，请在 Android 设备的 root shell 中运行"
        exit 1
    fi
    if ! have base64 && ! have busybox && ! have openssl; then
        err "当前环境缺少 base64 解码工具"
        exit 1
    fi
}

fetch_version() {
    if have curl; then
        curl -fsSL --max-time 6 -A 'Mozilla/5.0' "$VERSION_URL" 2>/dev/null \
            | sed -n 's/.*"html_content":"\([^"]*\)".*/\1/p' | head -n1
    elif have wget; then
        wget -qO- --timeout=6 -U 'Mozilla/5.0' "$VERSION_URL" 2>/dev/null \
            | sed -n 's/.*"html_content":"\([^"]*\)".*/\1/p' | head -n1
    fi
}

check_version() {
    remote=$(fetch_version)
    if [ -z "$remote" ]; then
        warn "无法连接版本服务器，已跳过版本验证"
        return 0
    fi
    remote=$(printf '%s' "$remote" | tr -d '[:space:]')
    case "$remote" in
        *[!0-9.]*|"")
            warn "版本号格式无效，已跳过版本验证"
            return 0
            ;;
    esac
    if [ "$remote" = "$CURRENT_VERSION" ]; then
        ok "驱动版本验证通过,当前已是最新版本!"
        return 0
    fi
    newer=$(awk -v a="$CURRENT_VERSION" -v b="$remote" 'BEGIN {
        n = split(a, x, "."); m = split(b, y, ".");
        for (i = 1; i <= 3; i++) {
            xi = (i <= n) ? x[i] + 0 : 0;
            yi = (i <= m) ? y[i] + 0 : 0;
            if (yi > xi) { print "1"; exit }
            if (yi < xi) { print "0"; exit }
        }
        print "0"
    }')
    if [ "$newer" = "1" ]; then
        err "驱动版本验证失败,请更新最新版本!"
        return 1
    fi
    ok "驱动版本验证通过,当前已是最新版本!"
    return 0
}
setup_kernel() {
    KERNEL_RELEASE=$(uname -r)
    KERNEL_MM=$(printf '%s' "$KERNEL_RELEASE" | cut -d. -f1,2)
    KERNEL_ANDROID=$(printf '%s' "$KERNEL_RELEASE" | grep -o 'android[0-9][0-9]*' 2>/dev/null | head -n1)

    REC_FAMILY=""
    REC_KMI=""
    for fam in $FAMILIES; do
        [ "$fam" = "$KERNEL_MM" ] && REC_FAMILY="$fam"
    done
    if [ -n "$KERNEL_ANDROID" ] && [ -n "$REC_FAMILY" ]; then
        target="bs-$KERNEL_ANDROID-$REC_FAMILY.ko"
        if [ -n "$MODULES_DIR" ] && [ -f "$MODULES_DIR/$target" ]; then
            REC_KMI="$KERNEL_ANDROID-$REC_FAMILY"
        else
            for name in $MODULE_LIST; do
                [ "$name" = "$target" ] && REC_KMI="$KERNEL_ANDROID-$REC_FAMILY"
            done
        fi
    fi
}

count_family() {
    n=0
    for f in "$MODULES_DIR"/bs-*-"$1".ko; do
        [ -f "$f" ] && n=$((n + 1))
    done
    printf '%s' "$n"
}

count_desc() {
    case "$1" in
        0) printf '（安装包未内置）' ;;
        1) printf '（一个子方案）' ;;
        2) printf '（二个子方案）' ;;
        *) printf '（%s 个子方案）' "$1" ;;
    esac
}

family_by_index() {
    i=1
    for fam in $FAMILIES; do
        if [ "$i" -eq "$1" ]; then
            printf '%s' "$fam"
            return 0
        fi
        i=$((i + 1))
    done
    return 1
}

rec_index() {
    [ -z "$REC_FAMILY" ] && return 0
    i=1
    for fam in $FAMILIES; do
        if [ "$fam" = "$REC_FAMILY" ]; then
            printf '%s' "$i"
            return 0
        fi
        i=$((i + 1))
    done
}

show_menu() {
    i=1
    for fam in $FAMILIES; do
        cnt=$(count_family "$fam")
        desc=$(count_desc "$cnt")
        printf '%b[%d] 内核版本：%-6s        %s%b\n' "$MAGENTA" "$i" "$fam" "$desc" "$RST"
        i=$((i + 1))
    done
    printf '%b[7] 退出程序%b\n' "$MAGENTA" "$RST"
}

pick_module() {
    fam=$1
    cnt=$(count_family "$fam")
    [ "$cnt" -eq 0 ] && return 1

    if [ "$cnt" -eq 1 ]; then
        for f in "$MODULES_DIR"/bs-*-"$fam".ko; do
            if [ -f "$f" ]; then
                printf '%s' "$f"
                return 0
            fi
        done
    fi

    printf '\n请选择子方案：\n' >&2
    i=1
    for f in "$MODULES_DIR"/bs-*-"$fam".ko; do
        [ -f "$f" ] || continue
        kmi=${f##*/bs-}
        kmi=${kmi%.ko}
        mark=""
        [ "$kmi" = "$REC_KMI" ] && mark=""
        printf '  [%d] %s%s\n' "$i" "$kmi" "$mark" >&2
        i=$((i + 1))
    done
    printf '  [0] 返回\n' >&2
    printf '[-] 请输入子方案编号: ' >&2
    if ! read sub; then
        return 1
    fi
    [ -z "$sub" ] && sub=1
    case "$sub" in
        0) return 2 ;;
        *[!0-9]*) return 1 ;;
    esac
    i=1
    for f in "$MODULES_DIR"/bs-*-"$fam".ko; do
        [ -f "$f" ] || continue
        if [ "$i" -eq "$sub" ]; then
            printf '%s' "$f"
            return 0
        fi
        i=$((i + 1))
    done
    return 1
}

load_module() {
    module=$1

    if grep -q '^bs ' /proc/modules 2>/dev/null; then
        cleanup
        ok "内核模块加载成功!"
        return 0
    fi

    output=$(insmod "$module" $PARAMS 2>&1)
    rc=$?
    if [ "$rc" -ne 0 ] || ! grep -q '^bs ' /proc/modules 2>/dev/null; then
        cleanup
        err "内核模块加载失败!"
        return 1
    fi

    cleanup
    ok "内核模块加载成功!"
    return 0
}

# ---------- main ----------
AUTO=0
SKIP_VERIFY=0
LIST_ONLY=0
VERBOSE=0
PARAMS=""
while [ $# -gt 0 ]; do
    case "$1" in
        --auto|-a) AUTO=1 ;;
        --no-verify|-n) SKIP_VERIFY=1 ;;
        --verbose|-v) VERBOSE=1 ;;
        --stage)
            shift
            if [ -z "${1:-}" ]; then
                err "--stage 需要指定阶段（1-4）"
                exit 2
            fi
            PARAMS="bs_stage=$1"
            ;;
        --list) LIST_ONLY=1 ;;
        --help|-h) usage; exit 0 ;;
        *) err "未知参数：$1"; usage; exit 2 ;;
    esac
    shift
done

if [ "$VERBOSE" -eq 1 ]; then
    PARAMS="$PARAMS bs_log_verbose=1"
fi

if [ "$LIST_ONLY" -eq 1 ]; then
    check_environment
    setup_kernel
    printf '内置驱动模块：\n'
    for name in $MODULE_LIST; do
        printf '  - %s\n' "$name"
    done
    printf '\n当前内核：%s\n' "$(uname -r)"
    exit 0
fi

trap 'cleanup' EXIT INT TERM

banner
check_root
umask 077
check_environment
cleanup_leftovers
make_workdir
extract_modules || exit 1
setup_kernel

info "正在验证内置驱动..."
verify_modules
info "正在向服务器验证驱动版本请稍后..."
check_version || exit 1
printf '\n%b[-] 你的当前内核版本是：%b%s\n\n' "$TEAL" "$RST" "$KERNEL_RELEASE"

if [ "$AUTO" -eq 1 ]; then
    if [ -z "$REC_FAMILY" ]; then
        err "自动模式：当前内核 $KERNEL_MM 不在支持列表（5.10/5.15/6.1/6.6/6.12/6.18）"
        exit 1
    fi
    module=$(pick_module "$REC_FAMILY")
    rc=$?
    if [ "$rc" -ne 0 ] || [ -z "$module" ]; then
        err "自动模式：未找到 $REC_FAMILY 的驱动模块"
        exit 1
    fi
    load_module "$module" && exit 0
    exit 1
fi

while :; do
    printf '%b请根据当前内核版本，从列表中选择最接近的驱动方案安装：%b\n\n' "$YELLOW" "$RST"
    show_menu
    printf '\n[-] 请输入选项数字并回车：'
    if ! read choice; then
        err "输入已结束，程序退出"
        exit 1
    fi

    if [ -z "$choice" ]; then
        r=$(rec_index)
        [ -n "$r" ] && choice=$r
    fi

    if [ "$choice" = "7" ]; then
        exit 0
    fi

    fam=$(family_by_index "$choice" 2>/dev/null)
    if [ -z "$fam" ]; then
        err "无效选项：$choice"
        sleep 1
        continue
    fi

    module=$(pick_module "$fam")
    rc=$?
    if [ "$rc" -eq 2 ]; then
        continue
    fi
    if [ "$rc" -ne 0 ] || [ -z "$module" ]; then
        err "该内核版本没有内置对应的驱动模块"
        continue
    fi

    load_module "$module" && exit 0
    exit 1
done
