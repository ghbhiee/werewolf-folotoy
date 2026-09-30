#!/usr/bin/env bash
# 主机侧离屏渲染:用设备上同一份 LVGL 9.5 + main/ww_ui.c 把主持屏各界面画成 PNG,
# 顺便打印每屏 LVGL 堆占用。改自 ~/cc/ai-passport-base/toolkit/sim。
#
#   ./hostsim/lvgl/build.sh        # 出 hostsim/lvgl/out/*.png
#
# 前置:仓库至少 idf.py build 过一次(要 managed_components/ 里的 LVGL 源码)。
set -euo pipefail
HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd -- "${HERE}/../.." && pwd)"
LVGL="${REPO}/managed_components/lvgl__lvgl"
MAIN="${REPO}/main"
[[ -d "${LVGL}" ]] || { echo "找不到 ${LVGL},先 idf.py build 一次" >&2; exit 1; }
mkdir -p "${HERE}/out"
rm -f "${HERE}"/out/*.ppm "${HERE}"/out/*.png
find "${LVGL}/src" -name '*.c' -not -path '*/drivers/*' > "${HERE}/out/lvgl_srcs.txt"
cc -std=gnu11 -O1 -w -DLV_CONF_INCLUDE_SIMPLE \
   -I"${HERE}" -I"${HERE}/fake" -I"${LVGL}" -I"${MAIN}" -I"${REPO}/components/bsp/include" \
   "${HERE}/harness.c" "${MAIN}/ui_pixel.c" "${MAIN}/ui_pixel_math.c" \
   "${MAIN}/ww_core.c" "${MAIN}/ww_host.c" "${MAIN}/font_pixel_12.c" "${MAIN}/font_pixel_24.c" \
   $(cat "${HERE}/out/lvgl_srcs.txt") \
   -lm -o "${HERE}/out/sim"
cd "${HERE}"
ZSIM_OUT="${HERE}/out" ./out/sim
python3 - "${HERE}/out" <<'PY'
import glob, os, sys
out = sys.argv[1]
try:
    from PIL import Image
except ImportError:
    print("(没装 Pillow,只留下 PPM)"); sys.exit(0)
files = sorted(glob.glob(os.path.join(out, "*.ppm")))
for f in files:
    Image.open(f).save(f[:-4] + ".png")
    os.remove(f)
# 拼一张总览图,方便一眼看完
imgs = [Image.open(f[:-4] + ".png") for f in files]
if imgs:
    cols = 6
    rows = (len(imgs) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * 250, rows * 330), (40, 40, 40))
    for i, im in enumerate(imgs):
        sheet.paste(im, ((i % cols) * 250 + 5, (i // cols) * 330 + 5))
    sheet.save(os.path.join(out, "_overview.png"))
print(f"PNG 已生成于 {out}")
PY
