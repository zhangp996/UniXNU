#!/usr/bin/env bash
#
# install-macos-runner.sh — 在 macOS 上安装 GitHub Actions 自托管 runner
# 默认目标仓库: zhangp996/UniXNU
#
# 用法:
#   ./install-macos-runner.sh install --token <TOKEN> [选项]
#   ./install-macos-runner.sh install --pat <PAT>     [选项]
#   ./install-macos-runner.sh status
#   ./install-macos-runner.sh uninstall [--token <REMOVE_TOKEN> | --pat <PAT>] [--purge]
#
# 选项:
#   --repo <owner/repo>   仓库级 runner（默认 zhangp996/UniXNU）
#   --org <org>           组织级 runner（与 --repo 二选一）
#   --name <name>         runner 名称，默认 <主机名>-macos
#   --labels a,b          额外标签；self-hosted / macOS / ARM64 由 runner 自动添加
#   --dir <path>          安装目录，默认 ~/actions-runner
#   --work <path>         工作目录，默认 <dir>/_work
#   --version <x.y.z>     固定 runner 版本，默认取 GitHub 最新 release
#   --no-service          只注册，不安装 launchd 常驻服务
#   --keepalive           服务异常退出后自动拉起（KeepAlive）
#   --no-sleep            关闭系统/硬盘休眠（需要 sudo）
#   --accept-license      自动接受 Xcode 许可（需要 sudo）
#   --skip-xcode-check    跳过 Xcode 检查
#   --purge               卸载时连同安装目录一起删除
#
# 环境变量: RUNNER_TOKEN / GH_PAT 可替代 --token / --pat
#
set -euo pipefail

SCRIPT_VERSION="1.1.0"
DEFAULT_REPO="zhangp996/UniXNU"
FALLBACK_VERSION="2.338.1"

CMD="install"
REPO="$DEFAULT_REPO"
ORG=""
TOKEN=""
PAT=""
RUNNER_NAME=""
EXTRA_LABELS=""
RUNNER_DIR="$HOME/actions-runner"
WORK_DIR=""
RUNNER_VERSION=""
RUNNER_ARCH=""
LABELS=""
SERVICE=1
KEEPALIVE=0
NO_SLEEP=0
ACCEPT_LICENSE=0
SKIP_XCODE_CHECK=0
PURGE=0

log()  { printf '==> %s\n' "$*"; }
warn() { printf 'warning: %s\n' "$*" >&2; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }

usage() {
  cat <<'USAGE'
install-macos-runner.sh — 在 macOS 上安装 GitHub Actions 自托管 runner

  ./install-macos-runner.sh install   --token <TOKEN> [选项]
  ./install-macos-runner.sh install   --pat <PAT>     [选项]
  ./install-macos-runner.sh status
  ./install-macos-runner.sh uninstall [--token <REMOVE_TOKEN> | --pat <PAT>] [--purge]

选项:
  --repo <owner/repo>   仓库级 runner（默认 zhangp996/UniXNU）
  --org <org>           组织级 runner（与 --repo 二选一）
  --name <name>         runner 名称，默认 <主机名>-macos
  --labels a,b          额外标签（self-hosted/macOS/ARM64 自动添加，无需手写）
  --dir <path>          安装目录，默认 ~/actions-runner
  --work <path>         工作目录，默认 <dir>/_work
  --version <x.y.z>     固定 runner 版本，默认取 GitHub 最新 release
  --no-service          只注册，不安装 launchd 服务
  --keepalive           服务异常退出后自动拉起（KeepAlive）
  --no-sleep            关闭系统/硬盘休眠（需要 sudo）
  --accept-license      自动接受 Xcode 许可（需要 sudo）
  --skip-xcode-check    跳过 Xcode 检查
  --purge               卸载时连同安装目录一起删除

环境变量: RUNNER_TOKEN / GH_PAT 可替代 --token / --pat
USAGE
}

# ---------------------------------------------------------------- 环境检查 ---

require_macos() {
  if [ "$(uname -s)" != "Darwin" ]; then
    die "此脚本只能在 macOS 上运行（当前系统: $(uname -s)）"
  fi
}

require_not_root() {
  if [ "$(id -u)" -eq 0 ]; then
    die "不要用 sudo/root 运行：macOS 的 svc.sh 会拒绝 root，服务需要装在你的登录用户下"
  fi
}

require_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "缺少命令: $1"
}

check_disk() {
  local free_gb
  free_gb=$(df -g / 2>/dev/null | awk 'NR==2 {print $4}') || true
  case "$free_gb" in
    ''|*[!0-9]*) return 0 ;;
  esac
  if [ "$free_gb" -lt 40 ]; then
    warn "根分区剩余 ${free_gb}GB；Xcode 构建建议至少 40GB 可用空间"
  fi
}

check_xcode() {
  if ! xcode-select -p >/dev/null 2>&1; then
    die "未找到 Command Line Tools。请先运行: xcode-select --install"
  fi
  log "Developer dir: $(xcode-select -p)"

  if xcodebuild -version >/dev/null 2>&1; then
    log "Xcode: $(xcodebuild -version | tr '\n' ' ')"
  else
    warn "xcodebuild 不可用：可能只装了 Command Line Tools，或 Xcode 许可尚未接受"
    if [ "$ACCEPT_LICENSE" -eq 1 ]; then
      sudo xcodebuild -license accept || warn "接受 Xcode 许可失败，请手动执行: sudo xcodebuild -license accept"
    else
      warn "可加 --accept-license 自动接受，或手动执行: sudo xcodebuild -license accept"
    fi
  fi

  if ! xcrun --sdk iphoneos --show-sdk-version >/dev/null 2>&1; then
    warn "找不到 iphoneos SDK：iOS 构建会失败，需要完整安装 Xcode 并在 Xcode 中下载 SDK"
  fi
}

disable_sleep() {
  log "关闭休眠（需要 sudo）..."
  if ! sudo pmset -a sleep 0 disksleep 0 displaysleep 0 womp 1 autorestart 1; then
    warn "pmset 设置失败，可稍后手动执行"
  fi
}

# -------------------------------------------------------------- 参数与解析 ---

validate_scope() {
  if [ -n "$ORG" ] && [ "$REPO" != "$DEFAULT_REPO" ]; then
    die "--repo 与 --org 只能二选一"
  fi
  if [ -z "$ORG" ]; then
    case "$REPO" in
      */*) ;;
      *) die "--repo 格式应为 owner/repo，例如 zhangp996/UniXNU" ;;
    esac
    case "$REPO" in
      */*/*) die "--repo 格式应为 owner/repo，例如 zhangp996/UniXNU" ;;
    esac
  fi
}

scope_kind() {
  if [ -n "$ORG" ]; then printf 'orgs'; else printf 'repos'; fi
}

resolve_slug() {
  if [ -n "$ORG" ]; then printf '%s' "$ORG"; else printf '%s' "$REPO"; fi
}

scope_desc() {
  if [ -n "$ORG" ]; then printf '组织级'; else printf '仓库级'; fi
}

parse_args() {
  if [ $# -eq 0 ]; then usage; exit 0; fi
  case "$1" in
    install|status|uninstall) CMD="$1"; shift ;;
    -h|--help|help) usage; exit 0 ;;
    -*) CMD="install" ;;
    *) die "未知子命令: $1（可用: install / status / uninstall）" ;;
  esac

  while [ $# -gt 0 ]; do
    case "$1" in
      --repo)    [ $# -ge 2 ] || die "--repo 缺少参数";    REPO="$2"; shift 2 ;;
      --org)     [ $# -ge 2 ] || die "--org 缺少参数";     ORG="$2"; shift 2 ;;
      --token)   [ $# -ge 2 ] || die "--token 缺少参数";   TOKEN="$2"; shift 2 ;;
      --pat)     [ $# -ge 2 ] || die "--pat 缺少参数";     PAT="$2"; shift 2 ;;
      --name)    [ $# -ge 2 ] || die "--name 缺少参数";    RUNNER_NAME="$2"; shift 2 ;;
      --labels)  [ $# -ge 2 ] || die "--labels 缺少参数";  EXTRA_LABELS="$2"; shift 2 ;;
      --dir)     [ $# -ge 2 ] || die "--dir 缺少参数";     RUNNER_DIR="$2"; shift 2 ;;
      --work)    [ $# -ge 2 ] || die "--work 缺少参数";    WORK_DIR="$2"; shift 2 ;;
      --version) [ $# -ge 2 ] || die "--version 缺少参数"; RUNNER_VERSION="$2"; shift 2 ;;
      --no-service)       SERVICE=0; shift ;;
      --keepalive)        KEEPALIVE=1; shift ;;
      --no-sleep)         NO_SLEEP=1; shift ;;
      --accept-license)   ACCEPT_LICENSE=1; shift ;;
      --skip-xcode-check) SKIP_XCODE_CHECK=1; shift ;;
      --purge)            PURGE=1; shift ;;
      -h|--help)          usage; exit 0 ;;
      *) die "未知选项: $1（--help 查看用法）" ;;
    esac
  done

  if [ -z "$TOKEN" ]; then
    TOKEN="${RUNNER_TOKEN:-}"
  fi
  if [ -z "$PAT" ]; then
    PAT="${RUNNER_CFG_PAT:-${GH_PAT:-}}"
  fi
  RUNNER_DIR="${RUNNER_DIR/#\~/$HOME}"
  if [ -z "$WORK_DIR" ]; then WORK_DIR="$RUNNER_DIR/_work"; fi
}

# ---------------------------------------------------------------- JSON 解析 ---

json_field() {
  local key="$1"
  if command -v python3 >/dev/null 2>&1; then
    python3 -c 'import json,sys
try:
    data = json.load(sys.stdin)
except Exception:
    sys.exit(1)
print(data.get(sys.argv[1], "") or "")' "$key"
  else
    sed -n 's/.*"'"$key"'"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | sed -n '1p'
  fi
}

# ------------------------------------------------------------------- token ---

resolve_token() {
  local kind="$1" json token

  if [ -n "$TOKEN" ]; then
    printf '%s' "$TOKEN"
    return 0
  fi

  if [ -n "$PAT" ]; then
    json=$(curl -fsSL -X POST \
      -H "Accept: application/vnd.github+json" \
      -H "Authorization: Bearer $PAT" \
      -H "X-GitHub-Api-Version: 2022-11-28" \
      "https://api.github.com/$(scope_kind)/$(resolve_slug)/actions/runners/$kind") \
      || die "用 PAT 获取 $kind 失败：检查 PAT 是否有该仓库的 Administration 读写权限"
    token=$(printf '%s' "$json" | json_field token) || true
    if [ -z "$token" ]; then
      die "PAT 响应中没有 token 字段: $(printf '%s' "$json" | cut -c1-200)"
    fi
    printf '%s' "$token"
    return 0
  fi

  if [ ! -t 0 ]; then
    die "需要 token：--token <TOKEN> 或 --pat <PAT>（也可用环境变量 RUNNER_TOKEN）"
  fi
  printf '粘贴 GitHub 生成的 token（输入不回显）: ' >&2
  token=""
  read -r -s token || true
  printf '\n' >&2
  [ -n "$token" ] || die "token 为空"
  printf '%s' "$token"
}

# --------------------------------------------------------------------- 版本 ---

resolve_version() {
  if [ -n "$RUNNER_VERSION" ]; then
    printf '%s' "$RUNNER_VERSION"
    return 0
  fi
  local tag=""
  tag=$(curl -fsSL --connect-timeout 10 --max-time 30 \
    https://api.github.com/repos/actions/runner/releases/latest 2>/dev/null | json_field tag_name) || true
  tag="${tag#v}"
  case "$tag" in
    [0-9]*.[0-9]*.[0-9]*) printf '%s' "$tag" ;;
    *)
      warn "无法从 GitHub API 获取最新版本，回退到 $FALLBACK_VERSION"
      printf '%s' "$FALLBACK_VERSION"
      ;;
  esac
}

runner_arch() {
  case "$(uname -m)" in
    arm64)  printf 'arm64' ;;
    x86_64) printf 'x64' ;;
    *)      die "不支持的架构: $(uname -m)" ;;
  esac
}

# --------------------------------------------------------------------- 标签 ---

auto_labels() {
  local out="unixnu" xcode_major=""
  xcode_major=$(xcodebuild -version 2>/dev/null \
    | sed -n 's/^Xcode[[:space:]]\{1,\}\([0-9][0-9]*\).*/\1/p' \
    | sed -n '1p') || true
  if [ -n "$xcode_major" ]; then
    out="$out,xcode-$xcode_major"
  fi
  printf '%s' "$out"
}

effective_labels() {
  local raw="$EXTRA_LABELS" out="" item lower old_ifs
  old_ifs="$IFS"
  IFS=','
  for item in $raw; do
    item=$(printf '%s' "$item" | sed 's/^[[:space:]]*//; s/[[:space:]]*$//')
    if [ -z "$item" ]; then continue; fi
    lower=$(printf '%s' "$item" | tr '[:upper:]' '[:lower:]')
    case "$lower" in
      self-hosted|linux|windows|macos|darwin|arm|arm64|x64|x86|amd64|aarch64)
        warn "标签 '$item' 由 runner 自动添加，已忽略"
        continue
        ;;
    esac
    if [ -z "$out" ]; then out="$item"; else out="$out,$item"; fi
  done
  IFS="$old_ifs"
  printf '%s' "$out"
}

# ---------------------------------------------------------------- 下载与注册 ---

install_runner_files() {
  local version="$1"
  local tarball="actions-runner-osx-${RUNNER_ARCH}-${version}.tar.gz"
  local url="https://github.com/actions/runner/releases/download/v${version}/${tarball}"
  local tmp
  tmp=$(mktemp -d)

  log "下载 actions-runner v$version (osx-$RUNNER_ARCH) ..."
  if ! curl -fL --retry 3 --retry-delay 3 --connect-timeout 20 --progress-bar \
      -o "$tmp/$tarball" "$url"; then
    rm -rf "$tmp"
    die "下载失败: $url"
  fi

  log "解压到 $RUNNER_DIR"
  mkdir -p "$RUNNER_DIR"
  if ! tar -xzf "$tmp/$tarball" -C "$RUNNER_DIR"; then
    rm -rf "$tmp"
    die "解压失败"
  fi
  rm -rf "$tmp"
  chmod +x "$RUNNER_DIR/config.sh" "$RUNNER_DIR/svc.sh" "$RUNNER_DIR/run.sh" 2>/dev/null || true
}

reset_local_config() {
  if [ -f "$RUNNER_DIR/.runner" ]; then
    warn "检测到本地已有配置，先停服务并清理本地凭据（GitHub 上同名 runner 会被 --replace 覆盖）"
    if [ -x "$RUNNER_DIR/svc.sh" ]; then
      ( cd "$RUNNER_DIR" && ./svc.sh stop ) >/dev/null 2>&1 || true
      ( cd "$RUNNER_DIR" && ./svc.sh uninstall ) >/dev/null 2>&1 || true
    fi
    rm -f "$RUNNER_DIR/.runner" "$RUNNER_DIR/.credentials" \
          "$RUNNER_DIR/.credentials_rsaparams" "$RUNNER_DIR/.service" 2>/dev/null || true
  fi
}

configure_runner() {
  local token="$1" url
  url="https://github.com/$(resolve_slug)"
  set -- --unattended --url "$url" --token "$token" --name "$RUNNER_NAME" --replace
  if [ -n "$WORK_DIR" ]; then
    set -- "$@" --work "$WORK_DIR"
  fi
  if [ -n "$LABELS" ]; then
    set -- "$@" --labels "$LABELS"
  fi

  log "注册 runner 到 $url ..."
  if ! ( cd "$RUNNER_DIR" && ./config.sh "$@" ); then
    die "注册失败：token 可能已过期（有效期 1 小时），或当前账号没有该仓库的 Admin 权限"
  fi
}

# --------------------------------------------------------------------- 服务 ---

service_plist() {
  ls -t "$HOME"/Library/LaunchAgents/actions.runner.*.plist 2>/dev/null | sed -n '1p' || true
}

install_service() {
  local plist
  plist=$(service_plist)
  if [ -n "$plist" ]; then
    log "清理旧服务: $(basename "$plist")"
    ( cd "$RUNNER_DIR" && ./svc.sh stop ) >/dev/null 2>&1 || true
    ( cd "$RUNNER_DIR" && ./svc.sh uninstall ) >/dev/null 2>&1 || true
  fi

  log "安装 launchd 服务 ..."
  if ! ( cd "$RUNNER_DIR" && ./svc.sh install ); then
    die "svc.sh install 失败（macOS 上不要用 sudo，且需在图形登录会话中执行）"
  fi

  if [ "$KEEPALIVE" -eq 1 ]; then
    plist=$(service_plist)
    if [ -n "$plist" ]; then
      /usr/libexec/PlistBuddy -c "Add :KeepAlive bool true" "$plist" >/dev/null 2>&1 \
        || /usr/libexec/PlistBuddy -c "Set :KeepAlive true" "$plist" >/dev/null 2>&1 \
        || warn "写入 KeepAlive 失败（不影响使用）"
      log "已启用 KeepAlive: $plist"
    fi
  fi

  log "启动服务 ..."
  if ! ( cd "$RUNNER_DIR" && ./svc.sh start ); then
    die "svc.sh start 失败，查看 $HOME/Library/Logs/actions.runner.*/ 下的日志"
  fi
  sleep 2
  ( cd "$RUNNER_DIR" && ./svc.sh status ) || true
}

verify_online() {
  local i=0 status="" body=""
  if [ -z "$PAT" ]; then
    return 0
  fi
  if ! command -v python3 >/dev/null 2>&1; then
    return 0
  fi
  log "等待 runner 上线（最多 60 秒）..."
  while [ "$i" -lt 12 ]; do
    body=$(curl -fsSL -H "Authorization: Bearer $PAT" -H "Accept: application/vnd.github+json" \
      "https://api.github.com/$(scope_kind)/$(resolve_slug)/actions/runners" 2>/dev/null) || true
    if [ -n "$body" ]; then
      status=$(printf '%s' "$body" | python3 -c 'import json,sys
try:
    data = json.load(sys.stdin)
except Exception:
    sys.exit(1)
name = sys.argv[1]
print(next((r["status"] for r in data.get("runners", []) if r["name"] == name), ""))' "$RUNNER_NAME" 2>/dev/null) || true
      if [ "$status" = "online" ]; then
        log "runner 已在线"
        return 0
      fi
    fi
    sleep 5
    i=$((i + 1))
  done
  warn "60 秒内未看到 runner 在线（当前: ${status:-unknown}）；检查 $RUNNER_DIR/_diag 日志"
}

# --------------------------------------------------------------------- 输出 ---

print_summary() {
  local settings_url
  if [ -n "$ORG" ]; then
    settings_url="https://github.com/organizations/$(resolve_slug)/settings/actions/runners"
  else
    settings_url="https://github.com/$(resolve_slug)/settings/actions/runners"
  fi

  printf '\n'
  log "安装完成"
  printf '  仓库/组织 : %s (%s)\n' "$(resolve_slug)" "$(scope_desc)"
  printf '  runner    : %s\n' "$RUNNER_NAME"
  printf '  标签      : self-hosted, macOS, %s, %s\n' "$RUNNER_ARCH" "$LABELS"
  printf '  安装目录  : %s\n' "$RUNNER_DIR"
  printf '  工作目录  : %s\n' "$WORK_DIR"
  printf '  服务日志  : %s/Library/Logs/actions.runner.*/\n' "$HOME"
  printf '  runner 页 : %s\n' "$settings_url"
  printf '\n后续:\n'
  printf '  1. 确认设置页里该 runner 显示 Online（绿色）\n'
  printf '  2. 在仓库 Actions 里手动触发一次 "Self-hosted runner check"\n'
  printf '  3. 状态: ./install-macos-runner.sh status   卸载: ./install-macos-runner.sh uninstall --purge\n'
}

print_manual_run() {
  printf '\n'
  log "已注册（未安装服务）。手动启动："
  printf '  cd %s && ./run.sh\n' "$RUNNER_DIR"
  printf '注意：关闭终端就会停止 runner；要常驻请去掉 --no-service 重新执行。\n'
}

# --------------------------------------------------------------------- 命令 ---

cmd_install() {
  require_macos
  require_not_root
  require_cmd curl
  require_cmd tar
  check_disk
  if [ "$SKIP_XCODE_CHECK" -eq 0 ]; then
    check_xcode
  fi
  if [ "$NO_SLEEP" -eq 1 ]; then
    disable_sleep
  fi

  validate_scope
  RUNNER_DIR="${RUNNER_DIR%/}"
  if [ -z "$RUNNER_NAME" ]; then
    RUNNER_NAME="$(hostname -s 2>/dev/null || hostname)-macos"
  fi
  RUNNER_ARCH=$(runner_arch)

  local version extra
  version=$(resolve_version)
  LABELS=$(auto_labels)
  if [ -n "$EXTRA_LABELS" ]; then
    extra=$(effective_labels)
    if [ -n "$extra" ]; then LABELS="$LABELS,$extra"; fi
  fi

  log "目标: https://github.com/$(resolve_slug)（$(scope_desc)）"
  log "runner: $RUNNER_NAME"
  log "标签: self-hosted, macOS, $RUNNER_ARCH, $LABELS"
  log "目录: $RUNNER_DIR"

  local token
  token=$(resolve_token registration-token)

  install_runner_files "$version"
  reset_local_config
  configure_runner "$token"

  if [ "$SERVICE" -eq 1 ]; then
    install_service
    verify_online
    print_summary
  else
    print_manual_run
  fi
}

cmd_status() {
  require_macos
  if [ ! -d "$RUNNER_DIR" ]; then
    die "未找到安装目录: $RUNNER_DIR"
  fi
  log "目录: $RUNNER_DIR"
  if [ -x "$RUNNER_DIR/svc.sh" ]; then
    ( cd "$RUNNER_DIR" && ./svc.sh status ) || true
  fi
  local plist latest
  plist=$(service_plist)
  if [ -n "$plist" ]; then
    log "plist: $plist"
  fi
  latest=$(ls -td "$HOME"/Library/Logs/actions.runner.* 2>/dev/null | sed -n '1p') || true
  if [ -n "$latest" ]; then
    log "最近日志: $latest/stdout.log"
    tail -n 15 "$latest/stdout.log" 2>/dev/null || true
  fi
}

cmd_uninstall() {
  require_macos
  if [ -d "$RUNNER_DIR" ] && [ -x "$RUNNER_DIR/svc.sh" ]; then
    log "停止并卸载服务 ..."
    ( cd "$RUNNER_DIR" && ./svc.sh stop ) >/dev/null 2>&1 || true
    ( cd "$RUNNER_DIR" && ./svc.sh uninstall ) >/dev/null 2>&1 || true
  fi

  if [ -f "$RUNNER_DIR/.runner" ]; then
    local token=""
    token=$(resolve_token remove-token) || true
    if [ -n "$token" ]; then
      ( cd "$RUNNER_DIR" && ./config.sh remove --token "$token" ) \
        || warn "从 GitHub 移除 runner 失败；可到设置页 Actions → Runners 手动删除"
    else
      warn "没有移除 token，只清理了本地；请到 GitHub 设置页手动删除该 runner"
    fi
  else
    warn "$RUNNER_DIR 里没有已注册的 runner 配置"
  fi

  if [ "$PURGE" -eq 1 ]; then
    if [ -t 0 ]; then
      printf '确认删除 %s ? [y/N] ' "$RUNNER_DIR" >&2
      local answer=""
      read -r answer || true
      case "$answer" in
        y|Y|yes|YES) ;;
        *) log "已跳过删除目录"; exit 0 ;;
      esac
    fi
    rm -rf "$RUNNER_DIR"
    log "已删除 $RUNNER_DIR"
  fi
}

main() {
  parse_args "$@"
  case "$CMD" in
    install)   cmd_install ;;
    status)    cmd_status ;;
    uninstall) cmd_uninstall ;;
  esac
}

main "$@"
