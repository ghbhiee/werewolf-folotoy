<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Werewolf (LAN edition)

A stripped-down Werewolf game for the AI Passport. The device is the game server,
the moderator, and the moderator screen at the same time; players join by
scanning a QR code on the device with their phone browser. Without API keys it
is a pure LAN game; with a DeepSeek key and a DashScope (Qwen) key the device also
runs AI players that decide, speak, and listen to human speech.

Rules follow the original `werewolf` project (`game.py`): 2–12 players, the
wolf / seer / witch / villager board, night order wolves → witch → seer,
alternating speaking direction, voting with abstention, ties exile nobody.

## Architecture

| Layer | File | Notes |
| --- | --- | --- |
| Rules state machine | `main/ww_core.{h,c}` | Pure C. Time and random seed are injected, so it runs unchanged in host tests. |
| Three-button moderator | `main/ww_host.{h,c}` | Pure C. Maps UP/DOWN/OK gestures to game actions and computes a view model of the screen. |
| HTTP protocol | `main/ww_api.{h,c}` | Pure C. Shared by the firmware and the host simulator, so both behave identically. |
| Screen | `main/ww_ui.{h,c}` | LVGL rendering of the view model with the `ui_pixel` theme. |
| Wi-Fi | `main/ww_net.{h,c}` | Home Wi-Fi (STA) if configured, otherwise its own hotspot (SoftAP, at most 10 phones on the ESP32-C3). |
| HTTP server | `main/ww_http.{h,c}` | `esp_http_server`, 12 sockets with LRU purge; the player page is gzip-embedded. |
| Sound | `main/ww_sound.{h,c}`, `main/ww_voice.{h,c}` | Pre-recorded Mandarin prompts (IMA-ADPCM), sine-tone fallback; an I2S lock shared with the AI voice. |
| AI prompts | `main/ww_ai.{h,c}` | Pure C. Picks the next AI seat to act, writes a prompt from that seat's own view only, parses the model's JSON, falls back to a random legal move. |
| Cloud | `main/ww_cloud.{h,c}` | DeepSeek chat over HTTPS; Qwen TTS (16 kHz PCM streamed to I2S) and Qwen streaming ASR over WebSocket. Keys in NVS namespace `wwai`. |
| Brain task | `main/ww_brain.{h,c}` | One task does all cloud work in sequence, so at most one TLS connection is open. |
| Glue | `main/ww_app.{h,c}`, `main/main.c` | One mutex around the game state; button callback only enqueues. |
| Player page | `web/player.html`, `web/setup.html` | Single-file pages, no external resources. |

Players poll `GET /api/state` once per second; unchanged state returns `204`.
Actions are `POST /api/act` with a form body. Short polling was chosen over
WebSocket to keep sockets free and to make reconnection after a phone locks
trivial.

## Buttons

| Screen | UP | DOWN | OK click | OK double | OK long |
| --- | --- | --- | --- | --- | --- |
| Lobby | seats +1 | seats −1 | start (confirm twice) | switch QR (hotspot) | menu |
| Announcements | — | — | skip wait (confirm twice) | status | menu |
| Night role | — | — | force next (confirm twice) | status | menu |
| Discussion | previous speaker | next speaker | start voting (confirm twice) | status | talk, release to send |
| Voting | — | — | end voting (confirm twice) | status | menu |
| Game over | — | — | play again (confirm twice) | status | menu |
| Menu | up | down | select | — | back |

"Confirm twice" means the first press arms the action for 3 seconds and the
second press executes it; UP or DOWN cancels. Holding UP opens the menu on any
screen. Menu items that take the device off home Wi-Fi also need two presses,
and a single OK press leaves the Wi-Fi setup screen.
The device screen never shows roles or night targets during a game, because the
person holding it also plays.

## AI players and pacing

- Bots fill empty seats. Without keys they act at random; with keys (and AI
  turned on in AI settings) DeepSeek decides for them and writes their speeches,
  which Qwen reads aloud through the device speaker. If the cloud does not answer
  within 25 seconds the bot falls back to a random legal move.
- A human speaker holds OK, waits for the beep, talks, and releases. The audio is
  streamed to Qwen ASR while recording; the text goes into the speech log shown on
  every phone and into the AI prompts.
- Every night role phase lasts at least a random 8 to 15 seconds and phases of
  dead or missing roles last 8 to 18 seconds; AI votes wait a random 3 to 15
  seconds. Otherwise the timing would reveal which roles are AI.
- The final ASR message carries per-word timestamps and is several kilobytes
  long; the firmware keeps only the first 2 KB and reads `event`, `text`, and
  `sentence_end`, which precede the word list.
- The WebSocket client masks outgoing payloads in place, so every buffer passed
  to `ww_asr_feed()` must be writable RAM (a `const` buffer in flash crashes).

## Host tools

```bash
./tools/validate.sh --static          # includes the werewolf host tests below
./hostsim/build.sh                    # host simulator server
./hostsim/out/server 8080             # open http://localhost:8080/sim
python3 hostsim/bot_game.py           # HTTP bot full games against the simulator
./hostsim/lvgl/build.sh               # off-screen renders of every screen to PNG
python3 tools/gen_cjk_font.py --repo .   # after changing any on-screen text
python3 tools/gen_voice.py            # regenerate voice prompts
```
