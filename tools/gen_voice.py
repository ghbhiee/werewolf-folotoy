#!/usr/bin/env python3
"""生成狼人杀主持语音 main/ww_voice.c(16 kHz 单声道 IMA-ADPCM)。

三种来源(按优先级):自己的录音 > 千问 TTS > macOS say。

    DASHSCOPE_API_KEY=… python3 tools/gen_voice.py              # 千问 TTS(仓库里的版本就是这样生成的)
    python3 tools/gen_voice.py --engine say                     # macOS say(输出只许个人非商用,别发布)
    python3 tools/gen_voice.py --from-dir recordings            # 自己的录音,缺的键再用 --engine 补

录音文件名 = 下表的键名(例如 recordings/night.wav)。依赖:sox;千问需要 websockets。
输出约 8 KB/秒。来源与许可见 assets/README.md。
"""
from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# (键名, ww_cue_t 枚举名, 台词)——顺序无所谓,按枚举名对应
LINES = [
    ("deal", "WW_CUE_DEAL", "身份已发放，请查看手机，记住自己的身份。"),
    ("night", "WW_CUE_NIGHT", "天黑请闭眼。"),
    ("wolf_open", "WW_CUE_WOLF_OPEN", "狼人请睁眼，请选择今晚袭击的目标。"),
    ("wolf_close", "WW_CUE_WOLF_CLOSE", "狼人请闭眼。"),
    ("witch_open", "WW_CUE_WITCH_OPEN", "女巫请睁眼。"),
    ("witch_close", "WW_CUE_WITCH_CLOSE", "女巫请闭眼。"),
    ("seer_open", "WW_CUE_SEER_OPEN", "预言家请睁眼，请选择要查验的玩家。"),
    ("seer_close", "WW_CUE_SEER_CLOSE", "预言家请闭眼。"),
    ("dawn_peace", "WW_CUE_DAWN_PEACE", "天亮了，请大家睁眼。昨晚是平安夜。"),
    ("dawn_death", "WW_CUE_DAWN_DEATH", "天亮了，请大家睁眼。昨晚有玩家死亡，请看屏幕。"),
    ("discuss", "WW_CUE_DISCUSS", "请按顺序发言。"),
    ("vote", "WW_CUE_VOTE", "讨论结束，请在手机上投票。"),
    ("vote_end", "WW_CUE_VOTE_END", "投票结束。"),
    ("win_good", "WW_CUE_WIN_GOOD", "游戏结束，好人阵营胜利！"),
    ("win_wolf", "WW_CUE_WIN_WOLF", "游戏结束，狼人阵营胜利！"),
]

STEP = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66,
    73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408,
    449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630,
    9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
]
INDEX = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]


def adpcm_encode(samples: list[int]) -> bytes:
    """IMA-ADPCM 编码,和 ww_sound.c 里的解码器逐位对应(初始预测 0、步长下标 0)。"""
    pred, idx = 0, 0
    nibbles = []
    for s in samples:
        step = STEP[idx]
        diff = s - pred
        nib = 0
        if diff < 0:
            nib = 8
            diff = -diff
        delta = step >> 3
        if diff >= step:
            nib |= 4; diff -= step; delta += step
        step >>= 1
        if diff >= step:
            nib |= 2; diff -= step; delta += step
        step >>= 1
        if diff >= step:
            nib |= 1; delta += step
        pred = pred - delta if nib & 8 else pred + delta
        pred = max(-32768, min(32767, pred))
        idx = max(0, min(88, idx + INDEX[nib]))
        nibbles.append(nib)
    if len(nibbles) % 2:
        nibbles.append(0)
    return bytes(nibbles[i] | (nibbles[i + 1] << 4) for i in range(0, len(nibbles), 2))


def qwen_tts(text: str, voice: str, out: Path) -> None:
    """千问 qwen-audio-3.1-tts-flash,百炼 WebSocket,直接要 16 kHz PCM,存成 wav。"""
    import asyncio
    import json
    import os
    import uuid

    import websockets

    key = os.environ["DASHSCOPE_API_KEY"]

    async def run() -> bytes:
        tid = uuid.uuid4().hex
        hdrs = {"Authorization": f"bearer {key}"}
        major = int(websockets.__version__.split(".")[0])
        kw = {"additional_headers": hdrs} if major >= 14 else {"extra_headers": hdrs}
        audio = b""
        async with websockets.connect("wss://dashscope.aliyuncs.com/api-ws/v1/inference",
                                      max_size=1 << 24, **kw) as ws:
            await ws.send(json.dumps({
                "header": {"action": "run-task", "task_id": tid, "streaming": "duplex"},
                "payload": {"task_group": "audio", "task": "tts", "function": "SpeechSynthesizer",
                            "model": "qwen-audio-3.1-tts-flash", "input": {},
                            "parameters": {"text_type": "PlainText", "voice": voice,
                                           "format": "pcm", "sample_rate": 16000}}}))
            while True:
                raw = await asyncio.wait_for(ws.recv(), 30)
                if isinstance(raw, bytes):
                    audio += raw
                    continue
                ev = json.loads(raw)
                e = ev["header"].get("event")
                if e == "task-started":
                    for action, inp in (("continue-task", {"text": text}), ("finish-task", {})):
                        await ws.send(json.dumps({"header": {"action": action, "task_id": tid,
                                                             "streaming": "duplex"},
                                                  "payload": {"input": inp}}))
                elif e == "task-finished":
                    return audio
                elif e == "task-failed":
                    raise RuntimeError(ev["header"].get("error_message"))

    pcm = asyncio.run(run())
    with wave.open(str(out), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(16000)
        w.writeframes(pcm)


def to_pcm16k(src: Path, tmp: Path) -> list[int]:
    out = tmp / (src.stem + "_16k.wav")      # 别和输入同名(千问来源本身就是 wav)
    # 16 kHz 单声道,去头尾静音,峰值归一到 -2 dBFS,再加 30 ms 头尾留白防爆音
    subprocess.run(["sox", str(src), "-r", "16000", "-c", "1", "-b", "16", str(out),
                    "silence", "1", "0.02", "0.3%", "reverse", "silence", "1", "0.02", "0.3%", "reverse",
                    "norm", "-2", "pad", "0.03", "0.05"], check=True)
    with wave.open(str(out)) as w:
        raw = w.readframes(w.getnframes())
    return [int.from_bytes(raw[i:i + 2], "little", signed=True) for i in range(0, len(raw), 2)]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--engine", choices=["qwen", "say"], default="qwen")
    ap.add_argument("--voice", default=None,
                    help="音色:千问默认 xieshurou_v3.1,say 默认 Tingting")
    ap.add_argument("--rate", default="185", help="say 语速(字/分钟左右)")
    ap.add_argument("--from-dir", type=Path, help="自己录音的目录,文件名=键名(night.wav 等)")
    ap.add_argument("--out", type=Path, default=ROOT / "main" / "ww_voice.c")
    a = ap.parse_args()

    total = 0
    parts = []
    table = []
    with tempfile.TemporaryDirectory() as t:
        tmp = Path(t)
        for key, enum, text in LINES:
            src = None
            if a.from_dir:
                for ext in (".wav", ".aiff", ".m4a", ".mp3"):
                    if (a.from_dir / (key + ext)).exists():
                        src = a.from_dir / (key + ext)
                        break
            if src is None and a.engine == "qwen":
                src = tmp / (key + "_qwen.wav")
                qwen_tts(text, a.voice or "xieshurou_v3.1", src)
            elif src is None:
                src = tmp / (key + ".aiff")
                subprocess.run(["say", "-v", a.voice or "Tingting", "-r", a.rate, "-o", str(src), text],
                               check=True)
            pcm = to_pcm16k(src, tmp)
            data = adpcm_encode(pcm)
            total += len(data)
            secs = len(pcm) / 16000
            print(f"  {key:12s} {secs:4.1f}s {len(data) / 1024:5.1f} KiB  {text}")
            lines = [",".join(f"0x{b:02x}" for b in data[i:i + 16]) + ","
                     for i in range(0, len(data), 16)]
            parts.append(f"// {text}({secs:.1f} 秒)\nstatic const uint8_t V_{key.upper()}[{len(data)}] = {{\n"
                         + "\n".join("    " + ln for ln in lines) + "\n};\n")
            table.append(f"    [{enum}] = {{V_{key.upper()}, sizeof(V_{key.upper()})}},")

    src = (
        "// main/ww_voice.c —— 由 tools/gen_voice.py 生成,不要手改。\n"
        f"// 来源:{'自录音 + ' if a.from_dir else ''}"
        f"{'千问 qwen-audio-3.1-tts-flash ' + (a.voice or 'xieshurou_v3.1') if a.engine == 'qwen' else 'macOS say -v ' + (a.voice or 'Tingting') + '(仅限个人非商业使用,不要发布)'}"
        "。见 assets/README.md。\n"
        "// 格式:16 kHz 单声道 IMA-ADPCM,每字节两个采样,低 4 位在前。\n"
        "#include \"ww_voice.h\"\n\n#include <stddef.h>\n\n#include \"ww_core.h\"\n\n"
        + "\n".join(parts)
        + "\ntypedef struct {\n    const uint8_t *data;\n    uint32_t len;\n} clip_t;\n\n"
        "static const clip_t CLIPS[WW_CUE_COUNT] = {\n" + "\n".join(table) + "\n};\n\n"
        "bool ww_voice_get(int cue, const uint8_t **data, uint32_t *len)\n{\n"
        "    if (cue <= 0 || cue >= WW_CUE_COUNT || !CLIPS[cue].data) return false;\n"
        "    *data = CLIPS[cue].data;\n    *len = CLIPS[cue].len;\n    return true;\n}\n"
    )
    a.out.write_text(src, encoding="utf-8")
    print(f"写入 {a.out.relative_to(ROOT)}:{len(LINES)} 句,共 {total / 1024:.0f} KiB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
