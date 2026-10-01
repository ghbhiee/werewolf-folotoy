<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Werewolf on FoloToy AI Passport

A LAN Werewolf (Mafia) game that runs entirely on a
[FoloToy AI Passport](https://ai-passport.folotoy.cn/) (ESP32-C3, no PSRAM,
240 x 320 screen, three buttons). The device is the game server, the moderator,
and the moderator screen. Players join by scanning a QR code with their phone
browser; the person holding the device runs the game with three buttons.

Optional AI players think with DeepSeek and speak through the device speaker
with Qwen text-to-speech. A human speaker holds the OK button, talks into the
device, and the speech is transcribed so the AI players can react to it.

This repository is a fork of the FoloToy
[ai-passport](https://github.com/folotoy/ai-passport) board template (MIT). The
play lives in `main/ww_*.c`, `web/`, `hostsim/`, and `docs/werewolf/`.

## Features

- 2 to 12 players; wolves, seer, witch, and villagers. Night order: wolves, witch, seer.
- Everything a player does happens on their own phone: role card, night actions,
  voting, a speech log, and a god view after being eliminated.
- The device screen never shows roles or night targets during a game, because the
  person holding it plays too.
- Voice prompts ("night falls", "wolves, open your eyes", ...) from the device speaker.
- Bots fill empty seats so a whole game can be tested with one phone.
- AI players (optional): decisions and speeches by DeepSeek, voices by Qwen TTS,
  human speech transcribed by Qwen streaming ASR. Night phases and votes run at a
  human-like random pace so the timing never reveals which roles are AI.
- Networking: home Wi-Fi (station) when configured, otherwise the device opens its
  own hotspot (at most 10 phones on the ESP32-C3).

## Quick start

Requires ESP-IDF 5.5.3.

```bash
source ~/esp/esp-idf-v5.5.3/export.sh
idf.py set-target esp32c3
idf.py build
idf.py -p /dev/cu.usbmodemXXXX flash
```

During development use the segmented `idf.py flash`: it keeps the settings
stored on the device. The merged `FoloToy-AI-Passport-full.bin` (from
`./tools/validate.sh --firmware`) is flashed at offset `0x0`; it ends before the
protected `cardid` partition at `0x356000`, but it overwrites the settings area,
so saved Wi-Fi and AI keys must be entered again. Never run `idf.py erase-flash`
on a provisioned device.

The device first tries home Wi-Fi, otherwise it opens a hotspot and shows its
password and a QR code. Home Wi-Fi is configured from the host menu (network
settings, then configure home Wi-Fi) with a phone.

## AI players

1. Host menu (hold OK in the lobby), then AI settings, then enter keys. The device
   shows a QR code for its `/ai` page; paste a DashScope (Alibaba Cloud Model
   Studio) key and a DeepSeek key there. Keys stay in the device NVS and are never
   sent back to any browser.
2. AI settings, then test AI: the device writes one line with DeepSeek and reads
   it aloud with Qwen.
3. In the lobby, choose "fill empty seats with bots". With keys configured the bots
   become AI players.

All cloud calls are made from the device, one TLS connection at a time
(about 35 KB free heap at the lowest point in testing).

## Controls

| Screen | UP | DOWN | OK click | OK double | OK hold |
| --- | --- | --- | --- | --- | --- |
| Lobby | seats +1 | seats -1 | start (press twice) | switch QR / status | host menu |
| Night | - | - | force next (press twice) | status | host menu |
| Discussion | previous speaker | next speaker | start voting (press twice) | status | talk (release to send) |
| Voting | - | - | end voting (press twice) | status | host menu |

Holding UP opens the host menu on any screen, which is how to reach it during
the discussion.

## Development

```bash
./tools/validate.sh --static      # repository checks + host tests + HTTP bot games
./hostsim/build.sh && ./hostsim/out/server 8080   # browser simulator at /sim
./hostsim/lvgl/build.sh           # off-screen renders of every screen to PNG
```

See [`docs/werewolf/README.md`](docs/werewolf/README.md) for the architecture.
The upstream AI development rules are in [`AGENTS.md`](AGENTS.md).

## Credits and licenses

- Board template, BSP, and pixel UI theme: FoloToy, MIT ([`LICENSE`](LICENSE)).
- Game rules and prompts adapted from the author's own networked Werewolf project.
- Pixel font: Fusion Pixel Font, SIL Open Font License 1.1.
- Voice prompts: generated with Qwen `qwen-audio-3.1-tts-flash`; see
  [`assets/README.md`](assets/README.md) to regenerate them or use your own recordings.
