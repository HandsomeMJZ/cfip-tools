# ☁ Cloudflare IP 优选工具

> 自动测试 Cloudflare CDN 节点的 TCP 延迟与下载速度，筛选最优 IP 并可自动推送至 GitHub，供 EdgeTunnel 等工具订阅使用。

---
## 我的预选订阅仓库

>https://github.com/HandsomeMJZ/cfip

## ⚠️特别提醒!!!

**请不要在运行期间使用tun模式代理!!!否则会严重消耗KV数值!!!##

## 📋 目录

- [功能特性](#功能特性)
- [快速开始](#快速开始)
- [命令行参数](#命令行参数)
- [配置文件详解](#配置文件详解)
- [输出文件说明](#输出文件说明)
- [GitHub 自动推送](#github-自动推送)
- [订阅使用方法](#订阅使用方法)
- [常见问题](#常见问题)

---

## ✨ 功能特性

- **TCP 延迟测试**：多线程并发探测节点连通性与延迟，支持 IPv4 / IPv6
- **下载测速**：对延迟达标节点进行真实带宽测试（通过 `speed.cloudflare.com` 下载 2 MB）
- **智能筛选**：每个区域取延迟最低的前 N 个节点参与测速，避免无效请求
- **自动推送**：测速完成后可一键推送结果文件和 README 至 GitHub 仓库
- **首次向导**：无配置文件时自动运行配置向导，开箱即用

---

## 🚀 快速开始

### 前置要求

| 依赖       | 说明                                     |
|----------|------------------------------------------|
| `curl.exe` | 需在系统 PATH 中，用于下载 IP 列表和测速  |
| `git.exe`  | 仅 GitHub 推送功能需要                   |
| Windows 10+ | 需支持 ANSI 虚拟终端（默认已支持）      |

### 第一次运行

直接双击 `cf_updater.exe` 或在终端执行：

```bat
cf_updater.exe
```

程序会自动：

1. 生成默认配置文件 `setting.config`
2. 弹出配置向导，引导填写 GitHub 推送相关信息（可直接回车跳过）
3. 下载 IP 列表 → TCP 延迟测试 → 下载测速 → 写入结果文件

---

## 🖥 命令行参数

```
cf_updater.exe [--config <路径>] [--setup] [--upload] [--no-upload] [--push-only] [--help]
```

| 参数              | 说明                                           |
|-----------------|----------------------------------------------|
| `--config <路径>` | 指定配置文件路径，默认为 `setting.config`         |
| `--setup`       | 强制重新运行配置向导（即使配置文件已存在）             |
| `--upload`      | 本次运行**强制开启** GitHub 推送（忽略配置文件设置） |
| `--no-upload`   | 本次运行**禁用** GitHub 推送（忽略配置文件设置）     |
| `--push-only`   | 跳过测速，仅更新 README 并推送现有结果文件至 GitHub  |
| `--help` / `-h` | 显示帮助信息                                    |

**示例：**

```bat
# 使用自定义配置文件运行
cf_updater.exe --config my_config.config

# 强制推送到 GitHub
cf_updater.exe --upload

# 只推送，不重新测速
cf_updater.exe --push-only
```

---

## ⚙ 配置文件详解

配置文件为纯文本格式，每行一项，格式为 `key=value`，`#` 开头为注释行。

首次运行自动生成于程序同目录下的 `setting.config`，可用任意文本编辑器修改。

---

### 📥 输入源

| 配置项             | 默认值                              | 说明                                           |
|------------------|-------------------------------------|------------------------------------------------|
| `input_file`     | `ips.txt`                           | 本地 IP 列表文件路径                             |
| `input_url`      | `https://zip.cm.edu.kg/all.txt`     | 远程 IP 列表下载地址                             |
| `download_input` | `true`                              | 每次运行时是否自动下载最新 IP 列表（`true`/`false`） |
| `download_timeout` | `30`                              | 下载超时时间（秒）                               |

> IP 列表格式：每行一条，格式为 `IP:端口#地区名`，例如 `1.2.3.4:443#HKG`

---

### 📤 输出文件

| 配置项               | 默认值          | 说明                                   |
|--------------------|-----------------|----------------------------------------|
| `full_output_file` | `full_ips.txt`  | 所有测速完成的节点（包含普通速度节点）     |
| `best_output_file` | `best_ips.txt`  | 仅高速节点（速度超过 `min_speed_mbps`）  |

---

### 📄 README 生成

| 配置项               | 默认值                         | 说明                                        |
|--------------------|-------------------------------|---------------------------------------------|
| `update_readme`    | `true`                        | 测速完成后是否自动更新 README 文件              |
| `readme_file`      | `README.MD`                   | README 文件路径                              |
| `raw_base_url`     | *(仓库 raw 地址)*              | 订阅链接的 URL 前缀（自动从 github_repo 推导） |
| `test_location`    | `中国四川联通`                  | 显示在 README 中的测试地点描述                 |
| `update_frequency` | `每半小时自动更新`               | 显示在 README 中的更新频率描述                 |

---

### 🔌 TCP 延迟测试

| 配置项              | 默认值   | 说明                                                 |
|-------------------|----------|------------------------------------------------------|
| `tcp_timeout_ms`  | `1500`   | 单个节点 TCP 连接超时（毫秒），超时即视为不可达            |
| `tcp_workers`     | `500`    | TCP 测试并发线程数，上限 1000，值越大速度越快但 CPU 占用越高 |
| `top_per_region`  | `10`     | 每个地区取延迟最低的前 N 个节点进入测速，过滤低质量节点      |
| `max_nodes`       | `0`      | 最多读取的节点数，`0` 表示不限制（读取全部）              |

---

### ⚡ 下载测速

| 配置项                      | 默认值  | 说明                                                     |
|---------------------------|---------|----------------------------------------------------------|
| `speed_timeout_sec`       | `6`     | 单次测速最大等待时间（秒）                                   |
| `speed_process_buffer_sec` | `8`    | 测速进程的缓冲超时（秒），略大于 `speed_timeout_sec` 即可     |
| `speed_workers`           | `16`    | 测速并发线程数，上限 128，建议不超过带宽允许的并发数             |
| `min_speed_mbps`          | `10.00` | 高速节点的最低速度阈值（Mbps），低于此值不写入 `best_ips.txt` |
| `fast_label`              | `优选高速` | 高速节点在输出文件中的标签前缀                              |
| `verbose`                 | `false` | 是否打印每个节点的详细测速日志（`true`/`false`）              |

---

### ☁ GitHub 自动推送

| 配置项                     | 默认值                    | 说明                                              |
|--------------------------|---------------------------|---------------------------------------------------|
| `github_upload_enabled`  | `false`                   | 是否启用 GitHub 自动推送                             |
| `github_repo`            | *(你的仓库地址)*            | 仓库完整地址，例如 `https://github.com/user/repo.git` |
| `github_branch`          | `main`                    | 推送的目标分支                                      |
| `github_workdir`         | `.github-sync`            | 本地 git 工作目录（程序自动管理，无需手动创建）           |
| `github_message`         | `更新 IP 结果和 README`    | git commit 的提交信息                               |
| `github_token`           | *(空)*                    | GitHub Personal Access Token，可留空改用环境变量      |
| `github_token_env`       | `GITHUB_TOKEN`            | 存放 Token 的环境变量名                              |
| `github_full_path`       | `full_ips.txt`            | 推送到仓库中的完整结果文件路径                          |
| `github_best_path`       | `best_ips.txt`            | 推送到仓库中的高速结果文件路径                          |
| `github_include_readme`  | `true`                    | 推送时是否同步 README 文件                           |
| `github_readme_path`     | `README.MD`               | 推送到仓库中的 README 文件路径                        |
| `github_push_retries`    | `3`                       | 推送失败后的最大重试次数                               |
| `github_retry_delay_sec` | `10`                      | 每次重试前的等待时间（秒）                             |
| `git_timeout_sec`        | `180`                     | 单条 git 命令的超时时间（秒）                          |

---

### 🌐 Git 代理

如果你的网络需要代理才能访问 GitHub，可以单独为 git 命令配置代理，不影响系统全局设置。

| 配置项              | 默认值 | 说明                                    |
|-------------------|--------|------------------------------------------|
| `git_http_proxy`  | *(空)* | HTTP 代理地址，例如 `http://127.0.0.1:7890` |
| `git_https_proxy` | *(空)* | HTTPS 代理地址，例如 `http://127.0.0.1:7890` |

---

## 📁 输出文件说明

程序运行后会在同目录生成以下文件：

```
cf_updater.exe
setting.config        ← 配置文件（自动生成）
ips.txt               ← 下载的原始 IP 列表
full_ips.txt          ← 所有测速完成的节点
best_ips.txt          ← 高速节点（可直接用于订阅）
README.MD             ← 自动更新的订阅说明文件
.github-sync/         ← git 工作目录（推送用，自动管理）
```

输出文件每行格式：

```
# 普通节点
1.2.3.4:443#HKG [12.34ms]

# 高速节点（速度超过阈值）
1.2.3.4:443#HKG [优选高速12.34ms]
```

---

## 🔗 GitHub 自动推送

### 配置 Token

1. 前往 GitHub → Settings → Developer settings → **Personal access tokens**
2. 生成一个具有 `repo` 权限的 Token
3. 将 Token 填入 `setting.config` 的 `github_token` 字段，**或**设置为环境变量：

```bat
set GITHUB_TOKEN=ghp_xxxxxxxxxxxxxxxx
```

### 推送流程

程序会自动完成以下步骤：

1. 若本地工作目录不存在，则 `git clone` 目标仓库
2. 将 `full_ips.txt`、`best_ips.txt`（和 README）复制进工作目录
3. `git add` → `git commit` → `git push`
4. 推送失败时自动重试（次数由 `github_push_retries` 控制）
5. 若文件内容无变化，自动跳过本次推送

### 定时自动运行（Windows 任务计划程序）

在任务计划程序中新建任务，触发器设为所需间隔，操作程序填写：

```
程序：C:\path\to\cf_updater.exe
参数：--upload
起始目录：C:\path\to\
```

---

## 📡 订阅使用方法

### EdgeTunnel 订阅

1. 打开 EdgeTunnel 后台管理页面
2. 进入「优选订阅」→「创建」
3. 订阅模式选择 **自定义**
4. 在订阅接口 / API / URL 中填入：

```
https://raw.githubusercontent.com/<用户名>/<仓库名>/refs/heads/<分支>/best_ips.txt
```

5. 验证可用性 → 选择**追加 API** → 保存并重新订阅

> 订阅链接会在每次程序运行并推送后自动更新，无需手动维护。

---

## ❓ 常见问题

**Q：程序启动后颜色显示异常或出现乱码？**

确保终端支持 ANSI 转义码。推荐使用 Windows Terminal 或 PowerShell 7+。
旧版 cmd.exe 可能显示异常，但不影响功能。

**Q：下载 IP 列表失败怎么办？**

程序会自动回退到本地的 `ips.txt` 文件。确保该文件存在且格式正确（每行 `IP:端口#地区`）。

**Q：测速结果为空？**

可能原因：TCP 超时设置过短（`tcp_timeout_ms`），或当前网络到 Cloudflare 节点整体不通畅。
尝试适当增大 `tcp_timeout_ms`，或检查 `min_speed_mbps` 是否设置过高。

**Q：GitHub 推送失败，提示凭据错误？**

确认 `github_token` 已正确填写，且 Token 具有 `repo` 写入权限，并且仓库地址格式正确（以 `.git` 结尾）。

**Q：如何只针对特定地区测速？**

修改 `input_url` 或本地 `ips.txt`，只保留目标地区的节点（地区标签在 `#` 后），然后将 `top_per_region` 适当调大。

---

## 📄 License

本项目仅供学习与个人使用。
