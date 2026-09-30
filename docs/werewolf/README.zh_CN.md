<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# 狼人杀（局域网精简版）

AI Passport 上的精简版狼人杀。设备同时是游戏服务器、主持人和主持屏；玩家用手机
浏览器扫设备屏幕上的二维码加入。不填 API Key 就是纯局域网游戏；填了 DeepSeek 和百炼
（千问）Key，设备还能跑 AI 玩家：会决策、会说话、能听懂真人发言。

规则沿用原 `werewolf` 项目（`game.py`）：2–12 人，狼人 / 预言家 / 女巫 / 村民板子，
夜晚顺序 狼人 → 女巫 → 预言家，发言方向每天交替，可弃票，平票无人出局。

## 架构

| 层 | 文件 | 说明 |
| --- | --- | --- |
| 规则状态机 | `main/ww_core.{h,c}` | 纯 C，时间和随机种子从外面注入，主机测试原样可跑。 |
| 三键主持 | `main/ww_host.{h,c}` | 纯 C，把上/下/确定手势变成游戏动作，并算出屏幕的视图模型。 |
| HTTP 协议 | `main/ww_api.{h,c}` | 纯 C，固件和主机仿真服务器共用，两边行为一致。 |
| 屏幕 | `main/ww_ui.{h,c}` | 用 `ui_pixel` 主题把视图模型画出来（LVGL）。 |
| Wi-Fi | `main/ww_net.{h,c}` | 配过家里 Wi-Fi 就连（STA），否则自己开热点（SoftAP，ESP32-C3 最多 10 台手机）。 |
| HTTP 服务器 | `main/ww_http.{h,c}` | `esp_http_server`，12 个连接 + LRU 回收；玩家网页 gzip 后嵌进固件。 |
| 声音 | `main/ww_sound.{h,c}`、`main/ww_voice.{h,c}` | 预录中文提示语（IMA-ADPCM），没有时退回正弦提示音；和 AI 发声共用一把 I2S 锁。 |
| AI 提示词 | `main/ww_ai.{h,c}` | 纯 C：挑下一个要出手的 AI 座位，只按这个座位自己能看到的信息写提示词，解析模型 JSON，失败随机兜底。 |
| 云端 | `main/ww_cloud.{h,c}` | DeepSeek 走 HTTPS；千问 TTS（16 kHz PCM 边收边播）和千问流式 ASR 走 WebSocket。Key 存 NVS 命名空间 `wwai`。 |
| 大脑任务 | `main/ww_brain.{h,c}` | 所有联网的活由一个任务串行做，同一时刻最多一条 TLS。 |
| 胶水 | `main/ww_app.{h,c}`、`main/main.c` | 一把锁护住游戏状态；按键回调只入队。 |
| 玩家网页 | `web/player.html`、`web/setup.html` | 单文件，无外部资源。 |

玩家每秒 `GET /api/state` 一次，状态没变返回 `204`；动作走 `POST /api/act`（表单）。
选短轮询而不是 WebSocket，是为了省 socket，也让手机锁屏回来重连不需要任何额外逻辑。

## 按键

| 界面 | 上 | 下 | 确定单击 | 确定双击 | 确定长按 |
| --- | --- | --- | --- | --- | --- |
| 大厅 | 座位 +1 | 座位 −1 | 开始（两次确认） | 切换二维码（热点） | 菜单 |
| 公告类阶段 | — | — | 跳过等待（两次确认） | 状态页 | 菜单 |
| 夜晚角色 | — | — | 强制下一步（两次确认） | 状态页 | 菜单 |
| 讨论 | 上一位发言人 | 下一位发言人 | 开始投票（两次确认） | 状态页 | 按住说话，松手发送 |
| 投票 | — | — | 结束投票（两次确认） | 状态页 | 菜单 |
| 游戏结束 | — | — | 再来一局（两次确认） | 状态页 | 菜单 |
| 菜单 | 上移 | 下移 | 执行 | — | 返回 |

“两次确认”：第一次按下后 3 秒内再按一次才执行，按上/下取消。任何界面长按「上」都能开菜单。
会让设备离开家里 Wi-Fi 的网络菜单项也要按两次；Wi-Fi 配置页上单击确定即可退出。
拿设备的人也是玩家，所以对局中设备屏幕绝不显示身份和夜间目标。

## AI 玩家与节奏

- 机器人补满空座位。没 Key 时随机行动；有 Key 且在 AI 设置里打开后，由 DeepSeek 替它们
  决策、写发言，千问从设备喇叭读出来。云端 25 秒没回就随机兜底。
- 真人发言：按住确定，听到“嘀”再说，说完松手。录音时边录边送千问 ASR，转出的文字进
  发言记录（每部手机都能看）和 AI 的提示词。
- 夜晚每个角色阶段至少随机 8–15 秒，角色已死/没有时假等 8–18 秒；AI 投票随机等 3–15 秒。
  否则从节奏就能听出哪个身份是 AI。
- ASR 定稿消息带逐字时间戳，有好几 KB；固件只收开头 2 KB，从里面取 `event`、`text`、
  `sentence_end`（它们都排在逐字列表前面）。
- WebSocket 客户端发送时会原地做掩码，所以传给 `ww_asr_feed()` 的缓冲必须是 RAM 里可写的
  （放在 flash 里的 `const` 缓冲会直接崩）。

## 主机侧工具

```bash
./tools/validate.sh --static          # 已包含下面这些狼人杀主机测试
./hostsim/build.sh                    # 编主机仿真服务器
./hostsim/out/server 8080             # 打开 http://localhost:8080/sim
python3 hostsim/bot_game.py           # bot 走真实 HTTP 接口打整局
./hostsim/lvgl/build.sh               # 离屏渲染每个界面为 PNG
python3 tools/gen_cjk_font.py --repo .   # 改了任何屏幕文案之后
python3 tools/gen_voice.py            # 重新生成提示语音
```
