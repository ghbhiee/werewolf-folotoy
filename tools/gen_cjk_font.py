#!/usr/bin/env python3
"""为 FoloToy AI Passport 的玩法生成「按用字裁剪」的中文点阵字体。

板子 8 MB Flash、无 PSRAM,塞不下完整 CJK 字库。本脚本扫描项目 main/ 下所有
C 源码里的**字符串字面量**(注释会被剥掉,所以中文注释不会被算进去),只把真正
会显示出来的字交给 lv_font_conv 生成 LVGL 字体。

    python3 toolkit/gen_cjk_font.py --repo ~/cc/my-play

产物:<repo>/main/font_pixel_12.c 与 font_pixel_24.c,变量名同文件名。
在代码里这样用:

    LV_FONT_DECLARE(font_pixel_12);
    lv_obj_set_style_text_font(label, &font_pixel_12, 0);

改动任何界面文案后都要重跑。源字库缺字时脚本直接报错退出,不会生成一份会在
设备上显示成空方框的字体 —— 这个检查是有来历的,见 HANDOFF.md「已知的坑」。

依赖:Node.js(npx 会自动拉 lv_font_conv)、Python 的 fonttools。
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
FONT_NAME = "fusion-pixel-12px-monospaced-zh_hans.otf"


def default_font(repo: Path) -> Path | None:
    """字库可能在两个地方:工具箱里(直接从 toolkit 跑),或者仓库的 assets/fonts/
    里(apply-to.sh 把本脚本装进 <repo>/tools/ 之后)。两处都找一下。"""
    for candidate in (repo / "assets/fonts" / FONT_NAME, HERE / "fonts" / FONT_NAME):
        if candidate.exists():
            return candidate
    return None

# 界面上会出现、但不一定写在字面量里的字符:星级符号、方向箭头,以及
# printf 系列格式化可能产出的数字与标点。
DEFAULT_EXTRA = "★☆◀▶●○0123456789%-:. "


def strip_comments(text: str) -> str:
    """去掉 // 与 /* */ 注释,但不动字符串字面量。"""
    out: list[str] = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '"':
            out.append(c)
            i += 1
            while i < n:
                out.append(text[i])
                if text[i] == "\\":
                    i += 1
                    if i < n:
                        out.append(text[i])
                        i += 1
                    continue
                if text[i] == '"':
                    i += 1
                    break
                i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                i += 1
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


def literals(text: str) -> list[str]:
    """取出每一段双引号字面量的内容。"""
    found: list[str] = []
    i, n = 0, len(text)
    while i < n:
        if text[i] != '"':
            i += 1
            continue
        i += 1
        buf: list[str] = []
        while i < n and text[i] != '"':
            if text[i] == "\\":
                i += 2
                continue
            buf.append(text[i])
            i += 1
        i += 1
        found.append("".join(buf))
    return found


def collect(sources: list[Path], extra: str) -> str:
    chars = set(extra)
    for path in sources:
        text = strip_comments(path.read_text(encoding="utf-8"))
        for literal in literals(text):
            chars.update(literal)
    # ASCII 可打印区一并带上,免得以后加个英文标签又要重跑。
    chars.update(chr(c) for c in range(0x20, 0x7F))
    chars -= {"\n", "\t"}
    return "".join(sorted(chars))


def generate(font: Path, symbols: str, size: int, out: Path) -> None:
    subprocess.run(
        [
            "npx", "--yes", "lv_font_conv@1.5.3",
            "--font", str(font),
            "--size", str(size),
            "--bpp", "1",            # 点阵字体,开抗锯齿只会糊
            "--no-compress",
            "--format", "lvgl",
            "--lv-include", "lvgl.h",
            "--symbols", symbols,
            "-o", str(out),
        ],
        check=True,
    )
    # lv_font_conv 把完整命令行(含本机绝对路径)写进文件头注释;换成仓库相对路径,
    # 免得把 /Users/<名字>/… 提交进公开仓库
    text = out.read_text(encoding="utf-8")
    for absolute, rel in ((str(font), font.name), (str(out), out.name)):
        text = text.replace(absolute, rel)
    out.write_text(text, encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--repo", type=Path, default=Path.cwd(),
                    help="玩法仓库根目录(默认当前目录)")
    ap.add_argument("--font", type=Path, default=None,
                    help="源字库 OTF/TTF(默认找 <repo>/assets/fonts/ 或工具箱自带的那份)")
    ap.add_argument("--sizes", default="12,24",
                    help="要生成的字号,逗号分隔(默认 12,24)")
    ap.add_argument("--extra", default=DEFAULT_EXTRA,
                    help="额外强制包含的字符")
    ap.add_argument("--prefix", default="font_pixel",
                    help="产物文件名与字体变量名前缀(默认 font_pixel)")
    args = ap.parse_args()

    repo = args.repo.resolve()
    main_dir = repo / "main"
    if not main_dir.is_dir():
        print(f"找不到 {main_dir},--repo 指对了吗?", file=sys.stderr)
        return 1

    font = args.font or default_font(repo)
    if font is None:
        print(f"找不到源字库 {FONT_NAME};先跑 toolkit/apply-to.sh,或用 --font 指定。",
              file=sys.stderr)
        return 1
    if not font.exists():
        print(f"源字库不存在:{font}", file=sys.stderr)
        return 1
    if shutil.which("npx") is None:
        print("没有 npx,先装 Node.js", file=sys.stderr)
        return 1

    # 只扫 .c:字面量基本都在实现里,.h 里的宏文案也会被 .c 展开引用到。
    sources = sorted(p for p in main_dir.glob("*.c")
                     if not p.name.startswith(args.prefix))
    symbols = collect(sources, args.extra)

    # 源字库缺字会在设备上静默显示成空方框,宁可现在失败。
    from fontTools.ttLib import TTFont

    cmap = TTFont(font).getBestCmap()
    missing = [c for c in symbols if ord(c) not in cmap]
    if missing:
        print(f"源字库缺 {len(missing)} 个字形:{''.join(missing)}", file=sys.stderr)
        print("换一份覆盖更全的字库,或改掉界面上用到这些字的文案。", file=sys.stderr)
        return 1

    print(f"字库 {font}")
    print(f"扫描 {len(sources)} 个源文件,{len(symbols)} 个字形")
    for token in args.sizes.split(","):
        size = int(token.strip())
        out = main_dir / f"{args.prefix}_{size}.c"
        generate(font, symbols, size, out)
        print(f"  {out.relative_to(repo)}  {out.stat().st_size / 1024:.1f} KiB")

    print("\n别忘了把产物加进 main/CMakeLists.txt 的 SRCS。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
