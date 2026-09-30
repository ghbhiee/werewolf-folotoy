#!/usr/bin/env bash
# 编译主机仿真服务器(和固件共用 main/ww_core.c / ww_host.c / ww_api.c)。
#   ./hostsim/build.sh && ./hostsim/out/server 8080
set -euo pipefail
HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd -- "${HERE}/.." && pwd)"
mkdir -p "${HERE}/out"
"${CC:-cc}" -std=gnu11 -Wall -Wextra -Werror -O1 -g -I"${REPO}/main" \
    "${HERE}/server.c" "${REPO}/main/ww_core.c" "${REPO}/main/ww_host.c" "${REPO}/main/ww_api.c" \
    -o "${HERE}/out/server"
echo "built ${HERE}/out/server  (运行时在仓库根目录下执行,或设 WW_ROOT=仓库路径)"
