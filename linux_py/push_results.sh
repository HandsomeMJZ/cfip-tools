#!/usr/bin/env bash
set -euo pipefail

ENABLE_GITHUB_UPLOAD="${ENABLE_GITHUB_UPLOAD:-true}"
ENABLE_R2_UPLOAD="${ENABLE_R2_UPLOAD:-false}"

REPO="${GITHUB_REPO:-}"
BRANCH="${GITHUB_BRANCH:-main}"
TOKEN="${GITHUB_TOKEN:-}"
WORK_DIR_CONFIG="${GITHUB_WORKDIR:-.github-sync}"
WORK_DIR=""
MESSAGE="${GITHUB_MESSAGE:-Update IP results and README}"
FILES=("best_ips.txt" "full_ips.txt" "README.MD")
PUSH_RETRIES="${GITHUB_PUSH_RETRIES:-3}"
PUSH_RETRY_DELAY="${GITHUB_PUSH_RETRY_DELAY:-10}"
GIT_HTTP_PROXY="${GIT_HTTP_PROXY:-}"
GIT_HTTPS_PROXY="${GIT_HTTPS_PROXY:-$GIT_HTTP_PROXY}"

R2_ACCOUNT_ID="${R2_ACCOUNT_ID:-}"
R2_BUCKET="${R2_BUCKET:-}"
R2_PREFIX="${R2_PREFIX:-}"
R2_ENDPOINT="${R2_ENDPOINT:-}"
R2_REGION="${R2_REGION:-auto}"
R2_CACHE_CONTROL="${R2_CACHE_CONTROL:-public, max-age=300}"
R2_ACL="${R2_ACL:-}"
R2_TEXT_CONTENT_TYPE="${R2_TEXT_CONTENT_TYPE:-text/plain; charset=utf-8}"
R2_MARKDOWN_CONTENT_TYPE="${R2_MARKDOWN_CONTENT_TYPE:-text/markdown; charset=utf-8}"

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

is_enabled() {
    case "${1,,}" in
        1|true|yes|on) return 0 ;;
        0|false|no|off|"") return 1 ;;
        *) die "invalid boolean value: $1" ;;
    esac
}

die() {
    echo "$*" >&2
    exit 1
}

git_args() {
    if [[ -n "$TOKEN" ]]; then
        local basic
        basic="$(printf 'x-access-token:%s' "$TOKEN" | base64 | tr -d '\n')"
        printf '%s\n' "-c" "http.https://github.com/.extraheader=AUTHORIZATION: basic $basic"
    fi
}

run_git() {
    local cwd=""
    if [[ "${1:-}" == "--cwd" ]]; then
        cwd="$2"
        shift 2
    fi

    local args=()
    mapfile -t args < <(git_args)

    if [[ -n "$GIT_HTTP_PROXY" ]]; then
        args+=("-c" "http.proxy=$GIT_HTTP_PROXY")
    fi
    if [[ -n "$GIT_HTTPS_PROXY" ]]; then
        args+=("-c" "https.proxy=$GIT_HTTPS_PROXY")
    fi
    
    if [[ -n "$cwd" ]]; then
        args+=("-c" "safe.directory=$cwd" "-C" "$cwd")
    fi
    git "${args[@]}" "$@"
}

ensure_files() {
    local file
    for file in "${FILES[@]}"; do
        [[ -f "$file" ]] || die "result file not found: $file"
    done
}

ensure_github_ready() {
    [[ -n "$REPO" ]] || die 'GITHUB_REPO is not set. Export GITHUB_REPO or edit start.sh.'
    command -v git >/dev/null 2>&1 || die "git command not found."
    command -v realpath >/dev/null 2>&1 || die "realpath command not found."
    WORK_DIR="$(realpath -m "$ROOT/$WORK_DIR_CONFIG")"

    if [[ -z "$TOKEN" ]]; then
        echo "Warning: GITHUB_TOKEN is not set. Push may fail if git has no saved credentials." >&2
    else
        echo "GitHub token loaded from environment."
    fi
}

ensure_worktree() {
    if [[ -d "$WORK_DIR/.git" ]]; then
        run_git --cwd "$WORK_DIR" fetch origin "$BRANCH"
        echo "Resetting local sync branch to origin/$BRANCH..."
        run_git --cwd "$WORK_DIR" reset --hard
        run_git --cwd "$WORK_DIR" checkout -B "$BRANCH" "origin/$BRANCH"
        return
    fi

    if [[ -e "$WORK_DIR" ]] && find "$WORK_DIR" -mindepth 1 -print -quit | grep -q .; then
        die "sync directory is not an empty git repository: $WORK_DIR"
    fi

    mkdir -p "$(dirname "$WORK_DIR")"
    run_git clone --branch "$BRANCH" --single-branch "$REPO" "$WORK_DIR"
}

copy_results() {
    local file
    for file in "${FILES[@]}"; do
        cp -f "$file" "$WORK_DIR/$file"
        run_git --cwd "$WORK_DIR" add "$file"
    done
}

commit_if_changed() {
    if run_git --cwd "$WORK_DIR" diff --cached --quiet; then
        return
    fi

    run_git --cwd "$WORK_DIR" \
        -c user.name="IP Update Bot" \
        -c user.email="ip-update-bot@users.noreply.github.com" \
        commit -m "$MESSAGE"
}

push_if_needed() {
    local ahead
    ahead="$(run_git --cwd "$WORK_DIR" rev-list --count "origin/$BRANCH..HEAD" 2>/dev/null || printf '0')"

    if [[ "${ahead:-0}" -le 0 ]]; then
        echo "Nothing to push: ${FILES[*]} are already up to date."
        return
    fi

    echo "Pushing $ahead commit(s) to $REPO ($BRANCH)..."
    local attempt=1
    while true; do
        if run_git --cwd "$WORK_DIR" push origin "$BRANCH"; then
            break
        fi

        if [[ "$attempt" -ge "$PUSH_RETRIES" ]]; then
            echo "Push failed after $attempt attempt(s)." >&2
            return 1
        fi

        echo "Push failed; retrying push only in ${PUSH_RETRY_DELAY}s ($((attempt + 1))/$PUSH_RETRIES)..." >&2
        sleep "$PUSH_RETRY_DELAY"
        attempt=$((attempt + 1))
    done
    echo "Push done: ${FILES[*]}"
}

normalize_prefix() {
    local prefix="$1"
    prefix="${prefix#/}"
    prefix="${prefix%/}"
    printf '%s' "$prefix"
}

r2_endpoint() {
    if [[ -n "$R2_ENDPOINT" ]]; then
        printf '%s' "${R2_ENDPOINT%/}"
        return
    fi
    [[ -n "$R2_ACCOUNT_ID" ]] || die "R2_ACCOUNT_ID is not set."
    printf 'https://%s.r2.cloudflarestorage.com' "$R2_ACCOUNT_ID"
}

ensure_r2_ready() {
    [[ -n "$R2_BUCKET" ]] || die "R2_BUCKET is not set."
    [[ -n "${AWS_ACCESS_KEY_ID:-}" ]] || die "AWS_ACCESS_KEY_ID is not set for R2."
    [[ -n "${AWS_SECRET_ACCESS_KEY:-}" ]] || die "AWS_SECRET_ACCESS_KEY is not set for R2."
    command -v aws >/dev/null 2>&1 || die "aws command not found. Install AWS CLI v2 to upload to R2."
}

r2_content_type() {
    case "${1,,}" in
        *.md) printf '%s' "$R2_MARKDOWN_CONTENT_TYPE" ;;
        *.txt) printf '%s' "$R2_TEXT_CONTENT_TYPE" ;;
        *) printf '%s' "application/octet-stream" ;;
    esac
}

upload_r2() {
    local endpoint prefix file key dest
    endpoint="$(r2_endpoint)"
    prefix="$(normalize_prefix "$R2_PREFIX")"

    echo "Uploading result files to R2 bucket $R2_BUCKET..."
    for file in "${FILES[@]}"; do
        key="$file"
        if [[ -n "$prefix" ]]; then
            key="$prefix/$file"
        fi
        dest="s3://$R2_BUCKET/$key"

        local args=(
            s3 cp "$file" "$dest"
            --endpoint-url "$endpoint"
            --region "$R2_REGION"
            --cache-control "$R2_CACHE_CONTROL"
            --content-type "$(r2_content_type "$file")"
        )
        if [[ -n "$R2_ACL" ]]; then
            args+=(--acl "$R2_ACL")
        fi
        aws "${args[@]}"
    done
    echo "R2 upload done: ${FILES[*]}"
}

upload_github() {
    ensure_github_ready
    ensure_worktree
    copy_results
    commit_if_changed
    push_if_needed
}

upload_r2_all() {
    ensure_r2_ready
    upload_r2
}

UPLOAD_STATUS=0

run_upload_job() {
    local name="$1"
    shift

    local status
    set +e
    ( set -e; "$@" )
    status=$?
    set -e

    if [[ "$status" -eq 0 ]]; then
        echo "$name upload finished."
    else
        echo "$name upload failed with exit code $status." >&2
        UPLOAD_STATUS=1
    fi
}

ensure_files

if is_enabled "$ENABLE_GITHUB_UPLOAD"; then
    run_upload_job "GitHub" upload_github
else
    echo "GitHub upload disabled."
fi

if is_enabled "$ENABLE_R2_UPLOAD"; then
    run_upload_job "R2" upload_r2_all
else
    echo "R2 upload disabled."
fi

exit "$UPLOAD_STATUS"
