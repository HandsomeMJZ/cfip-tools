# Cloudflare IP 优选工具

一个适用于 Windows 的 Cloudflare IP 自动测速工具。

它可以自动下载 IP 列表，批量测试 TCP 延迟和下载速度，筛选出更快的节点，并生成可直接用于订阅的 `best_ips.txt`。你也可以让它把结果自动上传到 GitHub 或 Cloudflare R2。

> 我的优选订阅仓库：<https://github.com/HandsomeMJZ/cfip>

---

## 重要提醒

> **运行测速期间，请不要使用 TUN 模式代理。**

测速会产生大量连接和下载请求。如果开启 TUN 模式代理，流量可能会经过你的代理通道，导致 KV、流量或额度被快速消耗。

建议运行前确认：

- TUN 模式已关闭
- 系统代理不会接管测速流量
- 需要代理 GitHub 时，只配置 `git_http_proxy` / `git_https_proxy`

---

## 功能一览

| 功能 | 说明 |
| --- | --- |
| 自动下载 IP 列表 | 从 `input_url` 下载最新节点，也支持本地 `ips.txt` |
| TCP 延迟测试 | 快速筛掉不可用或延迟过高的节点 |
| 下载测速 | 使用 Cloudflare 测速地址测试实际带宽 |
| 高速筛选 | 按 `min_speed_mbps` 自动生成优选结果 |
| 延迟显示 | 输出中可显示 `45ms` |
| 带宽显示 | 输出中可显示 `28Mbps`，精确到个位数 |
| GitHub 上传 | 可自动提交并推送结果文件 |
| R2 上传 | 可自动上传结果到 Cloudflare R2 |

---

## 文件说明

| 文件 | 作用 |
| --- | --- |
| `cf_updater.exe` | 主程序，双击或命令行运行 |
| `setting.config` | 配置文件，开关、阈值、上传信息都在这里 |
| `ips.txt` | 本地 IP 列表，关闭自动下载时使用 |
| `full_ips.txt` | 所有测速成功节点 |
| `best_ips.txt` | 达到高速阈值的优选节点，通常使用这个 |
| `README.MD` | 说明文件，也可随结果一起上传 |

---

## 快速开始

### 1. 运行程序

在当前目录打开 PowerShell：

```powershell
.\cf_updater.exe
```

也可以直接双击 `cf_updater.exe`。

### 2. 等待测速完成

程序会按顺序执行：

```text
下载 IP 列表 -> TCP 延迟测试 -> 下载测速 -> 写入结果 -> 上传同步
```

### 3. 查看结果

运行完成后会生成：

```text
full_ips.txt
best_ips.txt
```

新手一般只需要使用：

```text
best_ips.txt
```

输出示例：

```text
1.2.3.4:443#HK_1 [优选高速] [45ms 28Mbps]
```

---

## 推荐的新手配置

第一次使用时，建议先不要开启上传，只在本地生成结果：

```ini
download_input=true
min_speed_mbps=12.0
show_latency=true
show_bandwidth=true
github_upload_enabled=false
r2_upload_enabled=false
```

确认能正常生成 `best_ips.txt` 后，再考虑开启 GitHub 或 R2 上传。

---

## 常用配置

### 输入 IP 列表

自动下载在线列表：

```ini
download_input=true
input_url=https://zip.cm.edu.kg/all.txt
```

使用本地 `ips.txt`：

```ini
download_input=false
input_file=ips.txt
```

### 高速节点阈值

```ini
min_speed_mbps=12.0
```

含义：下载速度大于 `12 Mbps` 的节点会进入 `best_ips.txt`。

如果想更严格：

```ini
min_speed_mbps=30.0
```

如果节点太少：

```ini
min_speed_mbps=5.0
```

### 延迟和带宽显示

```ini
show_latency=true
show_bandwidth=true
```

开启后会显示：

```text
[45ms 28Mbps]
```

关闭带宽显示：

```ini
show_bandwidth=false
```

关闭延迟显示：

```ini
show_latency=false
```

### 每个地区测速数量

```ini
top_per_region=8
```

含义：每个地区先按 TCP 延迟选出前 `8` 个节点，再进入下载测速。

如果你想提高命中好节点的概率，可以适当调大，例如：

```ini
top_per_region=12
```

---

## GitHub 自动上传

开启 GitHub 上传：

```ini
github_upload_enabled=true
github_repo=https://github.com/你的用户名/你的仓库.git
github_branch=main
github_token_env=GITHUB_TOKEN
```

推荐使用环境变量保存 Token，不要直接写进配置文件。

PowerShell 临时设置：

```powershell
$env:GITHUB_TOKEN="你的 GitHub Token"
.\cf_updater.exe
```

上传成功后，仓库会更新：

```text
full_ips.txt
best_ips.txt
README.MD
```

临时关闭 GitHub 上传：

```powershell
.\cf_updater.exe --no-upload
```

---

## Cloudflare R2 自动上传

开启 R2 上传：

```ini
r2_upload_enabled=true
r2_account_id=你的 Cloudflare Account ID
r2_bucket=你的 R2 存储桶名称
r2_access_key_env=R2_ACCESS_KEY_ID
r2_secret_key_env=R2_SECRET_ACCESS_KEY
```

PowerShell 临时设置 R2 密钥：

```powershell
$env:R2_ACCESS_KEY_ID="你的 R2 Access Key ID"
$env:R2_SECRET_ACCESS_KEY="你的 R2 Secret Access Key"
.\cf_updater.exe
```

上传到 R2 指定目录：

```ini
r2_prefix=cfip
```

上传后的对象路径：

```text
cfip/full_ips.txt
cfip/best_ips.txt
cfip/README.MD
```

临时关闭 R2 上传：

```powershell
.\cf_updater.exe --no-r2-upload
```

---

## 常用命令

| 命令 | 作用 |
| --- | --- |
| `.\cf_updater.exe` | 正常测速并按配置上传 |
| `.\cf_updater.exe --help` | 查看帮助 |
| `.\cf_updater.exe --setup` | 重新运行配置向导 |
| `.\cf_updater.exe --push-only` | 不重新测速，只推送已有结果 |
| `.\cf_updater.exe --upload` | 本次运行强制开启 GitHub 上传 |
| `.\cf_updater.exe --no-upload` | 本次运行关闭 GitHub 上传 |
| `.\cf_updater.exe --r2-upload` | 本次运行强制开启 R2 上传 |
| `.\cf_updater.exe --no-r2-upload` | 本次运行关闭 R2 上传 |
| `.\cf_updater.exe --no-bandwidth` | 本次运行隐藏带宽 |

---

## 常见问题

### 没有生成高速节点怎么办？

把高速阈值调低一点：

```ini
min_speed_mbps=5.0
```

也可以增加每个地区进入测速的候选数量：

```ini
top_per_region=12
```

### GitHub 上传失败怎么办？

请检查：

- 已安装 Git
- `github_repo` 地址正确
- `github_branch` 分支存在
- `GITHUB_TOKEN` 有仓库写入权限
- 如网络访问 GitHub 较慢，可配置 `git_https_proxy`

### R2 上传失败怎么办？

请检查：

- `r2_account_id` 是否正确
- `r2_bucket` 是否存在
- R2 Access Key 是否有写入权限
- `R2_ACCESS_KEY_ID` 和 `R2_SECRET_ACCESS_KEY` 是否已设置

### 配置后面的中文注释可以删吗？

可以。程序支持行内注释：

```ini
show_bandwidth=true # 是否显示带宽
```

也支持无注释：

```ini
show_bandwidth=true
```

---

## 安全提醒

不要把下面这些内容提交到公开仓库：

- GitHub Token
- R2 Access Key
- R2 Secret Access Key

更推荐使用环境变量保存密钥，例如：

```powershell
$env:GITHUB_TOKEN="你的 GitHub Token"
$env:R2_ACCESS_KEY_ID="你的 R2 Access Key ID"
$env:R2_SECRET_ACCESS_KEY="你的 R2 Secret Access Key"
```
