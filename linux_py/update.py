import argparse
import asyncio
import heapq
import os
import shutil
import subprocess  # nosec
import sys
import time
import urllib.error

import requests
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence

from tqdm import tqdm


DEFAULT_INPUT_FILE = Path("ips.txt")
DEFAULT_INPUT_URL = "https://zip.cm.edu.kg/all.txt"
#DEFAULT_INPUT_URL = "https://countrymerge.pages.dev/all.txt"
DEFAULT_INPUT_DOWNLOAD_TIMEOUT = 30.0
DEFAULT_BEST_OUTPUT_FILE = Path("best_ips.txt")
DEFAULT_FULL_OUTPUT_FILE = Path("full_ips.txt")

DEFAULT_TCP_TIMEOUT = 1.5
DEFAULT_TCP_WORKERS = 200

DEFAULT_SPEED_TIMEOUT = 6.0
DEFAULT_SPEED_PROCESS_BUFFER = 8.0
DEFAULT_SPEED_WORKERS = 5
DEFAULT_MIN_SPEED_MBPS = 16
DEFAULT_TOP_PER_REGION = 10

SPEED_DOMAIN = "speed.cloudflare.com"
SPEED_PATH = "/__down"
SPEED_BYTES = 2 * 1024 * 1024
FAST_LABEL = "高速 by Jz "
MY_REGION = "KG2"
MY_SUPPLEMENT_TRIGGER_COUNT = 2
MY_SUPPLEMENT_LIMIT = 2


if sys.platform == "win32":
    asyncio.set_event_loop_policy(asyncio.WindowsSelectorEventLoopPolicy())


def parse_bool(value: str) -> bool:
    text = value.strip().lower()
    if text in {"1", "true", "yes", "on"}:
        return True
    if text in {"0", "false", "no", "off"}:
        return False
    raise ValueError(f"invalid boolean value: {value}")


def strip_region_number(region: str) -> str:
    base, sep, suffix = region.rpartition("_")
    if sep and base and suffix.isdigit():
        return base
    return region


@dataclass(frozen=True)
class AppConfig:
    input_file: Path
    full_output_file: Path
    best_output_file: Path
    tcp_timeout: float
    tcp_workers: int
    speed_timeout: float
    speed_process_buffer: float
    speed_workers: int
    min_speed_mbps: float
    top_per_region: int
    verbose: bool
    numbered_regions: bool
    show_latency: bool   # whether to include latency in output labels
    show_mbps: bool      # whether to include download speed in output labels


@dataclass(frozen=True)
class Node:
    ip: str
    port: int
    region: str

    @property
    def raw(self) -> str:
        return f"{self.ip}:{self.port}#{self.region}"


@dataclass(frozen=True)
class TcpResult:
    node: Node
    latency_ms: float


@dataclass(frozen=True)
class SpeedResult:
    node: Node
    latency_ms: float
    speed_mbps: float
    is_fast: bool


def parse_args() -> AppConfig:
    parser = argparse.ArgumentParser(description="Filter IPs by TCP latency and download speed.")
    parser.add_argument("-i", "--input", type=Path, default=DEFAULT_INPUT_FILE, help="input file")
    parser.add_argument("-o", "--output", type=Path, default=DEFAULT_FULL_OUTPUT_FILE, help="full output file")
    parser.add_argument("--best-output", type=Path, default=DEFAULT_BEST_OUTPUT_FILE, help="fast IP output file")
    parser.add_argument("--tcp-timeout", type=float, default=DEFAULT_TCP_TIMEOUT, help="TCP timeout in seconds")
    parser.add_argument("--tcp-workers", type=int, default=DEFAULT_TCP_WORKERS, help="TCP test concurrency")
    parser.add_argument("--speed-timeout", type=float, default=DEFAULT_SPEED_TIMEOUT, help="speed timeout in seconds")
    parser.add_argument(
        "--speed-process-buffer",
        type=float,
        default=DEFAULT_SPEED_PROCESS_BUFFER,
        help="extra seconds before killing a stuck curl process",
    )
    parser.add_argument("--speed-workers", type=int, default=DEFAULT_SPEED_WORKERS, help="speed test concurrency")
    parser.add_argument("--min-speed", type=float, default=DEFAULT_MIN_SPEED_MBPS, help="minimum fast speed in Mbps")
    parser.add_argument("--top", type=int, default=DEFAULT_TOP_PER_REGION, help="latency candidates kept per region")
    parser.add_argument("--verbose", action="store_true", help="print each successful test result")
    parser.add_argument(
        "--NO",
        nargs="?",
        const="true",
        default=os.environ.get("NO", "false"),
        help="number output region labels, for example #HK_1",
    )
    # ── label display options ──────────────────────────────────────────────────
    parser.add_argument(
        "--show-latency",
        nargs="?",
        const="true",
        default=os.environ.get("SHOW_LATENCY", "true"),
        metavar="BOOL",
        help="include latency (ms) in output labels (default: true; env: SHOW_LATENCY)",
    )
    parser.add_argument(
        "--show-mbps",
        nargs="?",
        const="true",
        default=os.environ.get("SHOW_MBPS", "false"),
        metavar="BOOL",
        help="include download speed (Mbps) in output labels (default: false; env: SHOW_MBPS)",
    )
    # ──────────────────────────────────────────────────────────────────────────
    args = parser.parse_args()

    try:
        numbered_regions = parse_bool(args.NO)
    except ValueError as exc:
        parser.error(str(exc))

    try:
        show_latency = parse_bool(args.show_latency) if args.show_latency is not None else True
    except ValueError as exc:
        parser.error(f"--show-latency: {exc}")

    try:
        show_mbps = parse_bool(args.show_mbps) if args.show_mbps is not None else False
    except ValueError as exc:
        parser.error(f"--show-mbps: {exc}")

    return AppConfig(
        input_file=args.input,
        full_output_file=args.output,
        best_output_file=args.best_output,
        tcp_timeout=args.tcp_timeout,
        tcp_workers=args.tcp_workers,
        speed_timeout=args.speed_timeout,
        speed_process_buffer=args.speed_process_buffer,
        speed_workers=args.speed_workers,
        min_speed_mbps=args.min_speed,
        top_per_region=args.top,
        verbose=args.verbose,
        numbered_regions=numbered_regions,
        show_latency=show_latency,
        show_mbps=show_mbps,
    )


def parse_node(line: str) -> Node | None:
    text = line.strip()
    if not text or text.startswith("#") or "#" not in text:
        return None

    address, region = (part.strip() for part in text.split("#", 1))
    if not address or not region or ":" not in address:
        return None

    ip, port_text = (part.strip() for part in address.rsplit(":", 1))
    try:
        port = int(port_text)
    except ValueError:
        return None

    if not ip or not 1 <= port <= 65535:
        return None
    return Node(ip=ip, port=port, region=strip_region_number(region))


def load_nodes(path: Path) -> list[Node]:
    if not path.exists():
        raise FileNotFoundError(f"input file not found: {path}")

    nodes: list[Node] = []
    seen: set[Node] = set()
    with path.open("r", encoding="utf-8-sig") as file:
        for line in file:
            node = parse_node(line)
            if node is None or node in seen:
                continue
            seen.add(node)
            nodes.append(node)
    return nodes


def refresh_input_file(url: str, path: Path, timeout: float) -> bool:
    temp_path = path.with_name(f"{path.name}.download")
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        if not url.lower().startswith("https://"):
            return False
        with requests.get(url, headers={"User-Agent": "cf-ip-updater/1.0"}, timeout=timeout, stream=True) as response:
            if response.status_code != 200:
                raise RuntimeError(f"HTTP {response.status_code}")
            with temp_path.open("wb") as file:
                response.raw.decode_content = True
                shutil.copyfileobj(response.raw, file)

        if temp_path.stat().st_size == 0:
            raise RuntimeError("downloaded file is empty")

        temp_path.replace(path)
        print(f"Downloaded input file from {url} to {path}")
        return True
    except (OSError, RuntimeError, http.client.HTTPException) as exc:
        if temp_path.exists():
            try:
                temp_path.unlink()
            except OSError:
                pass
        print(f"Input download failed: {exc}; using local {path}")
        return False


def positive_worker_count(requested: int, item_count: int) -> int:
    return max(1, min(max(1, requested), max(1, item_count)))


async def tcping(node: Node, timeout: float) -> float | None:
    start = time.perf_counter()
    writer: asyncio.StreamWriter | None = None
    try:
        _, writer = await asyncio.wait_for(asyncio.open_connection(node.ip, node.port), timeout=timeout)
        return round((time.perf_counter() - start) * 1000, 2)
    except (OSError, TimeoutError, asyncio.TimeoutError):
        return None
    finally:
        if writer is not None:
            writer.close()
            try:
                await writer.wait_closed()
            except (OSError, TimeoutError, asyncio.TimeoutError):
                pass


async def run_tcp_tests(nodes: Sequence[Node], *, timeout: float, workers: int, verbose: bool) -> list[TcpResult]:
    queue: asyncio.Queue[Node | None] = asyncio.Queue()
    results: list[TcpResult] = []
    progress = tqdm(total=len(nodes), desc="TCP latency", unit="ip")

    async def worker() -> None:
        while True:
            node = await queue.get()
            try:
                if node is None:
                    return
                latency = await tcping(node, timeout)
                if latency is not None:
                    results.append(TcpResult(node=node, latency_ms=latency))
                    if verbose:
                        tqdm.write(f"[LAT] {node.raw} -> {latency} ms")
                progress.update(1)
            finally:
                queue.task_done()

    tasks = [asyncio.create_task(worker()) for _ in range(positive_worker_count(workers, len(nodes)))]
    for node in nodes:
        queue.put_nowait(node)
    for _ in tasks:
        queue.put_nowait(None)

    await queue.join()
    await asyncio.gather(*tasks)
    progress.close()
    return results


def select_candidates(results: Iterable[TcpResult], top_per_region: int) -> list[TcpResult]:
    groups: dict[str, list[tuple[float, int, TcpResult]]] = defaultdict(list)
    limit = max(1, top_per_region)

    for index, result in enumerate(results):
        heap = groups[result.node.region]
        item = (-result.latency_ms, -index, result)
        if len(heap) < limit:
            heapq.heappush(heap, item)
        else:
            heapq.heappushpop(heap, item)

    candidates = [item[2] for region in sorted(groups) for item in groups[region]]
    candidates.sort(key=lambda item: (item.node.region, item.latency_ms))
    return candidates


def get_curl_command() -> str | None:
    if sys.platform == "win32":
        return shutil.which("curl.exe") or shutil.which("curl")
    return shutil.which("curl")


def measure_speed_with_curl(node: Node, timeout: float, process_buffer: float) -> float:
    curl = get_curl_command()
    if curl is None:
        return 0.0

    try:
        import ipaddress
        ipaddress.ip_address(node.ip)
    except ValueError:
        return 0.0

    url = f"https://{SPEED_DOMAIN}:{node.port}{SPEED_PATH}?bytes={SPEED_BYTES}"
    cmd = [
        curl,
        "-s",
        "-o",
        "NUL" if sys.platform == "win32" else "/dev/null",
        "-w",
        "%{size_download} %{time_total}",
        "--resolve",
        f"{SPEED_DOMAIN}:{node.port}:{node.ip}",
        "--connect-timeout",
        str(min(5.0, timeout)),
        "--max-time",
        str(timeout),
        "--insecure",
        url,
    ]

    try:
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=timeout + process_buffer,
            creationflags=subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0,
        )
        if result.returncode != 0:
            return 0.0
        return parse_curl_speed(result.stdout)
    except (OSError, subprocess.TimeoutExpired):
        return 0.0


def parse_curl_speed(stdout: str) -> float:
    try:
        size_text, time_text, *_ = stdout.strip().split()
        size_bytes = float(size_text)
        time_total = float(time_text)
    except ValueError:
        return 0.0

    if size_bytes <= 0 or time_total <= 0:
        return 0.0
    return round((size_bytes * 8) / (time_total * 1_000_000), 2)


async def run_speed_tests(
    candidates: Sequence[TcpResult],
    *,
    timeout: float,
    process_buffer: float,
    workers: int,
    min_speed: float,
    verbose: bool,
) -> list[SpeedResult]:
    queue: asyncio.Queue[TcpResult | None] = asyncio.Queue()
    results: list[SpeedResult] = []
    progress = tqdm(total=len(candidates), desc="Download speed", unit="ip")

    async def worker() -> None:
        while True:
            candidate = await queue.get()
            try:
                if candidate is None:
                    return
                speed = await asyncio.to_thread(measure_speed_with_curl, candidate.node, timeout, process_buffer)
                result = SpeedResult(
                    node=candidate.node,
                    latency_ms=candidate.latency_ms,
                    speed_mbps=speed,
                    is_fast=speed > min_speed,
                )
                results.append(result)
                if verbose:
                    status = "FAST" if result.is_fast else "NORMAL"
                    tqdm.write(f"[SPEED] {candidate.node.raw} -> {speed} Mbps {status}")
                progress.update(1)
            finally:
                queue.task_done()

    tasks = [asyncio.create_task(worker()) for _ in range(positive_worker_count(workers, len(candidates)))]
    for candidate in candidates:
        queue.put_nowait(candidate)
    for _ in tasks:
        queue.put_nowait(None)

    await queue.join()
    await asyncio.gather(*tasks)
    progress.close()

    results.sort(key=lambda item: (item.node.region, item.latency_ms, -item.speed_mbps))
    return results


def build_label(result: SpeedResult, *, show_latency: bool, show_mbps: bool) -> str:
    """Build the bracket annotation appended to each output line.

    Examples (with various flag combinations):
      show_latency=True,  show_mbps=False  →  [高速 by Jz 12ms]
      show_latency=False, show_mbps=True   →  [高速 by Jz 123.45Mbps]
      show_latency=True,  show_mbps=True   →  [高速 by Jz 12ms | 123.45Mbps]
      show_latency=False, show_mbps=False  →  [高速 by Jz]          (fast only)
                                           →  []                      (not fast, no info)
    """
    parts: list[str] = []
    fast_prefix = FAST_LABEL if result.is_fast else ""

    if show_latency:
        parts.append(f"{format_latency_ms(result.latency_ms)}ms")
    if show_mbps:
        parts.append(f"{result.speed_mbps:.0f}M")

    inner = " | ".join(parts) if parts else ""

    if fast_prefix and inner:
        return f"[{fast_prefix}{inner}]"
    if fast_prefix:
        return f"[{fast_prefix.rstrip()}]"
    if inner:
        return f"[{inner}]"
    return ""


def write_results(
    path: Path,
    results: Iterable[SpeedResult],
    numbered_regions: bool,
    *,
    show_latency: bool = True,
    show_mbps: bool = False,
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as file:
        region_counts: dict[str, int] = defaultdict(int)
        for result in results:
            region_counts[result.node.region] += 1
            region = (
                f"{result.node.region}_{region_counts[result.node.region]}"
                if numbered_regions
                else result.node.region
            )
            label = build_label(result, show_latency=show_latency, show_mbps=show_mbps)
            suffix = f" {label}" if label else ""
            file.write(f"{result.node.ip}:{result.node.port}#{region}{suffix}\n")


def format_latency_ms(latency_ms: float) -> str:
    return str(max(0, int(round(latency_ms))))


def filter_fast_results(results: Iterable[SpeedResult]) -> list[SpeedResult]:
    return [result for result in results if result.is_fast]


def is_region(node: Node, region: str) -> bool:
    return node.region.upper() == region.upper()


def node_key(node: Node) -> tuple[str, int, str]:
    return (node.ip, node.port, node.region.upper())


async def supplement_my_results(
    best_results: Sequence[SpeedResult],
    tcp_results: Sequence[TcpResult],
    config: AppConfig,
) -> list[SpeedResult]:
    results = list(best_results)
    my_count = sum(1 for result in results if is_region(result.node, MY_REGION))
    if my_count > MY_SUPPLEMENT_TRIGGER_COUNT:
        return results

    my_candidates = [result for result in tcp_results if is_region(result.node, MY_REGION)]
    if not my_candidates:
        print("MY supplement skipped: no TCP reachable MY nodes")
        return results

    print(
        f"MY nodes in best output: {my_count}; "
        f"retesting all {len(my_candidates)} TCP reachable MY node(s)"
    )
    tested_my_results = await run_speed_tests(
        my_candidates,
        timeout=config.speed_timeout,
        process_buffer=config.speed_process_buffer,
        workers=config.speed_workers,
        min_speed=config.min_speed_mbps,
        verbose=config.verbose,
    )

    existing_nodes = {node_key(result.node) for result in results}
    additions = [
        result
        for result in tested_my_results
        if node_key(result.node) not in existing_nodes and result.speed_mbps > 0
    ]
    additions.sort(key=lambda item: (-item.speed_mbps, item.latency_ms, item.node.ip, item.node.port))
    selected = additions[:MY_SUPPLEMENT_LIMIT]
    if selected:
        results.extend(selected)
        results.sort(key=lambda item: (item.node.region, item.latency_ms, -item.speed_mbps))
    print(f"MY supplement added: {len(selected)}")
    return results


async def run(config: AppConfig) -> int:
    if config.full_output_file.resolve() == config.best_output_file.resolve():
        print("ERROR: --output and --best-output must point to different files")
        return 1

    refresh_input_file(DEFAULT_INPUT_URL, config.input_file, DEFAULT_INPUT_DOWNLOAD_TIMEOUT)

    try:
        nodes = load_nodes(config.input_file)
    except FileNotFoundError as exc:
        print(f"ERROR: {exc}")
        return 1

    if not nodes:
        print(f"ERROR: no valid nodes found in {config.input_file}")
        return 1

    print(f"Loaded {len(nodes)} unique nodes from {config.input_file}")
    print(f"Stage 1/2: TCP latency test, concurrency={config.tcp_workers}")
    tcp_results = await run_tcp_tests(
        nodes,
        timeout=config.tcp_timeout,
        workers=config.tcp_workers,
        verbose=config.verbose,
    )

    candidates = select_candidates(tcp_results, config.top_per_region)
    print(f"TCP reachable: {len(tcp_results)}; speed candidates: {len(candidates)}")

    if candidates:
        print(
            "Stage 2/2: download speed test, "
            f"concurrency={config.speed_workers}, fast tag > {config.min_speed_mbps} Mbps"
        )
        speed_results = await run_speed_tests(
            candidates,
            timeout=config.speed_timeout,
            process_buffer=config.speed_process_buffer,
            workers=config.speed_workers,
            min_speed=config.min_speed_mbps,
            verbose=config.verbose,
        )
    else:
        speed_results = []

    best_results = await supplement_my_results(filter_fast_results(speed_results), tcp_results, config)
    write_results(
        config.full_output_file,
        speed_results,
        config.numbered_regions,
        show_latency=config.show_latency,
        show_mbps=config.show_mbps,
    )
    write_results(
        config.best_output_file,
        best_results,
        config.numbered_regions,
        show_latency=config.show_latency,
        show_mbps=config.show_mbps,
    )
    print_summary(config, len(nodes), len(tcp_results), len(speed_results), len(best_results))
    return 0


def print_summary(
    config: AppConfig,
    input_count: int,
    tcp_count: int,
    speed_count: int,
    fast_count: int,
) -> None:
    print("Done")
    print(f"Input nodes: {input_count}")
    print(f"TCP reachable: {tcp_count}")
    print(f"Speed tested: {speed_count}")
    print(f"Fast tagged: {fast_count}")
    print(f"Full output: {config.full_output_file}")
    print(f"Best output: {config.best_output_file}")
    print(f"Label: latency={'on' if config.show_latency else 'off'}, mbps={'on' if config.show_mbps else 'off'}")


def main() -> int:
    return asyncio.run(run(parse_args()))


if __name__ == "__main__":
    raise SystemExit(main())
