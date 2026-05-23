# 🌐 Cloudflare 优选 IP


**本次更新**: 2026-05-23 22:04:01
> **✨ 新增节点**: 北京电信优选    
> **测试环境**: 中国四川联通 / 北京电信    
> **更新频率**: 每 45 分钟自动更新    

---

## 🔗 订阅链接

| 线路分类 | 高速优选 (推荐) | 所有可用 (适合二次测速) |
| --- | --- | --- |
| **四川联通** | [点击复制](https://raw.githubusercontent.com/HandsomeMJZ/cfip/refs/heads/main/best_ips.txt) | [点击复制](https://raw.githubusercontent.com/HandsomeMJZ/cfip/refs/heads/main/full_ips.txt) |
| **北京电信 ✨** | [点击复制](https://raw.githubusercontent.com/HandsomeMJZ/cfip/refs/heads/main/best_ips_bj.txt) | [点击复制](https://raw.githubusercontent.com/HandsomeMJZ/cfip/refs/heads/main/full_ips_bj.txt) |

### 💡 链接区别说明

* **高速优选**: 已通过自动化测速，直接筛选出的最优节点，**强烈推荐小白用户直接使用**。
* **所有可用**: 筛选出各地区延迟最低的前 10 个节点，不保证最终吞吐速度，适合有进阶需求的用户二次筛选。

---

## 🛠️ 同款工具

如果你希望在本地自主测速，请访问：

* **仓库地址**: [HandsomeMJZ/cfip-tools](https://github.com/HandsomeMJZ/cfip-tools)
* **提示**: 仓库提供已编译好的 Windows 版本，开箱即用。

---

## 📖 使用说明

### 1. 适用范围

已有大量用户反馈，该订阅对绝大多数地区的**电信**与**联通**网络具有良好适配性。若你的网络环境不匹配，建议使用上述工具自行测速。

### 2. 自动化配置

* **本地优选**: 若无需上传同步，直接运行对应系统的 start 脚本即可。
* **自动上传**: 上传逻辑仅在 `push_results.sh` 中执行，可通过环境变量分别开启或关闭 GitHub 与 Cloudflare R2。
* **GitHub 上传**: 设置 `ENABLE_GITHUB_UPLOAD=true`、`GITHUB_REPO`、`GITHUB_TOKEN`，可选设置 `GITHUB_BRANCH`。
* **R2 上传**: 设置 `ENABLE_R2_UPLOAD=true`、`R2_ACCOUNT_ID`、`R2_BUCKET`、`AWS_ACCESS_KEY_ID`、`AWS_SECRET_ACCESS_KEY`，可选设置 `R2_PREFIX`。
* **关闭上传**: 将 `ENABLE_GITHUB_UPLOAD=false` 且 `ENABLE_R2_UPLOAD=false`，脚本只生成本地结果文件。

### 3. EdgeTunnel 部署流程

1. 复制上述对应的**订阅链接**。
2. 进入 **EdgeTunnel 后台**。
3. 创建优选订阅，模式选择 **“优选订阅模式 - 自定义”**。
4. 在 **“订阅接口 / API / URL”** 栏中粘贴链接。
5. 点击 **“验证可用性”**，随后选择 **“追加 API”**。
6. 保存并重新订阅即可。
