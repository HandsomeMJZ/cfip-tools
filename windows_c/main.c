#define _WIN32_WINNT 0x0601

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <direct.h>
#include <ctype.h>
#include <errno.h>
#include <io.h>
#include <process.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ─── 版本 ─────────────────────────────────────────────── */
#define APP_VERSION "1.2.0"

/* ─── 测速参数 ──────────────────────────────────────────── */
#define SPEED_DOMAIN "speed.cloudflare.com"
#define SPEED_PATH   "/__down"
#define SPEED_BYTES  2097152

/* ─── ANSI 颜色（Windows 10+ 虚拟终端支持） ──────────────── */
#define COL_RESET   "\x1b[0m"
#define COL_BOLD    "\x1b[1m"
#define COL_DIM     "\x1b[2m"
#define COL_CYAN    "\x1b[96m"
#define COL_GREEN   "\x1b[92m"
#define COL_YELLOW  "\x1b[93m"
#define COL_RED     "\x1b[91m"
#define COL_BLUE    "\x1b[94m"
#define COL_MAGENTA "\x1b[95m"
#define COL_WHITE   "\x1b[97m"

/* ─── 进度条宽度 ─────────────────────────────────────────── */
#define BAR_WIDTH 40

/* ──────────────────────────────────────────────────────────
   数据结构
   ────────────────────────────────────────────────────────── */

typedef struct {
    char input_file[MAX_PATH];
    char input_url[1024];
    int  download_input;
    int  download_timeout;
    char full_output_file[MAX_PATH];
    char best_output_file[MAX_PATH];
    char readme_file[MAX_PATH];
    int  update_readme;
    char raw_base_url[1024];
    char test_location[256];
    char update_frequency[256];

    int    tcp_timeout_ms;
    int    tcp_workers;
    int    speed_timeout_sec;
    int    speed_process_buffer_sec;
    int    speed_workers;
    double min_speed_mbps;
    int    top_per_region;
    int    max_nodes;
    int    verbose;
    int    NO;
    int    show_latency;
    int    show_bandwidth;
    char   fast_label[128];

    int  github_upload_enabled;
    char github_repo[1024];
    char github_branch[128];
    char github_workdir[MAX_PATH];
    char github_message[512];
    char github_token[1024];
    char github_token_env[128];
    char github_best_path[MAX_PATH];
    char github_full_path[MAX_PATH];
    char github_readme_path[MAX_PATH];
    int  github_include_readme;
    int  github_push_retries;
    int  github_retry_delay_sec;
    int  git_timeout_sec;
    char git_http_proxy[512];
    char git_https_proxy[512];

    int  r2_upload_enabled;
    char r2_account_id[128];
    char r2_bucket[256];
    char r2_endpoint[1024];
    char r2_access_key_id[512];
    char r2_secret_access_key[1024];
    char r2_access_key_env[128];
    char r2_secret_key_env[128];
    char r2_prefix[MAX_PATH];
    char r2_full_path[MAX_PATH];
    char r2_best_path[MAX_PATH];
    int  r2_include_readme;
    char r2_readme_path[MAX_PATH];
    int  r2_timeout_sec;
    int  r2_retries;
    int  r2_retry_delay_sec;
} Config;

typedef struct { char ip[128]; int port; char region[128]; } Node;
typedef struct { Node node; double latency_ms; }                TcpResult;
typedef struct { Node node; double latency_ms; double speed_mbps; int is_fast; } SpeedResult;

typedef struct { Node       *items; size_t count; size_t capacity; } NodeArray;
typedef struct { TcpResult  *items; size_t count; size_t capacity; } TcpArray;
typedef struct { SpeedResult*items; size_t count; size_t capacity; } SpeedArray;

typedef struct {
    Node     *nodes;
    size_t    count;
    size_t    next_index;
    TcpArray *results;
    Config   *config;
    CRITICAL_SECTION lock;
    LONG done;
} TcpContext;

typedef struct {
    TcpResult  *items;
    size_t      count;
    size_t      next_index;
    SpeedArray *results;
    Config     *config;
    CRITICAL_SECTION lock;
    LONG done;
} SpeedContext;

/* ──────────────────────────────────────────────────────────
   前向声明
   ────────────────────────────────────────────────────────── */
static int  ensure_parent_dir(const char *path);
static int  file_exists(const char *path);

/* ──────────────────────────────────────────────────────────
   终端工具
   ────────────────────────────────────────────────────────── */

/* 开启 Windows 虚拟终端 ANSI 支持 */
static void enable_virtual_terminal(void) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode)) {
        SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
}

/* 打印分隔线 */
static void print_divider(const char *color) {
    printf("%s%s────────────────────────────────────────────────────%s\n",
           COL_BOLD, color, COL_RESET);
}

/* 打印带颜色的节标题 */
static void print_section(const char *icon, const char *title) {
    printf("\n%s%s %s%s%s\n", COL_BOLD, icon, COL_CYAN, title, COL_RESET);
    print_divider(COL_DIM);
}

/* 渲染进度条：[████████░░░░░░] 35/100 */
static void render_progress(LONG done, size_t total, const char *label) {
    int filled, i;
    double pct = (total > 0) ? (double)done / (double)total : 0.0;
    filled = (int)(pct * BAR_WIDTH);

    printf("\r  %s%s%s [", COL_BOLD, label, COL_RESET);
    for (i = 0; i < BAR_WIDTH; i++) {
        if (i < filled)
            printf("%s█%s", COL_CYAN, COL_RESET);
        else
            printf("%s░%s", COL_DIM, COL_RESET);
    }
    printf("] %s%ld/%zu%s", COL_YELLOW, done, total, COL_RESET);
    fflush(stdout);
}

/* 打印启动 Banner */
static void print_banner(void) {
    printf("%s", COL_CYAN COL_BOLD);
    printf("  ╔═══════════════════════════════════════════════════╗\n");
    printf("  ║       Cloudflare IP 优选工具  v%-6s             ║\n", APP_VERSION);
    printf("  ║      CF IP Speed Tester  -  Windows Edition       ║\n");
    printf("  ╚═══════════════════════════════════════════════════╝\n");
    printf("%s\n", COL_RESET);
}

/* 打印一行 key: value 格式的信息 */
static void print_kv(const char *key, const char *val_fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, val_fmt);
    vsnprintf(buf, sizeof(buf), val_fmt, ap);
    va_end(ap);
    printf("  %s%-18s%s %s%s%s\n",
           COL_DIM, key, COL_RESET,
           COL_WHITE, buf, COL_RESET);
}

/* ──────────────────────────────────────────────────────────
   字符串工具
   ────────────────────────────────────────────────────────── */

static char *ltrim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    return s;
}
static void rtrim_inplace(char *s) {
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
}
static char *trim(char *s) { s = ltrim(s); rtrim_inplace(s); return s; }

static void strip_inline_comment(char *s) {
    char *p;
    for (p = s; *p; p++) {
        if ((*p == '#' || *p == ';') && (p == s || isspace((unsigned char)p[-1]))) {
            *p = '\0';
            rtrim_inplace(s);
            return;
        }
    }
}

static int str_ieq(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

static int parse_bool(const char *value, int fallback) {
    if (str_ieq(value,"1")||str_ieq(value,"true")||str_ieq(value,"yes")||str_ieq(value,"y")||str_ieq(value,"on"))  return 1;
    if (str_ieq(value,"0")||str_ieq(value,"false")||str_ieq(value,"no")||str_ieq(value,"n")||str_ieq(value,"off")) return 0;
    return fallback;
}

static void copy_text(char *dst, size_t dst_size, const char *src) {
    if (dst_size == 0) return;
    snprintf(dst, dst_size, "%s", src ? src : "");
}

static void strip_region_number(const char *region, char *out, size_t out_size) {
    size_t n, i;
    copy_text(out, out_size, region);
    n = strlen(out);
    i = n;
    while (i > 0 && isdigit((unsigned char)out[i - 1])) i--;
    if (i < n && i > 1 && out[i - 1] == '_') {
        out[i - 1] = '\0';
    }
}

/* ──────────────────────────────────────────────────────────
   配置 — 默认值 / 加载 / 保存
   ────────────────────────────────────────────────────────── */

static void default_config(Config *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    copy_text(cfg->input_file,        sizeof(cfg->input_file),        "ips.txt");
    copy_text(cfg->input_url,         sizeof(cfg->input_url),         "https://zip.cm.edu.kg/all.txt");
    cfg->download_input    = 1;
    cfg->download_timeout  = 30;
    copy_text(cfg->full_output_file,  sizeof(cfg->full_output_file),  "full_ips.txt");
    copy_text(cfg->best_output_file,  sizeof(cfg->best_output_file),  "best_ips.txt");
    copy_text(cfg->readme_file,       sizeof(cfg->readme_file),       "README.MD");
    cfg->update_readme     = 1;
    copy_text(cfg->raw_base_url,      sizeof(cfg->raw_base_url),      "https://raw.githubusercontent.com/HandsomeMJZ/cfip/refs/heads/main");
    copy_text(cfg->test_location,     sizeof(cfg->test_location),     "中国四川联通");
    copy_text(cfg->update_frequency,  sizeof(cfg->update_frequency),  "每半小时自动更新");
    cfg->tcp_timeout_ms            = 1500;
    cfg->tcp_workers               = 500;
    cfg->speed_timeout_sec         = 3;
    cfg->speed_process_buffer_sec  = 5;
    cfg->speed_workers             = 32;
    cfg->min_speed_mbps            = 12.0;
    cfg->top_per_region            = 8;
    cfg->max_nodes                 = 0;
    cfg->verbose                   = 0;
    cfg->NO                        = 0;
    cfg->show_latency              = 1;
    cfg->show_bandwidth            = 1;
    copy_text(cfg->fast_label,        sizeof(cfg->fast_label),        "优选高速");
    cfg->github_upload_enabled     = 0;
    copy_text(cfg->github_repo,       sizeof(cfg->github_repo),       "https://github.com/HandsomeMJZ/cfip.git");
    copy_text(cfg->github_branch,     sizeof(cfg->github_branch),     "main");
    copy_text(cfg->github_workdir,    sizeof(cfg->github_workdir),    ".github-sync");
    copy_text(cfg->github_message,    sizeof(cfg->github_message),    "Update IP results and README");
    copy_text(cfg->github_token,      sizeof(cfg->github_token),      "");
    copy_text(cfg->github_token_env,  sizeof(cfg->github_token_env),  "GITHUB_TOKEN");
    copy_text(cfg->github_best_path,  sizeof(cfg->github_best_path),  "best_ips.txt");
    copy_text(cfg->github_full_path,  sizeof(cfg->github_full_path),  "full_ips.txt");
    copy_text(cfg->github_readme_path,sizeof(cfg->github_readme_path),"README.MD");
    cfg->github_include_readme     = 1;
    cfg->github_push_retries       = 3;
    cfg->github_retry_delay_sec    = 10;
    cfg->git_timeout_sec           = 180;
    cfg->r2_upload_enabled         = 0;
    copy_text(cfg->r2_account_id,          sizeof(cfg->r2_account_id),          "");
    copy_text(cfg->r2_bucket,              sizeof(cfg->r2_bucket),              "");
    copy_text(cfg->r2_endpoint,            sizeof(cfg->r2_endpoint),            "");
    copy_text(cfg->r2_access_key_id,       sizeof(cfg->r2_access_key_id),       "");
    copy_text(cfg->r2_secret_access_key,   sizeof(cfg->r2_secret_access_key),   "");
    copy_text(cfg->r2_access_key_env,      sizeof(cfg->r2_access_key_env),      "R2_ACCESS_KEY_ID");
    copy_text(cfg->r2_secret_key_env,      sizeof(cfg->r2_secret_key_env),      "R2_SECRET_ACCESS_KEY");
    copy_text(cfg->r2_prefix,              sizeof(cfg->r2_prefix),              "");
    copy_text(cfg->r2_full_path,           sizeof(cfg->r2_full_path),           "full_ips.txt");
    copy_text(cfg->r2_best_path,           sizeof(cfg->r2_best_path),           "best_ips.txt");
    cfg->r2_include_readme          = 1;
    copy_text(cfg->r2_readme_path,         sizeof(cfg->r2_readme_path),         "README.MD");
    cfg->r2_timeout_sec            = 120;
    cfg->r2_retries                = 3;
    cfg->r2_retry_delay_sec        = 10;
}

static void apply_localized_defaults(Config *cfg) {
    copy_text(cfg->test_location,    sizeof(cfg->test_location),    "China");
    copy_text(cfg->update_frequency, sizeof(cfg->update_frequency), "每半小时自动更新");
    copy_text(cfg->fast_label,       sizeof(cfg->fast_label),       "优选高速");
    copy_text(cfg->github_message,   sizeof(cfg->github_message),   "Update IP and README");
}

static void apply_config_value(Config *cfg, const char *key, const char *value) {
    if      (str_ieq(key,"input_file"))             copy_text(cfg->input_file,sizeof(cfg->input_file),value);
    else if (str_ieq(key,"input_url"))              copy_text(cfg->input_url,sizeof(cfg->input_url),value);
    else if (str_ieq(key,"download_input"))         cfg->download_input=parse_bool(value,cfg->download_input);
    else if (str_ieq(key,"download_timeout"))       cfg->download_timeout=atoi(value);
    else if (str_ieq(key,"full_output_file"))       copy_text(cfg->full_output_file,sizeof(cfg->full_output_file),value);
    else if (str_ieq(key,"best_output_file"))       copy_text(cfg->best_output_file,sizeof(cfg->best_output_file),value);
    else if (str_ieq(key,"readme_file"))            copy_text(cfg->readme_file,sizeof(cfg->readme_file),value);
    else if (str_ieq(key,"update_readme"))          cfg->update_readme=parse_bool(value,cfg->update_readme);
    else if (str_ieq(key,"raw_base_url"))           copy_text(cfg->raw_base_url,sizeof(cfg->raw_base_url),value);
    else if (str_ieq(key,"test_location"))          copy_text(cfg->test_location,sizeof(cfg->test_location),value);
    else if (str_ieq(key,"update_frequency"))       copy_text(cfg->update_frequency,sizeof(cfg->update_frequency),value);
    else if (str_ieq(key,"tcp_timeout_ms"))         cfg->tcp_timeout_ms=atoi(value);
    else if (str_ieq(key,"tcp_workers"))            cfg->tcp_workers=atoi(value);
    else if (str_ieq(key,"speed_timeout_sec"))      cfg->speed_timeout_sec=atoi(value);
    else if (str_ieq(key,"speed_process_buffer_sec")) cfg->speed_process_buffer_sec=atoi(value);
    else if (str_ieq(key,"speed_workers"))          cfg->speed_workers=atoi(value);
    else if (str_ieq(key,"min_speed_mbps"))         cfg->min_speed_mbps=atof(value);
    else if (str_ieq(key,"top_per_region"))         cfg->top_per_region=atoi(value);
    else if (str_ieq(key,"max_nodes"))              cfg->max_nodes=atoi(value);
    else if (str_ieq(key,"verbose"))                cfg->verbose=parse_bool(value,cfg->verbose);
    else if (str_ieq(key,"NO"))                     cfg->NO=parse_bool(value,cfg->NO);
    else if (str_ieq(key,"show_latency"))           cfg->show_latency=parse_bool(value,cfg->show_latency);
    else if (str_ieq(key,"show_bandwidth"))         cfg->show_bandwidth=parse_bool(value,cfg->show_bandwidth);
    else if (str_ieq(key,"fast_label"))             copy_text(cfg->fast_label,sizeof(cfg->fast_label),value);
    else if (str_ieq(key,"github_upload_enabled"))  cfg->github_upload_enabled=parse_bool(value,cfg->github_upload_enabled);
    else if (str_ieq(key,"github_repo"))            copy_text(cfg->github_repo,sizeof(cfg->github_repo),value);
    else if (str_ieq(key,"github_branch"))          copy_text(cfg->github_branch,sizeof(cfg->github_branch),value);
    else if (str_ieq(key,"github_workdir"))         copy_text(cfg->github_workdir,sizeof(cfg->github_workdir),value);
    else if (str_ieq(key,"github_message"))         copy_text(cfg->github_message,sizeof(cfg->github_message),value);
    else if (str_ieq(key,"github_token"))           copy_text(cfg->github_token,sizeof(cfg->github_token),value);
    else if (str_ieq(key,"github_token_env"))       copy_text(cfg->github_token_env,sizeof(cfg->github_token_env),value);
    else if (str_ieq(key,"github_best_path"))       copy_text(cfg->github_best_path,sizeof(cfg->github_best_path),value);
    else if (str_ieq(key,"github_full_path"))       copy_text(cfg->github_full_path,sizeof(cfg->github_full_path),value);
    else if (str_ieq(key,"github_readme_path"))     copy_text(cfg->github_readme_path,sizeof(cfg->github_readme_path),value);
    else if (str_ieq(key,"github_include_readme"))  cfg->github_include_readme=parse_bool(value,cfg->github_include_readme);
    else if (str_ieq(key,"github_push_retries"))    cfg->github_push_retries=atoi(value);
    else if (str_ieq(key,"github_retry_delay_sec")) cfg->github_retry_delay_sec=atoi(value);
    else if (str_ieq(key,"git_timeout_sec"))        cfg->git_timeout_sec=atoi(value);
    else if (str_ieq(key,"git_http_proxy"))         copy_text(cfg->git_http_proxy,sizeof(cfg->git_http_proxy),value);
    else if (str_ieq(key,"git_https_proxy"))        copy_text(cfg->git_https_proxy,sizeof(cfg->git_https_proxy),value);
    else if (str_ieq(key,"r2_upload_enabled"))      cfg->r2_upload_enabled=parse_bool(value,cfg->r2_upload_enabled);
    else if (str_ieq(key,"r2_account_id"))          copy_text(cfg->r2_account_id,sizeof(cfg->r2_account_id),value);
    else if (str_ieq(key,"r2_bucket"))              copy_text(cfg->r2_bucket,sizeof(cfg->r2_bucket),value);
    else if (str_ieq(key,"r2_endpoint"))            copy_text(cfg->r2_endpoint,sizeof(cfg->r2_endpoint),value);
    else if (str_ieq(key,"r2_access_key_id"))       copy_text(cfg->r2_access_key_id,sizeof(cfg->r2_access_key_id),value);
    else if (str_ieq(key,"r2_secret_access_key"))   copy_text(cfg->r2_secret_access_key,sizeof(cfg->r2_secret_access_key),value);
    else if (str_ieq(key,"r2_access_key_env"))      copy_text(cfg->r2_access_key_env,sizeof(cfg->r2_access_key_env),value);
    else if (str_ieq(key,"r2_secret_key_env"))      copy_text(cfg->r2_secret_key_env,sizeof(cfg->r2_secret_key_env),value);
    else if (str_ieq(key,"r2_prefix"))              copy_text(cfg->r2_prefix,sizeof(cfg->r2_prefix),value);
    else if (str_ieq(key,"r2_full_path"))           copy_text(cfg->r2_full_path,sizeof(cfg->r2_full_path),value);
    else if (str_ieq(key,"r2_best_path"))           copy_text(cfg->r2_best_path,sizeof(cfg->r2_best_path),value);
    else if (str_ieq(key,"r2_include_readme"))      cfg->r2_include_readme=parse_bool(value,cfg->r2_include_readme);
    else if (str_ieq(key,"r2_readme_path"))         copy_text(cfg->r2_readme_path,sizeof(cfg->r2_readme_path),value);
    else if (str_ieq(key,"r2_timeout_sec"))         cfg->r2_timeout_sec=atoi(value);
    else if (str_ieq(key,"r2_retries"))             cfg->r2_retries=atoi(value);
    else if (str_ieq(key,"r2_retry_delay_sec"))     cfg->r2_retry_delay_sec=atoi(value);
}

static int load_config(Config *cfg, const char *path) {
    FILE *fp = fopen(path, "rb");
    char line[2048];
    if (!fp) return 0;
    while (fgets(line, sizeof(line), fp)) {
        char *p = trim(line);
        char *eq;
        if (*p == '\0' || *p == '#' || *p == ';') continue;
        eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        p = trim(p);
        strip_inline_comment(eq + 1);
        apply_config_value(cfg, p, trim(eq + 1));
    }
    fclose(fp);
    return 1;
}

static int save_config(const Config *cfg, const char *path) {
    FILE *fp;
    ensure_parent_dir(path);
    fp = fopen(path, "wb");
    if (!fp) {
        fprintf(stderr, "%s错误：无法写入配置文件：%s%s\n", COL_RED, path, COL_RESET);
        return 0;
    }
    fprintf(fp, "# Cloudflare IP 优选工具配置  (v%s)\n", APP_VERSION);
    fprintf(fp, "# 重新配置请运行：cf_updater.exe --setup\n\n");

    fprintf(fp, "# ── 输入源 ──\n");
    fprintf(fp, "input_file=%s # 本地 IP 列表文件路径，程序从这里读取待测试节点\n", cfg->input_file);
    fprintf(fp, "input_url=%s # 在线 IP 列表地址，download_input=true 时会先下载到 input_file\n", cfg->input_url);
    fprintf(fp, "download_input=%s # 是否运行时自动下载 input_url 覆盖本地 input_file\n", cfg->download_input ? "true" : "false");
    fprintf(fp, "download_timeout=%d # 下载 input_url 的连接和总超时时间，单位秒\n\n", cfg->download_timeout);

    fprintf(fp, "# ── 输出文件 ──\n");
    fprintf(fp, "full_output_file=%s # 保存所有测速成功节点的本地文件\n", cfg->full_output_file);
    fprintf(fp, "best_output_file=%s # 保存达到 min_speed_mbps 阈值节点的本地文件\n\n", cfg->best_output_file);

    fprintf(fp, "# ── README 生成 ──\n");
    fprintf(fp, "update_readme=%s # 是否每次运行后自动生成 README 文件\n", cfg->update_readme ? "true" : "false");
    fprintf(fp, "readme_file=%s # README 本地输出文件路径\n", cfg->readme_file);
    fprintf(fp, "raw_base_url=%s # README 中订阅链接的基础地址，通常是 GitHub Raw 或 R2 公网地址\n", cfg->raw_base_url);
    fprintf(fp, "test_location=%s # README 中显示的测试地点说明\n", cfg->test_location);
    fprintf(fp, "update_frequency=%s # README 中显示的更新频率说明\n\n", cfg->update_frequency);

    fprintf(fp, "# ── TCP 与下载测速 ──\n");
    fprintf(fp, "tcp_timeout_ms=%d # TCP 延迟测试单个节点超时时间，单位毫秒\n", cfg->tcp_timeout_ms);
    fprintf(fp, "tcp_workers=%d # TCP 延迟测试并发线程数，越大越快但越占资源\n", cfg->tcp_workers);
    fprintf(fp, "speed_timeout_sec=%d # 下载测速单个节点总超时时间，单位秒\n", cfg->speed_timeout_sec);
    fprintf(fp, "speed_process_buffer_sec=%d # 预留测速进程缓冲时间，当前用于兼容旧配置\n", cfg->speed_process_buffer_sec);
    fprintf(fp, "speed_workers=%d # 下载测速并发线程数\n", cfg->speed_workers);
    fprintf(fp, "min_speed_mbps=%.1f # 判定为高速节点的最低带宽阈值，单位 Mbps\n", cfg->min_speed_mbps);
    fprintf(fp, "top_per_region=%d # 每个地区进入下载测速的 TCP 低延迟候选数量\n", cfg->top_per_region);
    fprintf(fp, "max_nodes=%d # 最多读取多少个节点，0 表示不限制\n", cfg->max_nodes);
    fprintf(fp, "verbose=%s # 是否打印每个节点的详细测速日志\n", cfg->verbose ? "true" : "false");
    fprintf(fp, "NO=%s # 是否给同地区输出名追加 _1、_2 这样的序号\n", cfg->NO ? "true" : "false");
    fprintf(fp, "show_latency=%s # 是否在输出节点标签中显示延迟，例如 [45ms]\n", cfg->show_latency ? "true" : "false");
    fprintf(fp, "show_bandwidth=%s # 是否在输出节点标签中显示带宽，精确到个位 Mbps\n", cfg->show_bandwidth ? "true" : "false");
    fprintf(fp, "fast_label=%s # 高速节点标签前缀，例如 [优选高速45ms 28Mbps]\n\n", cfg->fast_label);

    fprintf(fp, "# ── GitHub 推送（github_token 可留空，程序会读取环境变量）──\n");
    fprintf(fp, "github_upload_enabled=%s # 是否启用 GitHub 自动上传 true=上传 false=跳过\n", cfg->github_upload_enabled ? "true" : "false");
    fprintf(fp, "github_repo=%s # GitHub 仓库地址，支持 https://github.com/user/repo.git\n", cfg->github_repo);
    fprintf(fp, "github_branch=%s # 上传到 GitHub 的目标分支\n", cfg->github_branch);
    fprintf(fp, "github_workdir=%s # 本地临时克隆目录，用于 git add/commit/push\n", cfg->github_workdir);
    fprintf(fp, "github_message=%s # GitHub 自动提交时使用的 commit message\n", cfg->github_message);
    fprintf(fp, "github_token=%s # GitHub Token；留空时读取 github_token_env 指定的环境变量\n", cfg->github_token);
    fprintf(fp, "github_token_env=%s # GitHub Token 的环境变量名\n", cfg->github_token_env);
    fprintf(fp, "github_full_path=%s # full_output_file 上传到仓库内的目标路径\n", cfg->github_full_path);
    fprintf(fp, "github_best_path=%s # best_output_file 上传到仓库内的目标路径\n", cfg->github_best_path);
    fprintf(fp, "github_include_readme=%s # 是否同时上传 README 文件\n", cfg->github_include_readme ? "true" : "false");
    fprintf(fp, "github_readme_path=%s # README 上传到仓库内的目标路径\n", cfg->github_readme_path);
    fprintf(fp, "github_push_retries=%d # GitHub push 失败后的最大重试次数\n", cfg->github_push_retries);
    fprintf(fp, "github_retry_delay_sec=%d # GitHub push 每次重试前等待秒数\n", cfg->github_retry_delay_sec);
    fprintf(fp, "git_timeout_sec=%d # Git 命令超时时间，单位秒，当前保留用于兼容配置\n\n", cfg->git_timeout_sec);

    fprintf(fp, "# ── Git 代理（示例：http://127.0.0.1:7890）──\n");
    fprintf(fp, "git_http_proxy=%s # Git HTTP 代理地址，留空表示不用代理\n", cfg->git_http_proxy);
    fprintf(fp, "git_https_proxy=%s # Git HTTPS 代理地址，留空表示不用代理\n\n", cfg->git_https_proxy);

    fprintf(fp, "# ── Cloudflare R2 上传（密钥可留空，程序会读取环境变量）──\n");
    fprintf(fp, "r2_upload_enabled=%s # 是否启用 Cloudflare R2 自动上传 true=上传 false=跳过\n", cfg->r2_upload_enabled ? "true" : "false");
    fprintf(fp, "r2_account_id=%s # Cloudflare 账户 ID，用于拼接默认 R2 S3 API 端点\n", cfg->r2_account_id);
    fprintf(fp, "r2_bucket=%s # R2 存储桶名称\n", cfg->r2_bucket);
    fprintf(fp, "r2_endpoint=%s # 自定义 R2 S3 API 端点；留空时使用 https://<account_id>.r2.cloudflarestorage.com\n", cfg->r2_endpoint);
    fprintf(fp, "r2_access_key_id=%s # R2 Access Key ID；留空时读取 r2_access_key_env 指定的环境变量\n", cfg->r2_access_key_id);
    fprintf(fp, "r2_secret_access_key=%s # R2 Secret Access Key；留空时读取 r2_secret_key_env 指定的环境变量\n", cfg->r2_secret_access_key);
    fprintf(fp, "r2_access_key_env=%s # R2 Access Key ID 的环境变量名\n", cfg->r2_access_key_env);
    fprintf(fp, "r2_secret_key_env=%s # R2 Secret Access Key 的环境变量名\n", cfg->r2_secret_key_env);
    fprintf(fp, "r2_prefix=%s # R2 对象统一前缀目录，例如 cfip；留空表示桶根目录\n", cfg->r2_prefix);
    fprintf(fp, "r2_full_path=%s # full_output_file 上传到 R2 的对象名或相对路径\n", cfg->r2_full_path);
    fprintf(fp, "r2_best_path=%s # best_output_file 上传到 R2 的对象名或相对路径\n", cfg->r2_best_path);
    fprintf(fp, "r2_include_readme=%s # 是否同时上传 README 文件到 R2\n", cfg->r2_include_readme ? "true" : "false");
    fprintf(fp, "r2_readme_path=%s # README 上传到 R2 的对象名或相对路径\n", cfg->r2_readme_path);
    fprintf(fp, "r2_timeout_sec=%d # 单个 R2 文件上传超时时间，单位秒\n", cfg->r2_timeout_sec);
    fprintf(fp, "r2_retries=%d # R2 上传失败后的最大重试次数\n", cfg->r2_retries);
    fprintf(fp, "r2_retry_delay_sec=%d # R2 上传每次重试前等待秒数\n", cfg->r2_retry_delay_sec);
    fclose(fp);
    return 1;
}

/* ──────────────────────────────────────────────────────────
   配置参数校验（clamp）
   ────────────────────────────────────────────────────────── */
static void clamp_config(Config *cfg) {
    if (cfg->download_timeout <= 0)       cfg->download_timeout = 30;
    if (cfg->tcp_timeout_ms <= 0)         cfg->tcp_timeout_ms = 1500;
    if (cfg->tcp_workers <= 0)            cfg->tcp_workers = 1;
    if (cfg->tcp_workers > 1000)          cfg->tcp_workers = 1000;   /* 上限调高至1000 */
    if (cfg->speed_timeout_sec <= 0)      cfg->speed_timeout_sec = 6;
    if (cfg->speed_process_buffer_sec < 0) cfg->speed_process_buffer_sec = 0;
    if (cfg->speed_workers <= 0)          cfg->speed_workers = 1;
    if (cfg->speed_workers > 128)         cfg->speed_workers = 128;  /* 上限调高至128 */
    if (cfg->min_speed_mbps < 0)          cfg->min_speed_mbps = 0;
    if (cfg->top_per_region <= 0)         cfg->top_per_region = 1;
    if (cfg->github_push_retries <= 0)    cfg->github_push_retries = 1;
    if (cfg->github_retry_delay_sec < 0)  cfg->github_retry_delay_sec = 0;
    if (cfg->git_timeout_sec <= 0)        cfg->git_timeout_sec = 180;
    if (cfg->r2_timeout_sec <= 0)         cfg->r2_timeout_sec = 120;
    if (cfg->r2_retries <= 0)             cfg->r2_retries = 1;
    if (cfg->r2_retry_delay_sec < 0)      cfg->r2_retry_delay_sec = 0;
}

/* ──────────────────────────────────────────────────────────
   配置向导
   ────────────────────────────────────────────────────────── */

static void read_prompt(const char *prompt, char *out, size_t out_size, const char *fallback) {
    char line[2048];
    printf("  %s", prompt);
    fflush(stdout);
    if (!fgets(line, sizeof(line), stdin)) {
        copy_text(out, out_size, fallback);
        return;
    }
    copy_text(out, out_size, trim(line));
    if (out[0] == '\0') copy_text(out, out_size, fallback);
}

static int prompt_bool(const char *prompt, int fallback) {
    char line[64];
    printf("  %s %s(%s)%s: ", prompt,
           COL_DIM, fallback ? "Y/n" : "y/N", COL_RESET);
    fflush(stdout);
    if (!fgets(line, sizeof(line), stdin)) return fallback;
    trim(line);
    if (line[0] == '\0') return fallback;
    return parse_bool(line, fallback);
}

static void normalize_github_repo_to_raw(const char *repo, const char *branch, char *out, size_t out_size) {
    char work[1024];
    char *start, *end;
    copy_text(work, sizeof(work), repo);
    start = strstr(work, "github.com/");
    if (start) { start += strlen("github.com/"); }
    else {
        start = strstr(work, "github.com:");
        if (start) start += strlen("github.com:");
    }
    if (!start || !*start) return;
    end = strstr(start, ".git");
    if (end && end[4] == '\0') *end = '\0';
    snprintf(out, out_size, "https://raw.githubusercontent.com/%s/refs/heads/%s", start, branch);
}

static void run_setup_wizard(Config *cfg, const char *config_path, int first_run) {
    char value[2048];

    print_section("⚙", "首次配置向导");
    if (first_run) {
        printf("  %s已生成默认配置文件：%s%s\n", COL_DIM, config_path, COL_RESET);
    }
    printf("  %s直接回车保留括号内的默认值。%s\n\n", COL_DIM, COL_RESET);

    cfg->NO = prompt_bool("启用优选序号 (#HK_1)", cfg->NO);
    cfg->show_latency = prompt_bool("显示延迟信息 ([xxms])", cfg->show_latency);
    cfg->show_bandwidth = prompt_bool("显示带宽信息 ([xxMbps])", cfg->show_bandwidth);
    cfg->github_upload_enabled = prompt_bool("启用 GitHub 自动推送", cfg->github_upload_enabled);

    read_prompt("GitHub 仓库地址 (https://github.com/user/repo.git)\n"
                "  " COL_DIM "[默认: " COL_RESET,
                value, sizeof(value), cfg->github_repo);
    /* 补全右括号提示已在 read_prompt 前 printf 中以颜色呈现，此处不需额外输出 */
    copy_text(cfg->github_repo,      sizeof(cfg->github_repo),      value);

    read_prompt("分支名 [默认: " COL_RESET, value, sizeof(value), cfg->github_branch);
    copy_text(cfg->github_branch,    sizeof(cfg->github_branch),    value);

    read_prompt("GitHub Token（可留空，改用环境变量）[默认: 空]: ",
                value, sizeof(value), cfg->github_token);
    copy_text(cfg->github_token,     sizeof(cfg->github_token),     value);

    read_prompt("Token 环境变量名 [默认: " COL_RESET, value, sizeof(value), cfg->github_token_env);
    copy_text(cfg->github_token_env, sizeof(cfg->github_token_env), value);

    read_prompt("仓库内 best_ips.txt 路径 [默认: " COL_RESET, value, sizeof(value), cfg->github_best_path);
    copy_text(cfg->github_best_path, sizeof(cfg->github_best_path), value);

    read_prompt("仓库内 full_ips.txt 路径 [默认: " COL_RESET, value, sizeof(value), cfg->github_full_path);
    copy_text(cfg->github_full_path, sizeof(cfg->github_full_path), value);

    cfg->github_include_readme = prompt_bool("同步 README.MD", cfg->github_include_readme);

    cfg->r2_upload_enabled = prompt_bool("启用 Cloudflare R2 自动上传", cfg->r2_upload_enabled);

    read_prompt("Cloudflare Account ID [默认: 空]: ",
                value, sizeof(value), cfg->r2_account_id);
    copy_text(cfg->r2_account_id, sizeof(cfg->r2_account_id), value);

    read_prompt("R2 存储桶名称 [默认: 空]: ",
                value, sizeof(value), cfg->r2_bucket);
    copy_text(cfg->r2_bucket, sizeof(cfg->r2_bucket), value);

    read_prompt("R2 Access Key ID（可留空，改用环境变量）[默认: 空]: ",
                value, sizeof(value), cfg->r2_access_key_id);
    copy_text(cfg->r2_access_key_id, sizeof(cfg->r2_access_key_id), value);

    read_prompt("R2 Secret Access Key（可留空，改用环境变量）[默认: 空]: ",
                value, sizeof(value), cfg->r2_secret_access_key);
    copy_text(cfg->r2_secret_access_key, sizeof(cfg->r2_secret_access_key), value);

    read_prompt("R2 对象前缀目录 [默认: " COL_RESET,
                value, sizeof(value), cfg->r2_prefix);
    copy_text(cfg->r2_prefix, sizeof(cfg->r2_prefix), value);

    cfg->r2_include_readme = prompt_bool("同步 README.MD 到 R2", cfg->r2_include_readme);

    normalize_github_repo_to_raw(cfg->github_repo, cfg->github_branch,
                                 cfg->raw_base_url, sizeof(cfg->raw_base_url));
    save_config(cfg, config_path);
    printf("\n  %s✓ 配置已保存至 %s%s\n", COL_GREEN, config_path, COL_RESET);
}

/* ──────────────────────────────────────────────────────────
   文件系统工具
   ────────────────────────────────────────────────────────── */

static int ensure_parent_dir(const char *path) {
    char temp[MAX_PATH];
    char *p;
    copy_text(temp, sizeof(temp), path);
    for (p = temp; *p; p++) { if (*p == '/') *p = '\\'; }
    p = strrchr(temp, '\\');
    if (!p) return 1;
    *p = '\0';
    if (temp[0] == '\0') return 1;
    for (p = temp + 1; *p; p++) {
        if (*p == '\\') { *p = '\0'; _mkdir(temp); *p = '\\'; }
    }
    _mkdir(temp);
    return 1;
}

static int file_exists(const char *path) { return _access(path, 0) == 0; }

static void set_cwd_to_exe_dir(void) {
    char path[MAX_PATH];
    char *slash;
    DWORD len = GetModuleFileNameA(NULL, path, sizeof(path));
    if (len == 0 || len >= sizeof(path)) return;
    slash = strrchr(path, '\\');
    if (slash) { *slash = '\0'; SetCurrentDirectoryA(path); }
}

static char *quote_arg(const char *arg, char *out, size_t out_size) {
    size_t j = 0;
    if (out_size == 0) return out;
    out[j++] = '"';
    while (*arg && j + 2 < out_size) {
        if (*arg == '"') out[j++] = '\\';
        out[j++] = *arg++;
    }
    if (j + 1 < out_size) out[j++] = '"';
    out[j] = '\0';
    return out;
}

static int run_command(const char *cmd) {
    int code = system(cmd);
    return (code == -1) ? -1 : code;
}

static int command_available(const char *exe_name) {
    char path[MAX_PATH];
    DWORD len = SearchPathA(NULL, exe_name, NULL, sizeof(path), path, NULL);
    return len > 0 && len < sizeof(path);
}

static void append_process_path(const char *dir) {
    char path[32767];
    char merged[32767];
    DWORD len = GetEnvironmentVariableA("PATH", path, sizeof(path));
    if (len == 0 || len >= sizeof(path)) return;
    snprintf(merged, sizeof(merged), "%s;%s", path, dir);
    SetEnvironmentVariableA("PATH", merged);
}

static void refresh_git_path_from_common_locations(void) {
    if (command_available("git.exe")) return;
    if (file_exists("C:\\Program Files\\Git\\cmd\\git.exe")) {
        append_process_path("C:\\Program Files\\Git\\cmd");
    } else if (file_exists("C:\\Program Files (x86)\\Git\\cmd\\git.exe")) {
        append_process_path("C:\\Program Files (x86)\\Git\\cmd");
    }
}

static void open_git_download_page(void) {
    printf("  %s正在打开 Git 官方下载页面...%s\n", COL_CYAN, COL_RESET);
    run_command("start \"\" \"https://git-scm.com/download/win\"");
}

static int install_git_with_winget(void) {
    int rc;
    if (!command_available("winget.exe")) {
        fprintf(stderr,
                "  %s未检测到 winget，无法自动安装 Git。将打开官网，请手动安装 Git for Windows。%s\n",
                COL_RED, COL_RESET);
        open_git_download_page();
        return 0;
    }
    printf("  %s正在通过 winget 安装 Git for Windows...%s\n", COL_CYAN, COL_RESET);
    rc = run_command("winget install --id Git.Git -e --source winget --accept-package-agreements --accept-source-agreements");
    if (rc != 0) {
        fprintf(stderr,
                "  %swinget 安装 Git 失败。将打开官网，请手动安装 Git for Windows。%s\n",
                COL_YELLOW, COL_RESET);
        open_git_download_page();
        return 0;
    }
    return 1;
}

static int ensure_git_available(int *push_available) {
    refresh_git_path_from_common_locations();
    if (command_available("git.exe")) {
        *push_available = 1;
        printf("  %s✓ Git 已就绪，运行环境检查通过%s\n", COL_GREEN, COL_RESET);
        return 1;
    }

    *push_available = 0;
    fprintf(stderr, "  %s未检测到 Git，GitHub 推送需要先安装 Git。%s\n", COL_YELLOW, COL_RESET);
    if (!prompt_bool("是否现在使用 winget 安装 Git for Windows", 0)) {
        fprintf(stderr, "  %s已取消安装，当前无法使用 GitHub 推送功能。%s\n",
                COL_RED, COL_RESET);
        return 1;
    }

    if (!install_git_with_winget()) {
        fprintf(stderr, "  %s当前无法使用 GitHub 推送功能。安装 Git 后请重新运行。%s\n",
                COL_RED, COL_RESET);
        return 1;
    }
    refresh_git_path_from_common_locations();
    if (command_available("git.exe")) {
        *push_available = 1;
        printf("  %s✓ Git 安装完成，运行环境检查通过%s\n", COL_GREEN, COL_RESET);
        return 1;
    }

    fprintf(stderr,
            "  %sGit 可能已安装，但当前终端尚未刷新 PATH。当前无法使用 GitHub 推送功能，请重新打开终端后再运行。%s\n",
            COL_YELLOW, COL_RESET);
    return 1;
}

static int ensure_runtime_environment(Config *cfg, int push_only) {
    int push_available = 0;
    print_section("🧰", "运行前环境检查");
    if (cfg->github_upload_enabled || push_only) {
        if (!ensure_git_available(&push_available)) return 0;
        if (!push_available) {
            cfg->github_upload_enabled = 0;
            if (push_only && !cfg->r2_upload_enabled) {
                fprintf(stderr, "  %s仅推送模式需要至少启用一个可用上传目标，已停止运行。%s\n", COL_RED, COL_RESET);
                return 0;
            }
        }
    } else {
        printf("  %s— GitHub 推送未启用，跳过 Git 检查%s\n", COL_DIM, COL_RESET);
    }
    if (cfg->r2_upload_enabled) {
        if (!command_available("curl.exe")) {
            fprintf(stderr, "  %s错误：R2 上传需要 curl.exe，请先安装或加入 PATH。%s\n", COL_RED, COL_RESET);
            return 0;
        }
        printf("  %s✓ curl 已就绪，R2 上传环境检查通过%s\n", COL_GREEN, COL_RESET);
    }
    return 1;
}

/* ──────────────────────────────────────────────────────────
   下载 IP 列表
   ────────────────────────────────────────────────────────── */

static int download_input_file(const Config *cfg) {
    char tmp[MAX_PATH];
    char q_url[1200], q_tmp[MAX_PATH + 8];
    char cmd[4096];
    int rc;

    if (strlen(cfg->input_file) + 10 >= sizeof(tmp)) {
        fprintf(stderr, "  %s输入文件路径过长：%s%s\n", COL_RED, cfg->input_file, COL_RESET);
        return 0;
    }
    snprintf(tmp, sizeof(tmp), "%s.download", cfg->input_file);
    ensure_parent_dir(cfg->input_file);
    quote_arg(cfg->input_url, q_url, sizeof(q_url));
    quote_arg(tmp, q_tmp, sizeof(q_tmp));
    snprintf(cmd, sizeof(cmd),
             "curl.exe -fSL --connect-timeout %d --max-time %d -A cf-ip-updater-c/%s -o %s %s",
             cfg->download_timeout, cfg->download_timeout, APP_VERSION, q_tmp, q_url);
    printf("  %s↓%s 下载 IP 列表：%s%s%s\n", COL_CYAN, COL_RESET, COL_WHITE, cfg->input_url, COL_RESET);
    rc = run_command(cmd);
    if (rc != 0 || !file_exists(tmp)) {
        DeleteFileA(tmp);
        printf("  %s⚠ 下载失败，将使用本地文件：%s%s\n", COL_YELLOW, cfg->input_file, COL_RESET);
        return 0;
    }
    DeleteFileA(cfg->input_file);
    if (!MoveFileA(tmp, cfg->input_file)) {
        DeleteFileA(tmp);
        printf("  %s⚠ 下载成功但无法替换文件：%s%s\n", COL_YELLOW, cfg->input_file, COL_RESET);
        return 0;
    }
    printf("  %s✓ 下载完成%s\n", COL_GREEN, COL_RESET);
    return 1;
}

/* ──────────────────────────────────────────────────────────
   动态数组 push
   ────────────────────────────────────────────────────────── */

static void node_array_push(NodeArray *arr, Node item) {
    if (arr->count == arr->capacity) {
        size_t next = arr->capacity ? arr->capacity * 2 : 4096;
        Node *items = (Node *)realloc(arr->items, next * sizeof(Node));
        if (!items) { fprintf(stderr, "内存不足（节点数组）\n"); exit(2); }
        arr->items = items; arr->capacity = next;
    }
    arr->items[arr->count++] = item;
}

static void tcp_array_push(TcpArray *arr, TcpResult item) {
    if (arr->count == arr->capacity) {
        size_t next = arr->capacity ? arr->capacity * 2 : 1024;
        TcpResult *items = (TcpResult *)realloc(arr->items, next * sizeof(TcpResult));
        if (!items) { fprintf(stderr, "内存不足（TCP 结果数组）\n"); exit(2); }
        arr->items = items; arr->capacity = next;
    }
    arr->items[arr->count++] = item;
}

static void speed_array_push(SpeedArray *arr, SpeedResult item) {
    if (arr->count == arr->capacity) {
        size_t next = arr->capacity ? arr->capacity * 2 : 1024;
        SpeedResult *items = (SpeedResult *)realloc(arr->items, next * sizeof(SpeedResult));
        if (!items) { fprintf(stderr, "内存不足（测速结果数组）\n"); exit(2); }
        arr->items = items; arr->capacity = next;
    }
    arr->items[arr->count++] = item;
}

static void wait_and_close_threads(HANDLE *threads, int count) {
    int i;
    for (i = 0; i < count; i++) {
        if (threads[i])
            WaitForSingleObject(threads[i], INFINITE);
    }
    for (i = 0; i < count; i++) {
        if (threads[i])
            CloseHandle(threads[i]);
    }
}

/* ──────────────────────────────────────────────────────────
   节点解析与去重
   ────────────────────────────────────────────────────────── */

static int parse_node_line(char *line, Node *node) {
    char *hash, *colon, *address, *region, *port_text;
    char normalized_region[128];
    int port;
    char *p = trim(line);
    if (*p == '\0' || *p == '#') return 0;
    hash = strchr(p, '#');
    if (!hash) return 0;
    *hash = '\0';
    address = trim(p);
    region  = trim(hash + 1);
    colon   = strrchr(address, ':');
    if (!colon || *region == '\0') return 0;
    *colon = '\0';
    port_text = trim(colon + 1);
    port = atoi(port_text);
    if (port < 1 || port > 65535) return 0;
    copy_text(node->ip,     sizeof(node->ip),     trim(address));
    strip_region_number(region, normalized_region, sizeof(normalized_region));
    copy_text(node->region, sizeof(node->region), normalized_region);
    node->port = port;
    return node->ip[0] != '\0';
}

static int cmp_node(const void *a, const void *b) {
    const Node *x = (const Node *)a, *y = (const Node *)b;
    int c = strcmp(x->ip, y->ip);
    if (c) return c;
    if (x->port != y->port) return x->port - y->port;
    return strcmp(x->region, y->region);
}

static void dedupe_nodes(NodeArray *arr) {
    size_t i, out = 0;
    if (arr->count == 0) return;
    qsort(arr->items, arr->count, sizeof(Node), cmp_node);
    for (i = 0; i < arr->count; i++) {
        if (out == 0 || cmp_node(&arr->items[i], &arr->items[out - 1]) != 0)
            arr->items[out++] = arr->items[i];
    }
    arr->count = out;
}

static int load_nodes(const Config *cfg, NodeArray *arr) {
    FILE *fp = fopen(cfg->input_file, "rb");
    char line[2048];
    Node node;
    if (!fp) {
        fprintf(stderr, "  %s错误：找不到输入文件：%s%s\n", COL_RED, cfg->input_file, COL_RESET);
        return 0;
    }
    while (fgets(line, sizeof(line), fp)) {
        if (parse_node_line(line, &node)) {
            node_array_push(arr, node);
            if (cfg->max_nodes > 0 && (int)arr->count >= cfg->max_nodes) break;
        }
    }
    fclose(fp);
    dedupe_nodes(arr);
    return arr->count > 0;
}

/* ──────────────────────────────────────────────────────────
   TCP 延迟探测
   ────────────────────────────────────────────────────────── */

static double tcp_probe(const Node *node, int timeout_ms) {
    SOCKET s = INVALID_SOCKET;
    struct sockaddr_storage ss;
    int family = 0, addr_len = 0;
    u_long nonblock = 1;
    ULONGLONG start;
    fd_set wfds, efds;
    struct timeval tv;
    int rc, so_error = 0, so_len = sizeof(so_error);

    memset(&ss, 0, sizeof(ss));
    if (inet_pton(AF_INET, node->ip, &((struct sockaddr_in *)&ss)->sin_addr) == 1) {
        struct sockaddr_in *a = (struct sockaddr_in *)&ss;
        family = AF_INET; a->sin_family = AF_INET;
        a->sin_port = htons((u_short)node->port);
        addr_len = sizeof(struct sockaddr_in);
    } else if (inet_pton(AF_INET6, node->ip, &((struct sockaddr_in6 *)&ss)->sin6_addr) == 1) {
        struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)&ss;
        family = AF_INET6; a6->sin6_family = AF_INET6;
        a6->sin6_port = htons((u_short)node->port);
        addr_len = sizeof(struct sockaddr_in6);
    } else { return -1.0; }

    s = socket(family, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return -1.0;
    ioctlsocket(s, FIONBIO, &nonblock);
    start = GetTickCount64();
    rc = connect(s, (struct sockaddr *)&ss, addr_len);
    if (rc == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK && err != WSAEINPROGRESS && err != WSAEINVAL) {
            closesocket(s); return -1.0;
        }
    }
    FD_ZERO(&wfds); FD_ZERO(&efds);
    FD_SET(s, &wfds); FD_SET(s, &efds);
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    rc = select(0, NULL, &wfds, &efds, &tv);
    if (rc <= 0) { closesocket(s); return -1.0; }
    getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&so_error, &so_len);
    closesocket(s);
    return so_error != 0 ? -1.0 : (double)(GetTickCount64() - start);
}

static DWORD WINAPI tcp_worker(LPVOID param) {
    TcpContext *ctx = (TcpContext *)param;
    for (;;) {
        size_t index;
        Node node;
        double latency;
        LONG done;

        EnterCriticalSection(&ctx->lock);
        if (ctx->next_index >= ctx->count) { LeaveCriticalSection(&ctx->lock); break; }
        index = ctx->next_index++;
        node  = ctx->nodes[index];
        LeaveCriticalSection(&ctx->lock);

        latency = tcp_probe(&node, ctx->config->tcp_timeout_ms);
        if (latency >= 0.0) {
            TcpResult result; result.node = node; result.latency_ms = latency;
            EnterCriticalSection(&ctx->lock);
            tcp_array_push(ctx->results, result);
            LeaveCriticalSection(&ctx->lock);
            if (ctx->config->verbose)
                printf("  [延迟] %s:%d#%s -> %.0f ms\n", node.ip, node.port, node.region, latency);
        }
        done = InterlockedIncrement(&ctx->done);
        if (done == (LONG)ctx->count || done % 500 == 0)
            render_progress(done, ctx->count, "TCP 延迟");
    }
    return 0;
}

static void run_tcp_tests(Config *cfg, NodeArray *nodes, TcpArray *results) {
    int worker_count = cfg->tcp_workers;
    HANDLE *threads;
    TcpContext ctx;
    int i;

    if ((size_t)worker_count > nodes->count) worker_count = (int)nodes->count;
    if (worker_count < 1) worker_count = 1;

    threads = (HANDLE *)calloc((size_t)worker_count, sizeof(HANDLE));
    memset(&ctx, 0, sizeof(ctx));
    ctx.nodes = nodes->items; ctx.count = nodes->count;
    ctx.results = results; ctx.config = cfg;
    InitializeCriticalSection(&ctx.lock);

    printf("  %s并发线程：%d%s\n", COL_DIM, worker_count, COL_RESET);
    for (i = 0; i < worker_count; i++) {
        threads[i] = CreateThread(NULL, 0, tcp_worker, &ctx, 0, NULL);
        if (!threads[i])
            fprintf(stderr, "  %s警告：TCP 线程创建失败：%d/%d%s\n",
                    COL_YELLOW, i + 1, worker_count, COL_RESET);
    }
    wait_and_close_threads(threads, worker_count);
    DeleteCriticalSection(&ctx.lock);
    free(threads);

    render_progress((LONG)nodes->count, nodes->count, "TCP 延迟");
    printf("\n  %s✓ 完成，连通节点：%zu / %zu%s\n",
           COL_GREEN, results->count, nodes->count, COL_RESET);
}

/* ──────────────────────────────────────────────────────────
   候选节点筛选（每区取前 N 个延迟最低）
   ────────────────────────────────────────────────────────── */

static int cmp_tcp_region_latency(const void *a, const void *b) {
    const TcpResult *x = (const TcpResult *)a, *y = (const TcpResult *)b;
    int c = strcmp(x->node.region, y->node.region);
    if (c) return c;
    if (x->latency_ms < y->latency_ms) return -1;
    if (x->latency_ms > y->latency_ms) return  1;
    return strcmp(x->node.ip, y->node.ip);
}

static TcpArray select_candidates(const TcpArray *tcp, int top_per_region) {
    TcpArray candidates = {0};
    size_t i; int rank = 0;
    char last_region[128] = "";
    TcpResult *copy;

    if (tcp->count == 0) return candidates;
    copy = (TcpResult *)malloc(tcp->count * sizeof(TcpResult));
    if (!copy) { fprintf(stderr, "内存不足（候选节点）\n"); exit(2); }
    memcpy(copy, tcp->items, tcp->count * sizeof(TcpResult));
    qsort(copy, tcp->count, sizeof(TcpResult), cmp_tcp_region_latency);

    for (i = 0; i < tcp->count; i++) {
        if (strcmp(last_region, copy[i].node.region) != 0) {
            copy_text(last_region, sizeof(last_region), copy[i].node.region);
            rank = 0;
        }
        if (rank < top_per_region) { tcp_array_push(&candidates, copy[i]); rank++; }
    }
    free(copy);
    return candidates;
}

/* ──────────────────────────────────────────────────────────
   下载测速
   ────────────────────────────────────────────────────────── */

static double parse_curl_speed(const char *text) {
    double size_bytes = 0.0, total_time = 0.0;
    if (sscanf(text, "%lf %lf", &size_bytes, &total_time) != 2) return 0.0;
    if (size_bytes <= 0.0 || total_time <= 0.0) return 0.0;
    return (size_bytes * 8.0) / (total_time * 1000000.0);
}

static double measure_speed_with_curl(const Node *node, const Config *cfg) {
    char resolve[512], url[512], cmd[4096];
    char q_resolve[600], q_url[600];
    FILE *pipe;
    char output[256] = "";

    snprintf(resolve, sizeof(resolve), "%s:%d:%s", SPEED_DOMAIN, node->port, node->ip);
    snprintf(url,     sizeof(url),     "https://%s:%d%s?bytes=%d",
             SPEED_DOMAIN, node->port, SPEED_PATH, SPEED_BYTES);
    quote_arg(resolve, q_resolve, sizeof(q_resolve));
    quote_arg(url,     q_url,     sizeof(q_url));
    snprintf(cmd, sizeof(cmd),
             "curl.exe -s -o NUL -w \"%%{size_download} %%{time_total}\""
             " --resolve %s --connect-timeout %d --max-time %d --insecure %s 2>NUL",
             q_resolve,
             cfg->speed_timeout_sec < 5 ? cfg->speed_timeout_sec : 5,
             cfg->speed_timeout_sec, q_url);
    pipe = _popen(cmd, "r");
    if (!pipe) return 0.0;
    fgets(output, sizeof(output), pipe);
    _pclose(pipe);
    return parse_curl_speed(output);
}

static DWORD WINAPI speed_worker(LPVOID param) {
    SpeedContext *ctx = (SpeedContext *)param;
    for (;;) {
        size_t index;
        TcpResult candidate;
        SpeedResult result;
        LONG done;

        EnterCriticalSection(&ctx->lock);
        if (ctx->next_index >= ctx->count) { LeaveCriticalSection(&ctx->lock); break; }
        index     = ctx->next_index++;
        candidate = ctx->items[index];
        LeaveCriticalSection(&ctx->lock);

        result.node      = candidate.node;
        result.latency_ms= candidate.latency_ms;
        result.speed_mbps= measure_speed_with_curl(&candidate.node, ctx->config);
        result.is_fast   = result.speed_mbps > ctx->config->min_speed_mbps;

        EnterCriticalSection(&ctx->lock);
        speed_array_push(ctx->results, result);
        LeaveCriticalSection(&ctx->lock);

        if (ctx->config->verbose)
            printf("  [测速] %s:%d#%s -> %.1f Mbps %s%s%s\n",
                   result.node.ip, result.node.port, result.node.region, result.speed_mbps,
                   result.is_fast ? COL_GREEN : COL_DIM,
                   result.is_fast ? "高速" : "普通", COL_RESET);

        done = InterlockedIncrement(&ctx->done);
        if (done == (LONG)ctx->count || done % 50 == 0)
            render_progress(done, ctx->count, "下载测速");
    }
    return 0;
}

static void run_speed_tests(Config *cfg, TcpArray *candidates, SpeedArray *results) {
    int worker_count = cfg->speed_workers;
    HANDLE *threads;
    SpeedContext ctx;
    int i;

    if (candidates->count == 0) return;
    if ((size_t)worker_count > candidates->count) worker_count = (int)candidates->count;
    if (worker_count < 1) worker_count = 1;

    threads = (HANDLE *)calloc((size_t)worker_count, sizeof(HANDLE));
    memset(&ctx, 0, sizeof(ctx));
    ctx.items = candidates->items; ctx.count = candidates->count;
    ctx.results = results; ctx.config = cfg;
    InitializeCriticalSection(&ctx.lock);

    printf("  %s并发线程：%d  |  高速阈值：> %.2f Mbps%s\n",
           COL_DIM, worker_count, cfg->min_speed_mbps, COL_RESET);
    for (i = 0; i < worker_count; i++) {
        threads[i] = CreateThread(NULL, 0, speed_worker, &ctx, 0, NULL);
        if (!threads[i])
            fprintf(stderr, "  %s警告：测速线程创建失败：%d/%d%s\n",
                    COL_YELLOW, i + 1, worker_count, COL_RESET);
    }
    wait_and_close_threads(threads, worker_count);
    DeleteCriticalSection(&ctx.lock);
    free(threads);

    render_progress((LONG)candidates->count, candidates->count, "下载测速");
    printf("\n  %s✓ 测速完成%s\n", COL_GREEN, COL_RESET);
}

/* ──────────────────────────────────────────────────────────
   结果排序 & 写入
   ────────────────────────────────────────────────────────── */

static int cmp_speed_output(const void *a, const void *b) {
    const SpeedResult *x = (const SpeedResult *)a, *y = (const SpeedResult *)b;
    int c = strcmp(x->node.region, y->node.region);
    if (c) return c;
    if (x->latency_ms < y->latency_ms) return -1;
    if (x->latency_ms > y->latency_ms) return  1;
    if (x->speed_mbps > y->speed_mbps) return -1;
    if (x->speed_mbps < y->speed_mbps) return  1;
    return strcmp(x->node.ip, y->node.ip);
}

static void format_output_region(const char *region, int numbered, int rank, char *out, size_t out_size) {
    if (numbered)
        snprintf(out, out_size, "%s_%d", region, rank);
    else
        copy_text(out, out_size, region);
}

static int write_results(const char *path, const SpeedArray *results, const Config *cfg, int only_fast) {
    FILE *fp;
    size_t i;
    char last_region[128] = "";
    int region_rank = 0;
    char output_region[160];
    char fast_label[128];
    ensure_parent_dir(path);
    fp = fopen(path, "wb");
    if (!fp) {
        fprintf(stderr, "  %s错误：无法写入 %s%s\n", COL_RED, path, COL_RESET);
        return 0;
    }
    for (i = 0; i < results->count; i++) {
        const SpeedResult *r = &results->items[i];
        if (only_fast && !r->is_fast) continue;
        if (strcmp(last_region, r->node.region) != 0) {
            copy_text(last_region, sizeof(last_region), r->node.region);
            region_rank = 0;
        }
        region_rank++;
        format_output_region(r->node.region, cfg->NO, region_rank, output_region, sizeof(output_region));
        fprintf(fp, "%s:%d#%s", r->node.ip, r->node.port, output_region);
        if (r->is_fast) {
            copy_text(fast_label, sizeof(fast_label), cfg->fast_label);
            rtrim_inplace(fast_label);
            fprintf(fp, " [%s]", fast_label);
        }
        if (cfg->show_latency || cfg->show_bandwidth) {
            fprintf(fp, " [");
            if (cfg->show_latency) fprintf(fp, "%.0fms", r->latency_ms);
            if (cfg->show_latency && cfg->show_bandwidth) fprintf(fp, " ");
            if (cfg->show_bandwidth) fprintf(fp, "%.0fMbps", r->speed_mbps);
            fprintf(fp, "]");
        }
        fprintf(fp, "\n");
    }
    fclose(fp);
    return 1;
}

/* ──────────────────────────────────────────────────────────
   README 生成
   ────────────────────────────────────────────────────────── */

static void now_text(char *out, size_t out_size) {
    time_t t = time(NULL);
    struct tm tmv;
    localtime_s(&tmv, &t);
    strftime(out, out_size, "%Y-%m-%d %H:%M:%S", &tmv);
}

static int write_readme(const Config *cfg) {
    FILE *fp;
    char stamp[64];
    now_text(stamp, sizeof(stamp));
    ensure_parent_dir(cfg->readme_file);
    fp = fopen(cfg->readme_file, "wb");
    if (!fp) {
        fprintf(stderr, "  %s警告：无法写入 README：%s%s\n", COL_YELLOW, cfg->readme_file, COL_RESET);
        return 0;
    }
    fprintf(fp, "# Cloudflare 优选 IP\n\n");
    fprintf(fp, "**测试地点**: %s  \n", cfg->test_location);
    fprintf(fp, "**更新频率**: %s  \n", cfg->update_frequency);
    fprintf(fp, "**本次更新**: %s\n\n", stamp);
    fprintf(fp, "## 订阅链接\n\n");
    fprintf(fp, "- 高速优选：`%s/%s`\n",   cfg->raw_base_url, cfg->github_best_path);
    fprintf(fp, "- 所有可用：`%s/%s`\n\n", cfg->raw_base_url, cfg->github_full_path);
    fprintf(fp, "## 使用方法\n\n");
    fprintf(fp, "1. 复制订阅链接。\n");
    fprintf(fp, "2. 打开 EdgeTunnel 后台。\n");
    fprintf(fp, "3. 创建优选订阅，选择\xe2\x80\x9c优选订阅模式 - 自定义\xe2\x80\x9d。\n");
    fprintf(fp, "4. 在订阅接口 / API / URL 中粘贴订阅链接。\n");
    fprintf(fp, "5. 验证可用性，选择\xe2\x80\x9c追加 API\xe2\x80\x9d。\n");
    fprintf(fp, "6. 保存并重新订阅。\n");
    fclose(fp);
    printf("  %s✓ README 已更新：%s%s\n", COL_GREEN, stamp, COL_RESET);
    return 1;
}

/* ──────────────────────────────────────────────────────────
   GitHub 推送
   ────────────────────────────────────────────────────────── */

static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void base64_encode(const unsigned char *src, size_t len, char *out, size_t out_size) {
    size_t i = 0, j = 0;
    while (i < len && j + 4 < out_size) {
        size_t remain   = len - i;
        unsigned int oa = src[i++];
        unsigned int ob = remain > 1 ? src[i++] : 0;
        unsigned int oc = remain > 2 ? src[i++] : 0;
        unsigned int t  = (oa << 16) | (ob << 8) | oc;
        out[j++] = b64_table[(t >> 18) & 0x3F];
        out[j++] = b64_table[(t >> 12) & 0x3F];
        out[j++] = remain > 1 ? b64_table[(t >>  6) & 0x3F] : '=';
        out[j++] = remain > 2 ? b64_table[ t        & 0x3F] : '=';
    }
    out[j] = '\0';
}

static void append_git_common(char *cmd, size_t size, const Config *cfg) {
    char q[2048];
    const char *token = cfg->github_token[0] ? cfg->github_token : getenv(cfg->github_token_env);
    if (cfg->git_http_proxy[0]) {
        char part[2600];
        snprintf(part, sizeof(part), " -c http.proxy=%s",
                 quote_arg(cfg->git_http_proxy, q, sizeof(q)));
        strncat(cmd, part, size - strlen(cmd) - 1);
    }
    if (cfg->git_https_proxy[0]) {
        char part[2600];
        snprintf(part, sizeof(part), " -c https.proxy=%s",
                 quote_arg(cfg->git_https_proxy, q, sizeof(q)));
        strncat(cmd, part, size - strlen(cmd) - 1);
    }
    if (token && *token) {
        char raw[1400], encoded[2048], header[2300], part[2600];
        snprintf(raw, sizeof(raw), "x-access-token:%s", token);
        base64_encode((const unsigned char *)raw, strlen(raw), encoded, sizeof(encoded));
        snprintf(header, sizeof(header),
                 "http.https://github.com/.extraheader=AUTHORIZATION: basic %s", encoded);
        snprintf(part, sizeof(part), " -c %s", quote_arg(header, q, sizeof(q)));
        strncat(cmd, part, size - strlen(cmd) - 1);
    }
}

static int git_command(const Config *cfg, const char *workdir, const char *args) {
    char cmd[8192] = "git";
    char q[MAX_PATH + 8];
    append_git_common(cmd, sizeof(cmd), cfg);
    if (workdir && *workdir) {
        strncat(cmd, " -C ", sizeof(cmd) - strlen(cmd) - 1);
        strncat(cmd, quote_arg(workdir, q, sizeof(q)), sizeof(cmd) - strlen(cmd) - 1);
    }
    strncat(cmd, " ", sizeof(cmd) - strlen(cmd) - 1);
    strncat(cmd, args, sizeof(cmd) - strlen(cmd) - 1);
    return run_command(cmd);
}

static void path_join(char *out, size_t size, const char *a, const char *b) {
    size_t n;
    copy_text(out, size, a);
    n = strlen(out);
    if (n > 0 && out[n-1] != '\\' && out[n-1] != '/')
        strncat(out, "\\", size - strlen(out) - 1);
    strncat(out, b, size - strlen(out) - 1);
}

static void append_url_path(char *url, size_t size, const char *path) {
    const char *p = path;
    if (!path || !*path) return;
    while (*p == '/' || *p == '\\') p++;
    if (url[0] && url[strlen(url) - 1] != '/')
        strncat(url, "/", size - strlen(url) - 1);
    while (*p) {
        char ch = (*p == '\\') ? '/' : *p;
        char part[2] = { ch, '\0' };
        strncat(url, part, size - strlen(url) - 1);
        p++;
    }
}

static void r2_object_key(const Config *cfg, const char *path, char *out, size_t out_size) {
    out[0] = '\0';
    append_url_path(out, out_size, cfg->r2_prefix);
    append_url_path(out, out_size, path);
}

static int copy_file_to_workdir(const char *src, const char *workdir, const char *dst_rel) {
    char dst[MAX_PATH];
    path_join(dst, sizeof(dst), workdir, dst_rel);
    ensure_parent_dir(dst);
    if (!CopyFileA(src, dst, FALSE)) {
        fprintf(stderr, "  %sGitHub 同步：无法复制 %s → %s%s\n",
                COL_RED, src, dst, COL_RESET);
        return 0;
    }
    return 1;
}

static int directory_has_entries(const char *path) {
    char pattern[MAX_PATH];
    WIN32_FIND_DATAA data;
    HANDLE h;
    path_join(pattern, sizeof(pattern), path, "*");
    h = FindFirstFileA(pattern, &data);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (strcmp(data.cFileName,".") != 0 && strcmp(data.cFileName,"..") != 0) {
            FindClose(h); return 1;
        }
    } while (FindNextFileA(h, &data));
    FindClose(h); return 0;
}

static int ensure_git_worktree(const Config *cfg) {
    char git_dir[MAX_PATH];
    char q_repo[1200], q_branch[256], q_workdir[MAX_PATH + 8], args[4096];
    path_join(git_dir, sizeof(git_dir), cfg->github_workdir, ".git");
    if (file_exists(git_dir)) {
        snprintf(args, sizeof(args), "fetch origin %s",
                 quote_arg(cfg->github_branch, q_branch, sizeof(q_branch)));
        if (git_command(cfg, cfg->github_workdir, args) != 0) return 0;
        if (git_command(cfg, cfg->github_workdir, "reset --hard") != 0) return 0;
        snprintf(args, sizeof(args), "checkout -B %s origin/%s",
                 quote_arg(cfg->github_branch, q_branch, sizeof(q_branch)),
                 cfg->github_branch);
        return git_command(cfg, cfg->github_workdir, args) == 0;
    }
    if (file_exists(cfg->github_workdir) && directory_has_entries(cfg->github_workdir)) {
        fprintf(stderr, "  %sGitHub 同步：工作目录非空且非 git 仓库：%s%s\n",
                COL_RED, cfg->github_workdir, COL_RESET);
        return 0;
    }
    ensure_parent_dir(cfg->github_workdir);
    _mkdir(cfg->github_workdir);
    snprintf(args, sizeof(args), "clone --branch %s --single-branch %s %s",
             quote_arg(cfg->github_branch, q_branch, sizeof(q_branch)),
             quote_arg(cfg->github_repo,   q_repo,   sizeof(q_repo)),
             quote_arg(cfg->github_workdir,q_workdir,sizeof(q_workdir)));
    return git_command(cfg, NULL, args) == 0;
}

static int github_sync(const Config *cfg) {
    char q_msg[700], q_full[MAX_PATH+8], q_best[MAX_PATH+8], q_readme[MAX_PATH+8], args[2048];
    int diff_rc, attempt;

    if (!cfg->github_upload_enabled) {
        printf("  %s— GitHub 推送已关闭（setting.config）%s\n", COL_DIM, COL_RESET);
        return 1;
    }
    if (!cfg->github_repo[0]) {
        fprintf(stderr, "  %s错误：github_repo 未配置%s\n", COL_RED, COL_RESET);
        return 0;
    }
    if (!cfg->github_token[0] && !getenv(cfg->github_token_env)) {
        printf("  %s⚠ 未配置 github_token，且环境变量 %s 不存在；将使用本机 git 凭据。%s\n",
               COL_YELLOW, cfg->github_token_env, COL_RESET);
    }
    if (!ensure_git_worktree(cfg)) return 0;

    if (!copy_file_to_workdir(cfg->full_output_file, cfg->github_workdir, cfg->github_full_path)) return 0;
    if (!copy_file_to_workdir(cfg->best_output_file, cfg->github_workdir, cfg->github_best_path)) return 0;
    snprintf(args, sizeof(args), "add %s %s",
             quote_arg(cfg->github_full_path, q_full, sizeof(q_full)),
             quote_arg(cfg->github_best_path, q_best, sizeof(q_best)));
    if (git_command(cfg, cfg->github_workdir, args) != 0) return 0;

    if (cfg->github_include_readme && file_exists(cfg->readme_file)) {
        if (!copy_file_to_workdir(cfg->readme_file, cfg->github_workdir, cfg->github_readme_path)) return 0;
        snprintf(args, sizeof(args), "add %s",
                 quote_arg(cfg->github_readme_path, q_readme, sizeof(q_readme)));
        if (git_command(cfg, cfg->github_workdir, args) != 0) return 0;
    }

    diff_rc = git_command(cfg, cfg->github_workdir, "diff --cached --quiet");
    if (diff_rc == 0) {
        printf("  %s— 结果文件无变化，跳过推送%s\n", COL_DIM, COL_RESET);
        return 1;
    }
    snprintf(args, sizeof(args),
             "-c user.name=\"IP Update Bot\" -c user.email=\"ip-update-bot@users.noreply.github.com\""
             " commit -m %s", quote_arg(cfg->github_message, q_msg, sizeof(q_msg)));
    if (git_command(cfg, cfg->github_workdir, args) != 0) return 0;

    for (attempt = 1; attempt <= cfg->github_push_retries; attempt++) {
        snprintf(args, sizeof(args), "push origin %s", cfg->github_branch);
        if (git_command(cfg, cfg->github_workdir, args) == 0) {
            printf("  %s✓ 已推送至 %s（%s）%s\n",
                   COL_GREEN, cfg->github_repo, cfg->github_branch, COL_RESET);
            return 1;
        }
        if (attempt < cfg->github_push_retries) {
            printf("  %s⚠ 推送失败，%d 秒后重试（%d/%d）...%s\n",
                   COL_YELLOW, cfg->github_retry_delay_sec,
                   attempt + 1, cfg->github_push_retries, COL_RESET);
            Sleep((DWORD)cfg->github_retry_delay_sec * 1000);
        }
    }
    fprintf(stderr, "  %s✗ GitHub 推送最终失败%s\n", COL_RED, COL_RESET);
    return 0;
}

/* ──────────────────────────────────────────────────────────
   Cloudflare R2 上传
   ────────────────────────────────────────────────────────── */

static void r2_endpoint_url(const Config *cfg, char *out, size_t out_size) {
    if (cfg->r2_endpoint[0]) {
        copy_text(out, out_size, cfg->r2_endpoint);
        return;
    }
    snprintf(out, out_size, "https://%s.r2.cloudflarestorage.com", cfg->r2_account_id);
}

static int r2_upload_file(const Config *cfg, const char *local_path, const char *object_path) {
    const char *access_key = cfg->r2_access_key_id[0] ? cfg->r2_access_key_id : getenv(cfg->r2_access_key_env);
    const char *secret_key = cfg->r2_secret_access_key[0] ? cfg->r2_secret_access_key : getenv(cfg->r2_secret_key_env);
    char endpoint[1200], object_key[1024], url[2600], userpwd[1800];
    char q_file[MAX_PATH + 8], q_url[2800], q_userpwd[2000], cmd[8192];
    int attempt;

    if (!access_key || !*access_key || !secret_key || !*secret_key) {
        fprintf(stderr, "  %s错误：R2 密钥未配置，请填写 r2_access_key_id/r2_secret_access_key 或设置环境变量。%s\n",
                COL_RED, COL_RESET);
        return 0;
    }
    if (!file_exists(local_path)) {
        fprintf(stderr, "  %s错误：R2 上传源文件不存在：%s%s\n", COL_RED, local_path, COL_RESET);
        return 0;
    }

    r2_endpoint_url(cfg, endpoint, sizeof(endpoint));
    r2_object_key(cfg, object_path, object_key, sizeof(object_key));
    copy_text(url, sizeof(url), endpoint);
    append_url_path(url, sizeof(url), cfg->r2_bucket);
    append_url_path(url, sizeof(url), object_key);
    snprintf(userpwd, sizeof(userpwd), "%s:%s", access_key, secret_key);

    quote_arg(local_path, q_file, sizeof(q_file));
    quote_arg(url, q_url, sizeof(q_url));
    quote_arg(userpwd, q_userpwd, sizeof(q_userpwd));

    for (attempt = 1; attempt <= cfg->r2_retries; attempt++) {
        snprintf(cmd, sizeof(cmd),
                 "curl.exe -fSL --connect-timeout %d --max-time %d"
                 " --retry 0 --aws-sigv4 \"aws:amz:auto:s3\" --user %s"
                 " -T %s %s",
                 cfg->r2_timeout_sec < 10 ? cfg->r2_timeout_sec : 10,
                 cfg->r2_timeout_sec, q_userpwd, q_file, q_url);
        if (run_command(cmd) == 0) {
            printf("  %s✓ R2 已上传：%s%s\n", COL_GREEN, object_key, COL_RESET);
            return 1;
        }
        if (attempt < cfg->r2_retries) {
            printf("  %s⚠ R2 上传失败，%d 秒后重试（%d/%d）...%s\n",
                   COL_YELLOW, cfg->r2_retry_delay_sec,
                   attempt + 1, cfg->r2_retries, COL_RESET);
            Sleep((DWORD)cfg->r2_retry_delay_sec * 1000);
        }
    }

    fprintf(stderr, "  %s✗ R2 上传失败：%s%s\n", COL_RED, object_key, COL_RESET);
    return 0;
}

static int r2_sync(const Config *cfg) {
    if (!cfg->r2_upload_enabled) {
        printf("  %s— R2 上传已关闭（setting.config）%s\n", COL_DIM, COL_RESET);
        return 1;
    }
    if (!cfg->r2_bucket[0]) {
        fprintf(stderr, "  %s错误：r2_bucket 未配置%s\n", COL_RED, COL_RESET);
        return 0;
    }
    if (!cfg->r2_endpoint[0] && !cfg->r2_account_id[0]) {
        fprintf(stderr, "  %s错误：r2_account_id 未配置；或填写 r2_endpoint 自定义端点%s\n", COL_RED, COL_RESET);
        return 0;
    }

    if (!r2_upload_file(cfg, cfg->full_output_file, cfg->r2_full_path)) return 0;
    if (!r2_upload_file(cfg, cfg->best_output_file, cfg->r2_best_path)) return 0;
    if (cfg->r2_include_readme && file_exists(cfg->readme_file)) {
        if (!r2_upload_file(cfg, cfg->readme_file, cfg->r2_readme_path)) return 0;
    }
    return 1;
}

/* ──────────────────────────────────────────────────────────
   帮助信息
   ────────────────────────────────────────────────────────── */

static void print_usage(void) {
    printf("%s用法：%s cf_updater.exe [选项]\n\n", COL_BOLD, COL_RESET);
    printf("  %s--config <文件>%s  指定配置文件路径（默认：setting.config）\n", COL_CYAN, COL_RESET);
    printf("  %s--setup%s          重新运行配置向导\n",                          COL_CYAN, COL_RESET);
    printf("  %s--NO [true|false]%s  启用或关闭优选序号输出\n",                  COL_CYAN, COL_RESET);
    printf("  %s--show-latency [true|false]%s  显示或隐藏输出中的延迟\n",        COL_CYAN, COL_RESET);
    printf("  %s--no-latency%s    隐藏输出中的延迟\n",                           COL_CYAN, COL_RESET);
    printf("  %s--show-bandwidth [true|false]%s  显示或隐藏输出中的带宽\n",       COL_CYAN, COL_RESET);
    printf("  %s--no-bandwidth%s   隐藏输出中的带宽\n",                           COL_CYAN, COL_RESET);
    printf("  %s--upload%s         本次运行强制开启 GitHub 推送\n",               COL_CYAN, COL_RESET);
    printf("  %s--no-upload%s      本次运行禁用 GitHub 推送\n",                   COL_CYAN, COL_RESET);
    printf("  %s--r2-upload%s      本次运行强制开启 R2 上传\n",                   COL_CYAN, COL_RESET);
    printf("  %s--no-r2-upload%s   本次运行禁用 R2 上传\n",                       COL_CYAN, COL_RESET);
    printf("  %s--push-only%s      仅更新 README 并推送现有结果\n",               COL_CYAN, COL_RESET);
    printf("  %s--help%s           显示本帮助\n",                                 COL_CYAN, COL_RESET);
}

/* ──────────────────────────────────────────────────────────
   主函数
   ────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    Config cfg;
    NodeArray  nodes        = {0};
    TcpArray   tcp_results  = {0};
    TcpArray   candidates   = {0};
    SpeedArray speed_results= {0};
    WSADATA wsa;
    const char *config_path = "setting.config";
    int push_only       = 0;
    int setup_requested = 0;
    int config_loaded   = 0;
    size_t fast_count   = 0;
    size_t i;
    time_t t_start, t_end;

    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    enable_virtual_terminal();
    set_cwd_to_exe_dir();

    print_banner();

    default_config(&cfg);
    apply_localized_defaults(&cfg);

    /* 第一遍：仅解析 --config */
    for (i = 1; i < (size_t)argc; i++) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < (size_t)argc)
            config_path = argv[++i];
    }

    config_loaded = load_config(&cfg, config_path);
    if (!config_loaded) {
        printf("  %s未找到配置文件，已生成默认配置：%s%s\n",
               COL_YELLOW, config_path, COL_RESET);
        save_config(&cfg, config_path);
    }

    /* 第二遍：解析其余选项（覆盖配置文件） */
    for (i = 1; i < (size_t)argc; i++) {
        if      (strcmp(argv[i], "--upload")    == 0) cfg.github_upload_enabled = 1;
        else if (strcmp(argv[i], "--no-upload") == 0) cfg.github_upload_enabled = 0;
        else if (strcmp(argv[i], "--r2-upload") == 0) cfg.r2_upload_enabled = 1;
        else if (strcmp(argv[i], "--no-r2-upload") == 0) cfg.r2_upload_enabled = 0;
        else if (strcmp(argv[i], "--NO")        == 0) {
            if (i + 1 < (size_t)argc && argv[i + 1][0] != '-')
                cfg.NO = parse_bool(argv[++i], cfg.NO);
            else
                cfg.NO = 1;
        }
        else if (strncmp(argv[i], "--NO=", 5)   == 0) cfg.NO = parse_bool(argv[i] + 5, cfg.NO);
        else if (strcmp(argv[i], "--no-NO")     == 0) cfg.NO = 0;
        else if (strcmp(argv[i], "--show-latency") == 0) {
            if (i + 1 < (size_t)argc && argv[i + 1][0] != '-')
                cfg.show_latency = parse_bool(argv[++i], cfg.show_latency);
            else
                cfg.show_latency = 1;
        }
        else if (strncmp(argv[i], "--show-latency=", 15) == 0) cfg.show_latency = parse_bool(argv[i] + 15, cfg.show_latency);
        else if (strcmp(argv[i], "--no-latency") == 0) cfg.show_latency = 0;
        else if (strcmp(argv[i], "--show-bandwidth") == 0) {
            if (i + 1 < (size_t)argc && argv[i + 1][0] != '-')
                cfg.show_bandwidth = parse_bool(argv[++i], cfg.show_bandwidth);
            else
                cfg.show_bandwidth = 1;
        }
        else if (strncmp(argv[i], "--show-bandwidth=", 17) == 0) cfg.show_bandwidth = parse_bool(argv[i] + 17, cfg.show_bandwidth);
        else if (strcmp(argv[i], "--no-bandwidth") == 0) cfg.show_bandwidth = 0;
        else if (strcmp(argv[i], "--push-only") == 0) push_only = 1;
        else if (strcmp(argv[i], "--setup")     == 0) setup_requested = 1;
        else if (strcmp(argv[i], "--help")      == 0 ||
                 strcmp(argv[i], "-h")          == 0) {
            print_usage();
            return 0;
        }
    }

    clamp_config(&cfg);

    /* 配置摘要 */
    print_section("📋", "运行配置");
    print_kv("配置文件：",    "%s", config_path);
    print_kv("TCP 并发：",    "%d", cfg.tcp_workers);
    print_kv("测速并发：",    "%d", cfg.speed_workers);
    print_kv("高速阈值：",    "%.1f Mbps", cfg.min_speed_mbps);
    print_kv("每区取前 N：",  "%d", cfg.top_per_region);
    print_kv("优选序号：",    "%s", cfg.NO ? "开启" : "关闭");
    print_kv("延迟显示：",    "%s", cfg.show_latency ? "开启" : "关闭");
    print_kv("带宽显示：",    "%s", cfg.show_bandwidth ? "开启" : "关闭");
    print_kv("GitHub 推送：", "%s", cfg.github_upload_enabled ? "启用" : "关闭");
    print_kv("R2 上传：",     "%s", cfg.r2_upload_enabled ? "启用" : "关闭");

    if (!config_loaded || setup_requested) {
        run_setup_wizard(&cfg, config_path, !config_loaded);
        clamp_config(&cfg);
    }

    if (!ensure_runtime_environment(&cfg, push_only)) {
        return 1;
    }

    if (push_only) {
        print_section("☁", "仅推送模式");
        if (cfg.update_readme) write_readme(&cfg);
        if (!github_sync(&cfg)) return 1;
        if (!r2_sync(&cfg)) return 1;
        return 0;
    }

    time(&t_start);

    /* ── 下载输入文件 ── */
    print_section("⬇", "阶段 0 / 3  ·  下载 IP 列表");
    if (cfg.download_input) download_input_file(&cfg);

    /* ── 加载节点 ── */
    print_section("📂", "阶段 1 / 3  ·  加载节点");
    if (!load_nodes(&cfg, &nodes)) return 1;
    printf("  %s✓ 共读取 %zu 个唯一节点（来源：%s）%s\n",
           COL_GREEN, nodes.count, cfg.input_file, COL_RESET);

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "  %s错误：WSAStartup 初始化失败%s\n", COL_RED, COL_RESET);
        return 1;
    }

    /* ── TCP 延迟测试 ── */
    print_section("🔌", "阶段 2 / 3  ·  TCP 延迟测试");
    run_tcp_tests(&cfg, &nodes, &tcp_results);
    candidates = select_candidates(&tcp_results, cfg.top_per_region);
    printf("  %s→ 进入测速候选：%zu 个%s\n", COL_CYAN, candidates.count, COL_RESET);

    /* ── 下载测速 ── */
    print_section("⚡", "阶段 3 / 3  ·  下载测速");
    run_speed_tests(&cfg, &candidates, &speed_results);

    WSACleanup();

    if (speed_results.count > 0)
        qsort(speed_results.items, speed_results.count, sizeof(SpeedResult), cmp_speed_output);
    for (i = 0; i < speed_results.count; i++)
        if (speed_results.items[i].is_fast) fast_count++;

    /* ── 写入结果 ── */
    print_section("💾", "写入结果");
    write_results(cfg.full_output_file, &speed_results, &cfg, 0);
    printf("  %s✓ 完整结果 → %s%s\n", COL_GREEN, cfg.full_output_file, COL_RESET);
    write_results(cfg.best_output_file, &speed_results, &cfg, 1);
    printf("  %s✓ 高速优选 → %s%s\n", COL_GREEN, cfg.best_output_file, COL_RESET);
    if (cfg.update_readme) write_readme(&cfg);

    /* ── 上传 ── */
    print_section("☁", "上传同步");
    if (!github_sync(&cfg)) return 1;
    if (!r2_sync(&cfg)) return 1;

    /* ── 最终汇总 ── */
    time(&t_end);
    print_section("✅", "运行完成");
    printf("\n");
    print_kv("耗时：",      "%ld 秒", (long)(t_end - t_start));
    print_kv("输入节点：",  "%zu",    nodes.count);
    print_kv("TCP 可达：",  "%zu",    tcp_results.count);
    print_kv("已测速：",    "%zu",    speed_results.count);
    print_kv("高速节点：",  "%s%zu%s", COL_GREEN, fast_count, COL_RESET);
    print_kv("完整输出：",  "%s",     cfg.full_output_file);
    print_kv("优选输出：",  "%s",     cfg.best_output_file);
    printf("\n");
    print_divider(COL_CYAN);
    printf("\n按任意键退出");
    getchar();

    free(nodes.items);
    free(tcp_results.items);
    free(candidates.items);
    free(speed_results.items);
    return 0;
}
