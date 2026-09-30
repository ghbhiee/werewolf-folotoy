#!/usr/bin/env python3
"""HTTP bot 整局测试:起一个主机仿真服务器,N 个 bot 用真实的玩家接口
(/api/join /api/state /api/act)玩到结束,主持人操作走虚拟三键 /sim/key。

思路照 ~/cc/werewolf/test_game.py。只用标准库。

    ./hostsim/build.sh && python3 hostsim/bot_game.py            # 默认 4/6/9/12 人各 2 局
    python3 hostsim/bot_game.py --sizes 6 --games 5 --speed 30

检查:每局都能结束;胜负与最终身份一致;对局中活人看不到别人的身份,狼人只看到
狼队友;死人/结束后看到全部身份;轮询在没有变化时返回 204。
"""
from __future__ import annotations

import argparse
import json
import os
import random
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)


class Server:
    def __init__(self, speed: float):
        s = socket.socket()
        s.bind(("127.0.0.1", 0))
        self.port = s.getsockname()[1]
        s.close()
        env = dict(os.environ, WW_ROOT=REPO)
        self.proc = subprocess.Popen(
            [os.path.join(HERE, "out", "server"), str(self.port), str(speed)],
            env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        self.base = f"http://127.0.0.1:{self.port}"
        for _ in range(100):
            try:
                urllib.request.urlopen(self.base + "/sim/view", timeout=1).read()
                return
            except OSError:
                time.sleep(0.05)
        raise RuntimeError("仿真服务器没起来")

    def stop(self):
        self.proc.terminate()
        try:
            self.proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            self.proc.kill()

    def req(self, path: str, form: dict | None = None):
        data = urllib.parse.urlencode(form).encode() if form is not None else None
        r = urllib.request.Request(self.base + path, data=data)
        if data is not None:
            r.add_header("Content-Type", "application/x-www-form-urlencoded")
        try:
            with urllib.request.urlopen(r, timeout=5) as resp:
                body = resp.read().decode()
                return resp.status, (json.loads(body) if body and body[0] == "{" else body)
        except urllib.error.HTTPError as e:
            body = e.read().decode()
            return e.code, (json.loads(body) if body.startswith("{") else body)

    def key(self, k: str, g: str = "click"):
        self.req("/sim/key", {"k": k, "g": g})

    def view(self):
        return self.req("/sim/view")[1]


class Bot:
    def __init__(self, srv: Server, seat: int, rng: random.Random):
        self.srv, self.seat, self.rng = srv, seat, rng
        code, j = srv.req("/api/join", {"name": f"机器人{seat}"})
        assert code == 200 and "token" in j, (code, j)
        self.tok = j["token"]
        self.st: dict = {}
        self.ver = 0
        self.n204 = 0

    def poll(self):
        code, j = self.srv.req(f"/api/state?t={self.tok}&v={self.ver}")
        if code == 204:
            self.n204 += 1
            return self.st
        assert code == 200, (code, j)
        self.st, self.ver = j, j["v"]
        return j

    def act(self, a: str, **kw):
        return self.srv.req("/api/act", {"t": self.tok, "a": a, **kw})

    def alive_others(self, st):
        return [s["s"] for s in st["seats"] if s.get("name") and s.get("alive") and s["s"] != self.seat]

    def step(self):
        st = self.poll()
        you = st.get("you", {})
        ph = st["ph"]
        if ph == "lobby" and not you.get("seat"):
            code, j = self.act("seat", seat=self.seat)
            assert code == 200, j
            return
        if not you.get("pending") or st.get("step"):
            return
        r = self.rng
        if ph == "wolf":
            mates = {s["s"] for s in st["seats"] if s.get("mate")} | {self.seat}
            cands = [s for s in self.alive_others(st) if s not in mates] + [0]
            self.act("wolf", target=r.choice(cands))
        elif ph == "witch" and "witch" in st:
            w = st["witch"]
            choice = r.random()
            if w["antidote"] and w["kill"] and choice < 0.4:
                self.act("witch", do=1)
            elif w["poison"] and choice < 0.6:
                self.act("witch", do=2, target=r.choice(self.alive_others(st)))
            else:
                self.act("witch", do=0)
        elif ph == "seer":
            others = self.alive_others(st)
            if others:
                code, j = self.act("seer", target=r.choice(others))
                assert code == 200, j
        elif ph == "vote":
            code, j = self.act("vote", target=r.choice(self.alive_others(st) + [0]))
            assert code == 200, j


def check_secrecy(bots: list[Bot], roles: dict[int, str]):
    """对局中:活人只看到自己的身份;狼人额外看到狼队友标记。"""
    for b in bots:
        st = b.st
        if st.get("ph") in ("lobby", "over") or not st:
            continue
        you = st["you"]
        for s in st["seats"]:
            if s["s"] == b.seat:
                continue
            if you["alive"]:
                assert "role" not in s, f"{b.seat}号(活)看到了{s['s']}号的身份"
                if s.get("mate"):
                    assert you["role"] == "wolf" and roles[s["s"]] == "wolf"
            else:
                assert s.get("role") == roles[s["s"]], "死人应看到全部身份"


def play(srv: Server, n: int, seed: int) -> str:
    rng = random.Random(seed)
    # 座位数调成 n
    v = srv.view()
    while v["kind"] != "lobby":
        time.sleep(0.05)
        v = srv.view()
    for _ in range(12):
        srv.key("down")
    for _ in range(n - 2):
        srv.key("up")
    bots = [Bot(srv, s, rng) for s in range(1, n + 1)]
    for b in bots:
        b.step()
    for b in bots:
        b.poll()
    assert bots[0].st["seated"] == n, bots[0].st
    assert bots[0].st["n"] == n
    srv.key("ok")
    srv.key("ok")
    roles: dict[int, str] = {}
    deadline = time.time() + 240
    last_phase = ""
    discuss_seen_at = 0.0
    while time.time() < deadline:
        for b in bots:
            b.step()
        st = bots[0].st
        ph = st["ph"]
        for b in bots:
            if b.st.get("you", {}).get("role"):
                roles[b.seat] = b.st["you"]["role"]
        if ph != last_phase:
            last_phase = ph
            discuss_seen_at = time.time()
        if ph == "discuss":
            # 主持人按"下一位"走一轮,然后一半局用主持人开投票、一半由玩家开
            if time.time() - discuss_seen_at > 0.3:
                v = srv.view()
                assert "发言" in v["big"], v
                if rng.random() < 0.5:
                    srv.key("down")
                    srv.key("ok")
                    srv.key("ok")
                else:
                    alive = [b for b in bots if b.st["you"]["alive"]]
                    code, j = alive[0].act("startvote")
                    assert code == 200, j
                discuss_seen_at = time.time() + 5
        check_secrecy(bots, roles)
        if ph == "over":
            break
        time.sleep(0.03)
    else:
        raise AssertionError(f"{n} 人局没在限定时间内结束,卡在 {last_phase}")

    for b in bots:
        b.poll()
    st = bots[0].st
    winner = st["winner"]
    final = {s["s"]: s["role"] for s in st["seats"]}
    assert final == roles, (final, roles)
    wolves = sum(1 for s in st["seats"] if s["alive"] and s["role"] == "wolf")
    goods = sum(1 for s in st["seats"] if s["alive"]) - wolves
    if winner == "good":
        assert wolves == 0
    else:
        assert wolves >= goods
    v = srv.view()
    assert v["big"] in ("好人胜利", "狼人胜利"), v
    # 再来一局:回大厅,座位保留
    srv.key("ok")
    srv.key("ok")
    bots[0].poll()
    assert bots[0].st["ph"] == "lobby" and bots[0].st["seated"] == n
    # 清空房间,准备下一局换人数
    srv.key("ok", "long")
    for _ in range(5):
        srv.key("down")          # 踢出玩家/机器人/AI/网络/音量 -> 清空房间
    assert srv.view()["items"][srv.view()["sel"]] == "清空房间"
    srv.key("ok")
    srv.key("ok")
    code, _ = srv.req(f"/api/state?t={bots[0].tok}&v=0")
    assert code == 401, "清空房间后旧 token 应失效"
    n204 = sum(b.n204 for b in bots)
    rounds = st["round"]
    print(f"  {n:2d} 人局 seed={seed}: {'好人' if winner == 'good' else '狼人'}胜,"
          f"{rounds} 轮;204 未变化响应 {n204} 次")
    assert n204 > 0, "轮询应该能拿到 204"
    return winner


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sizes", default="4,6,9,12")
    ap.add_argument("--games", type=int, default=2)
    ap.add_argument("--speed", type=float, default=25.0)
    ap.add_argument("--seed", type=int, default=1)
    a = ap.parse_args()
    srv = Server(a.speed)
    try:
        # 页面能拿到
        code, html = srv.req("/")
        assert code == 200 and "狼人杀" in html
        results = []
        for n in [int(x) for x in a.sizes.split(",")]:
            for g in range(a.games):
                results.append(play(srv, n, a.seed * 1000 + n * 10 + g))
        print(f"bot HTTP 整局:{len(results)} 局全部正常结束"
              f"(好人胜 {results.count('good')},狼人胜 {results.count('wolf')})")
    finally:
        srv.stop()


if __name__ == "__main__":
    try:
        main()
    except AssertionError as e:
        print("FAIL:", e)
        sys.exit(1)
