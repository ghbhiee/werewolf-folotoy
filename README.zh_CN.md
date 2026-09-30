<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# FoloToy AI Passport 狼人杀

完全跑在一台 [FoloToy AI Passport](https://ai-passport.folotoy.cn/)（ESP32-C3，无 PSRAM，
240×320 屏，三个按键）上的局域网狼人杀。设备同时是游戏服务器、主持人和主持屏；玩家用手机
浏览器扫设备上的二维码加入，拿着设备的人用三个按键控场。

可选的 AI 玩家：用 DeepSeek 想事情、写发言，用千问 TTS 从设备喇叭说出来。真人发言时按住
「确定」对着设备说话，语音转成文字，AI 玩家据此应对。

本仓库 fork 自 FoloToy 的 [ai-passport](https://github.com/folotoy/ai-passport) 板级模板（MIT）。
玩法代码在 `main/ww_*.c`、`web/`、`hostsim/`、`docs/werewolf/`。

## 功能

- 2–12 人；狼人、预言家、女巫、村民。夜晚顺序：狼人 → 女巫 → 预言家。
- 玩家的一切操作都在自己手机上：身份卡、夜间行动、投票、发言记录、出局后上帝视角。
- 对局中设备屏幕绝不显示身份和夜间目标（拿设备的人也是玩家）。
- 设备喇叭播主持语音（"天黑请闭眼""狼人请睁眼"……）。
- 机器人补满空座位，一部手机就能测完整一局。
- AI 玩家（可选）：DeepSeek 决策和写发言、千问 TTS 发声、千问流式 ASR 转写真人发言。
  夜晚角色阶段和投票都按类似真人的随机节奏进行，听节奏猜不出哪个身份是 AI。
- 网络：配过家里 Wi-Fi 就连（STA），否则设备自己开热点（ESP32-C3 最多 10 台手机）。

## 快速开始

需要 ESP-IDF 5.5.3。

```bash
source ~/esp/esp-idf-v5.5.3/export.sh
idf.py set-target esp32c3
idf.py build
idf.py -p /dev/cu.usbmodemXXXX flash
```

用分段的 `idf.py flash`。已配号的设备**不要**把合并后的整片镜像写到 `0x0`，会抹掉
`0x356000` 的 `cardid` 分区。

设备先连家里 Wi-Fi，连不上就自己开热点，屏上显示密码和二维码。家里 Wi-Fi 在主持菜单
「网络设置 → 配置家里 Wi-Fi」里用手机配置。

## AI 玩家

1. 大厅长按确定开主持菜单 → AI 设置 → 填写 Key。设备显示 `/ai` 页面的二维码，手机扫码
   粘贴百炼（DashScope）Key 和 DeepSeek Key。Key 只存在设备 NVS 里，不会回传给任何浏览器。
2. AI 设置 → 测试 AI：设备用 DeepSeek 写一句话，再用千问读出来。
3. 大厅选「机器人补满空位」。配好 Key 后机器人就是 AI 玩家。

所有云端调用都由设备直接发起，同一时刻只有一条 TLS 连接（实测堆最低约 35 KB）。

## 按键

| 界面 | 上 | 下 | 确定单击 | 确定双击 | 确定长按 |
| --- | --- | --- | --- | --- | --- |
| 大厅 | 座位 +1 | 座位 −1 | 开始（按两次） | 切换二维码 / 状态 | 主持菜单 |
| 夜晚 | — | — | 强制下一步（按两次） | 状态 | 主持菜单 |
| 讨论 | 上一位发言人 | 下一位发言人 | 开始投票（按两次） | 状态 | 按住说话，松手发送 |
| 投票 | — | — | 结束投票（按两次） | 状态 | 主持菜单 |

任何界面长按「上」都能打开主持菜单，讨论阶段就靠它。

## 开发

```bash
./tools/validate.sh --static      # 仓库检查 + 主机测试 + HTTP bot 整局
./hostsim/build.sh && ./hostsim/out/server 8080   # 浏览器仿真，打开 /sim
./hostsim/lvgl/build.sh           # 离屏渲染每个界面为 PNG
```

架构见 [`docs/werewolf/README.zh_CN.md`](docs/werewolf/README.zh_CN.md)。上游的 AI 开发规则在
[`AGENTS.zh_CN.md`](AGENTS.zh_CN.md)。

## 致谢与许可

- 板级模板、BSP、像素 UI 主题：FoloToy，MIT（[`LICENSE`](LICENSE)）。
- 规则与提示词改编自作者自己的联网版狼人杀项目。
- 像素字体：缝合像素字体（Fusion Pixel Font），SIL Open Font License 1.1。
- 主持语音：用千问 `qwen-audio-3.1-tts-flash` 生成；重新生成或换成自己的录音见
  [`assets/README.zh_CN.md`](assets/README.zh_CN.md)。
