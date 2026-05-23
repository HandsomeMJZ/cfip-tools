export ENABLE_GITHUB_UPLOAD=true #设置为 true/on/1 来启用 GitHub 上传，false/off/0 来禁用
export ENABLE_R2_UPLOAD=true #设置为 true/on/1 来启用 R2 上传，false/off/0 来禁用 

export GITHUB_REPO="https://github.com/名字/仓库.git"
export GITHUB_TOKEN="ghp_你的token"

export R2_ACCOUNT_ID="你的账户ID"
export R2_BUCKET="你的存储桶名"
export AWS_ACCESS_KEY_ID="你的access_key"
export AWS_SECRET_ACCESS_KEY="你的secret_key"



#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

# Upload switches. Set to false/off/0 to disable, true/on/1 to enable.
export ENABLE_GITHUB_UPLOAD="${ENABLE_GITHUB_UPLOAD:-true}"
export ENABLE_R2_UPLOAD="${ENABLE_R2_UPLOAD:-false}"

# GitHub upload config. Export GITHUB_TOKEN before running this script, or rely on saved git credentials.
export GITHUB_REPO="${GITHUB_REPO:-https://github.com/your-user/your-repo.git}"
export GITHUB_BRANCH="${GITHUB_BRANCH:-main}"

# Cloudflare R2 upload config. R2 uses the S3-compatible AWS CLI variables:
# AWS_ACCESS_KEY_ID, AWS_SECRET_ACCESS_KEY, R2_ACCOUNT_ID, R2_BUCKET.
export R2_ACCOUNT_ID="${R2_ACCOUNT_ID:-}"
export R2_BUCKET="${R2_BUCKET:-}"
export R2_PREFIX="${R2_PREFIX:-}"
export R2_REGION="${R2_REGION:-auto}"
export R2_CACHE_CONTROL="${R2_CACHE_CONTROL:-public, max-age=300}"

require_result_files() {
    local file
    for file in best_ips.txt full_ips.txt README.MD; do
        if [[ ! -f "$ROOT/$file" ]]; then
            echo "Missing result file: $file. Choose 2 to regenerate results first." >&2
            exit 1
        fi
    done
}

run_update() {
    python3 "$ROOT/update.py" --show-latency false --show-mbps true
    python3 "$ROOT/update_md.py"
}

run_push() {
    require_result_files
    "$ROOT/push_results.sh"
}

echo
echo "Choose an action:"
echo "1. Push existing result files"
echo "2. Regenerate results, then push (Default in 5s)"
echo

if ! read -t 5 -r -p "Enter 1 or 2 [Default: 2]: " choice; then
    echo -e "\n\nTime out! Automatically selecting choice 2..."
    choice=2
fi

choice="${choice//[[:space:]]/}"
if [[ -z "$choice" ]]; then
    choice=2
fi

case "$choice" in
    1)
        run_push
        ;;
    2)
        run_update
        run_push
        ;;
    *)
        echo "Invalid choice: $choice" >&2
        exit 1
        ;;
esac
