# 给 UniXNU 接入自托管 macOS Runner

把仓库的 GitHub Actions job 跑到你自己的 Apple Silicon Mac 上。全程约 15 分钟，完成后
`runs-on: [self-hosted, macOS, ARM64]` 的 job 会直接在你机器上执行。

> 目标仓库：**zhangp996/UniXNU**（公开 fork）。换仓库只需改 `--repo`。
>
> **当前状态**：仓库 CI 已使用 GitHub 托管的 `macos-latest`（公开仓库免费额度）。
> 本文档是将来需要自托管时的参考，不影响现有构建。

---

## 0. 为什么值得做

现有 [`.github/workflows/macos.yml`](https://github.com/zhangp996/UniXNU/blob/main/.github/workflows/macos.yml)
跑在 `macos-latest` 上，有几个硬限制：

| 项目 | GitHub 托管 | 自托管（你自己的 Mac） |
| --- | --- | --- |
| 单个 job 时限 | 6 小时 | 5 天 |
| 机器规格 | 固定（3–4 核、14GB 左右） | 你的机器，M 系列可全核 |
| 工具依赖 | 每次 `brew install` 一遍 | 预装一次，后续秒过 |
| 工作区 | 每次冷启动 | `~/actions-runner/_work` 可复用 |
| 费用 | 公开仓库免费 | 免费（用自己的电和机器） |

代价：机器要保持在线、要自己维护，而且**公开仓库用自托管 runner 有安全风险**（见第 7 节）。

---

## 1. 它是怎么工作的

~~~text
GitHub Actions  ──(job 排队)──▶  你的 Mac
                                  └─ actions.runner 服务（launchd 常驻）
                                       └─ 仓库被拉到 ~/actions-runner/_work/UniXNU/UniXNU
                                            └─ 逐步执行 workflow 的 steps
~~~

- runner 主动**外连** GitHub 领任务，不需要公网 IP、不开端口、不做内网穿透。
- runner 自动带 3 个标签：`self-hosted`、`macOS`、`ARM64`（Apple Silicon）。
  脚本再加 `unixnu` 和 `xcode-<主版本>`，方便以后按工具链分流。
- workflow 写 `runs-on: [self-hosted, macOS, ARM64]` 就会命中它。

---

## 2. 交付的三个文件

| 文件 | 放到哪里 | 作用 |
| --- | --- | --- |
| `install-macos-runner.sh` | Mac 上任意目录（建议同时提交到仓库 `scripts/`） | 安装 / 查看状态 / 卸载 |
| `.github/workflows/self-hosted-runner-check.yml` | 仓库根目录下同路径 | 验证 runner 与 Xcode 工具链 |
| 本文件 `SELF-HOSTED-RUNNER.md` | 仓库根目录（可选） | 运维手册 |

提交 workflow（Windows 上最省事，纯浏览器）：

1. 打开 <https://github.com/zhangp996/UniXNU/upload/main/.github/workflows>
2. 把 `self-hosted-runner-check.yml` 拖进去，Commit。

用 git 的话：

~~~bash
git checkout -b ci/self-hosted-runner
mkdir -p .github/workflows
cp self-hosted-runner-check.yml .github/workflows/
cp install-macos-runner.sh scripts/          # 可选，方便 Mac 直接 curl 下载
git add . && git commit -m "ci: add self-hosted macOS runner check" && git push -u origin HEAD
~~~

---

## 3. 在 Mac 上安装

### 3.1 前置检查（1 分钟）

~~~bash
uname -s && uname -m                  # 期望 Darwin / arm64

xcode-select -p                       # 必须指向完整 Xcode，只有 CLT 不行
sudo xcode-select -s /Applications/Xcode.app/Contents/Developer
sudo xcodebuild -license accept
xcodebuild -version

df -h /                               # 建议 ≥ 40GB 空闲
~~~

需要能访问 `github.com`、`api.github.com`、`objects.githubusercontent.com`。

> ⚠️ 一定要在 Mac 的**图形界面终端**（Terminal.app / iTerm）里操作。
> SSH 会话里执行 `svc.sh install` 会拿不到 GUI 会话，报
> `Bootstrap failed: 5: Input/output error`。

### 3.2 拿注册 token

打开 <https://github.com/zhangp996/UniXNU/settings/actions/runners/new>，
页面命令里带一个 `--token <XXXX>`，复制它（**有效期 1 小时**）。

或用 CLI：

~~~bash
gh api -X POST repos/zhangp996/UniXNU/actions/runners/registration-token --jq .token

# 或者用 PAT（需要该仓库 Administration: Read and write）
curl -s -X POST -H "Authorization: Bearer $GH_PAT" https://api.github.com/repos/zhangp996/UniXNU/actions/runners/registration-token
~~~

### 3.3 运行安装脚本

~~~bash
# 方式 A：从仓库下载（先把脚本提交到 scripts/）
curl -fL -O https://raw.githubusercontent.com/zhangp996/UniXNU/main/scripts/install-macos-runner.sh

# 方式 B：从 Windows 传过去（PowerShell 自带 scp）
#   scp .\install-macos-runner.sh 你的用户名@mac.local:~/

chmod +x install-macos-runner.sh

# 最小用法
./install-macos-runner.sh install --token 粘贴刚才的TOKEN

# 更省事：用 PAT 自动换 token，装完还会等到 runner 上线
./install-macos-runner.sh install --pat ghp_xxxxxxxx

# 常用组合
./install-macos-runner.sh install --token XXXX --name mac-mini-m4 --labels unixnu,ios --keepalive --no-sleep
~~~

脚本做的事：

1. 检查 macOS、架构、Xcode、磁盘；
2. 从 GitHub 取最新版本（`v2.338.1` 兜底），下载 `osx-arm64` 包；
3. `config.sh --unattended --replace` 注册，标签自动带 `self-hosted,macOS,ARM64`，
   另加 `unixnu` 和 `xcode-<主版本>`；
4. `svc.sh install && svc.sh start` 装成 launchd 用户级服务；
5. （可选）写入 `KeepAlive`，进程异常退出自动拉起。

### 3.4 确认在线

~~~bash
./install-macos-runner.sh status
~~~

或刷新 <https://github.com/zhangp996/UniXNU/settings/actions/runners>，看到 **Idle / 绿点** 即可。

### 3.5 跑一次验证 job

runner 注册好之后，任何 `runs-on: [self-hosted, macOS, ARM64]` 的 job 都会落到这台机器上。
最直接的验证是按第 4 节把 `macos.yml` 临时切过去跑一次；也可以先加一个只执行
`uname -a && xcodebuild -version` 的 `workflow_dispatch` workflow 试水。

> 仓库里的 `macos-toolchain-check.yml` 验证的是 GitHub 托管镜像，不是这台自托管机器。

---

## 4. 把现有 macos.yml 切到自托管

改一行：

~~~diff
 jobs:
   build:
-    runs-on: macos-latest
+    runs-on: [self-hosted, macOS, ARM64]
     timeout-minutes: 360
~~~

然后在 Actions 里手动触发 **macOS build**，看着它在你机器上跑。

注意点：

- `timeout-minutes: 360` 可以调大（自托管上限 5 天），但时间越长占机越久。
- `brew install` 每次都会执行：可以在 Mac 上预装，再改成
  `brew list cmake >/dev/null 2>&1 || brew install cmake ninja unifdef llvm mtools dtc`。
- 工作区不会自动清零：`actions/checkout@v4` 默认 `clean: true` 只清 git 目录，
  `_work` 下的构建产物需要自己定期清理。
- 想保留云端兜底：给 `workflow_dispatch` 加一个 `runner` 选择框，然后

  ~~~yaml
  runs-on: ${{ inputs.runner == 'self-hosted' && fromJSON('["self-hosted","macOS","ARM64"]') || 'macos-latest' }}
  ~~~

---

## 5. 让它 7×24 在线

### 5.1 服务是「跟随登录」启动，不是「开机」启动

macOS 上 `svc.sh` 装的是 **LaunchAgent**
（`~/Library/LaunchAgents/actions.runner.<owner>-<repo>.<name>.plist`），
只有该用户**登录图形会话**后才会加载。所以：机器重启后没人登录，runner 不会起来。

### 5.2 自动登录（两种取舍）

| 方案 | 做法 | 代价 |
| --- | --- | --- |
| A. 关 FileVault + 自动登录 | `sudo fdesetup disable`，再 `sudo defaults write /Library/Preferences/com.apple.loginwindow autoLoginUser "$USER"` | 磁盘不加密 |
| B. 保留 FileVault | 重启后手动登录一次 | 每次重启要人操作 |

查 FileVault：`fdesetup status`。

### 5.3 不休眠、断电自启

~~~bash
sudo pmset -a sleep 0 disksleep 0 displaysleep 0 womp 1 autorestart 1
pmset -g custom
~~~

`sleep 0` 不睡眠、`womp 1` 网络唤醒、`autorestart 1` 断电恢复后自动开机。
安装时加 `--no-sleep` 让脚本代劳。

### 5.4 进程挂了自动拉起

`svc.sh` 默认 plist 只有 `RunAtLoad`，**没有** `KeepAlive`。加 `--keepalive`
让脚本用 PlistBuddy 补上。

### 5.5 验证自愈

~~~bash
sudo reboot
./install-macos-runner.sh status
launchctl list | grep actions.runner
~~~

---

## 6. 日常运维

| 场景 | 命令 |
| --- | --- |
| 查看状态 | `./install-macos-runner.sh status` |
| 服务日志 | `tail -f ~/Library/Logs/actions.runner.*/stdout.log` |
| runner 诊断日志 | `tail -f ~/actions-runner/_diag/Runner_*.log` |
| 重启服务 | `cd ~/actions-runner && ./svc.sh stop && ./svc.sh start` |
| 临时停机 | `./svc.sh stop`（页面变 Offline） |
| 升级 | 默认自动升级；也可重跑安装脚本 |
| 换名字 / 标签 | 重跑脚本并传 `--name` / `--labels` |
| 卸载 | `./install-macos-runner.sh uninstall --purge` |

> `--labels` 是**覆盖**不是追加。重跑时不带 `--labels`，之前手加的标签会丢
> （脚本自己的 `unixnu`、`xcode-<版本>` 会重新生成）。

---

## 7. 安全（公开仓库必读）

GitHub 官方不建议在**公开仓库**用自托管 runner：任何能提 PR 的人，都能让你的机器执行任意代码。
本仓库是公开 fork，务必做到：

1. **不要给自托管 job 加 `pull_request` 触发**。
   本项目的 `self-hosted-runner-check.yml` 只监听 `workflow_dispatch` 和定时任务，这是故意的。
2. **收紧 fork PR 审批**：Settings → Actions → General → *Fork pull request workflows* →
   **Require approval for all outside collaborators**。
3. **真要跑 PR 就限制来源**：

   ~~~yaml
   if: github.event.pull_request.head.repo.full_name == github.repository
   ~~~

4. **runner 上不留长期凭据**：证书、token 走 Secrets 临时注入，别写进 `~/.zshrc`。
5. **更彻底**：单独建 macOS 用户或用虚拟机，以 `--ephemeral` 一次性模式运行。
6. **谁能改 workflow，谁就能改你的机器**：只给可信的人仓库 Admin。

---

## 8. 可选：iOS 签名（当前仓库不需要）

~~~yaml
      - name: Import signing certificate
        env:
          CERT_P12: ${{ secrets.BUILD_CERTIFICATE_BASE64 }}
          P12_PASSWORD: ${{ secrets.P12_PASSWORD }}
          KEYCHAIN_PASSWORD: ${{ secrets.KEYCHAIN_PASSWORD }}
        run: |
          set -euo pipefail
          keychain="$RUNNER_TEMP/build.keychain-db"
          echo -n "$CERT_P12" | base64 --decode -o "$RUNNER_TEMP/cert.p12"
          security create-keychain -p "$KEYCHAIN_PASSWORD" "$keychain"
          security set-keychain-settings -lut 21600 "$keychain"
          security unlock-keychain -p "$KEYCHAIN_PASSWORD" "$keychain"
          security import "$RUNNER_TEMP/cert.p12" -k "$keychain" -P "$P12_PASSWORD" -T /usr/bin/codesign
          security set-key-partition-list -S apple-tool:,apple: -k "$KEYCHAIN_PASSWORD" "$keychain"
~~~

自托管 runner 不会自动清理 keychain，job 结束前记得 `security delete-keychain`，否则证书会一直留在机器上。

---

## 9. 排错

| 现象 | 可能原因 | 处理 |
| --- | --- | --- |
| workflow 一直 **Queued** | runner 离线 / 标签不匹配 | 设置页确认 Online；`runs-on` 的标签要和 runner 的标签对应（大小写不敏感） |
| 报 `Must not run with sudo` | 用了 sudo | 去掉 sudo，用登录用户跑 |
| `svc.sh install` 失败 / `Bootstrap failed: 5` | 在 SSH 会话里执行 | 到图形界面终端重跑 |
| `xcodebuild` 报 license | 没接受许可 | `sudo xcodebuild -license accept` |
| 找不到 `iphoneos` SDK | 只装了 CLT | 装完整 Xcode，再 `sudo xcode-select -s` |
| job 卡在 checkout | 网络 / 代理 / DNS | Mac 上 `git ls-remote https://github.com/zhangp996/UniXNU` 自测 |
| 磁盘被吃满 | DerivedData、旧工作区 | `rm -rf ~/Library/Developer/Xcode/DerivedData/*`，清 `_work` |
| token 无效 | 超过 1 小时 | 重新生成 |
| 页面 Offline 但进程在 | plist 没加载 | `launchctl load -w ~/Library/LaunchAgents/actions.runner.*.plist` |
| 重跑后标签丢了 | `--labels` 是覆盖 | 带上完整 `--labels` |

---

## 10. 验收清单

- [ ] 设置页里 runner 显示 **Online / Idle**
- [ ] 任意 `runs-on: [self-hosted, macOS, ARM64]` 的 job 能落到这台机器上
- [ ] `macos.yml` 改 `runs-on` 后手动跑通一次构建
- [ ] 重启 Mac 后 runner 能自动回来（或清楚知道自己要手动登录）
- [ ] 第 7 节安全项已确认（尤其 fork PR 审批）

---

## 附录：不用脚本的手动安装

~~~bash
mkdir -p ~/actions-runner && cd ~/actions-runner

curl -fL -O https://github.com/actions/runner/releases/download/v2.338.1/actions-runner-osx-arm64-2.338.1.tar.gz
tar xzf actions-runner-osx-arm64-2.338.1.tar.gz

./config.sh --url https://github.com/zhangp996/UniXNU --token <TOKEN> --name mac-mini-m4 --labels unixnu --work _work

./svc.sh install
./svc.sh start
./svc.sh status

# 卸载
./svc.sh stop && ./svc.sh uninstall
./config.sh remove --token <REMOVE_TOKEN>
~~~
