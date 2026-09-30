<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# 资源目录（Assets）

本目录集中存放可复用的资源（字库、图片、音乐等），按资源类型分子目录管理。每个资源放在其类型对应的子目录，并记录放置路径、命名方式、集成方式与来源/许可。二进制资源（字体、图片、音频）不属于纯 markdown 文档，请勿与文档混放。涉及版权/授权的资源需注明来源与许可。

## 字库（fonts）

可复用的字库文件与生成的字库源码放在 `fonts/`。

- 命名要能反映字族、字重、字级与格式。
- 记录来源、许可、字符范围、转换命令与目标放置路径。
- 添加字库前评估 Flash 与内部 RAM 影响；ESP32-C3 无 PSRAM。
- 不提交许可不允许分发的字库。

### 引入：缝合像素字体 12px 等宽（简体中文）

| 项 | 值 |
| --- | --- |
| 文件 | `fonts/fusion-pixel-12px-monospaced-zh_hans.otf` |
| 来源 | <https://github.com/TakWolf/fusion-pixel-font>（从 AI Passport 基线工具箱复制） |
| 许可 | SIL Open Font License 1.1（`fonts/fusion-pixel-OFL.txt`），允许再分发 |
| 使用者 | 狼人杀玩法（`main/ww_*.c`） |
| 产物 | `main/font_pixel_12.c`、`main/font_pixel_24.c` |
| 转换 | `python3 tools/gen_cjk_font.py --repo .`（封装 `lv_font_conv`，1 bpp） |
| 字符范围 | `main/*.c` 字符串字面量里出现的字 + ASCII 0x20-0x7E，目前 370 个左右 |

改了任何屏幕文案都要重跑；源字库缺字时生成器直接报错。玩家在手机上填的名字不会
画到设备屏上（任意名字不在子集里）。

## 图片（images）

可复用的源图与生成的显示资产放在 `images/`。

- 使用描述性命名，并记录尺寸、像素格式、转换步骤与目标路径。
- 优先采用适合 240 × 320 RGB565 显示的格式，并纳入 Flash 与内部 RAM 考量。
- 许可允许时保留可编辑源文件，并记录来源与许可。
- 图片中不得包含设备二维码秘密、凭证或个人数据。

## 音乐与音效（music）

可复用的音乐与音效源码放在 `music/`。

- 记录来源、许可、采样率、位深、声道、转换命令与目标路径。
- 与当前 BSP 音频路径匹配时优先采用 16 kHz、16 位单声道 PCM。
- 嵌入音频前评估 Flash 与内部 RAM 成本；长录音应流式或分块。
- 无再分发许可不提交媒体文件。

### 生成：狼人杀主持提示语音

| 项 | 值 |
| --- | --- |
| 产物 | `main/ww_voice.c`（生成文件，不要手改） |
| 生成器 | `python3 tools/gen_voice.py`（默认千问 TTS；`--engine say` 或 `--from-dir <录音目录>` 可替换） |
| 来源 | 阿里云百炼 `qwen-audio-3.1-tts-flash`，音色 `xieshurou_v3.1`，由仓库作者生成 |
| 格式 | 16 kHz 单声道 IMA-ADPCM（4 bit），`main/ww_sound.c` 边解码边送 I2S |
| 大小 | 15 句，约 310 KB Flash，不占 RAM |
| 注意 | 用 `--engine say` 生成的不要发布：macOS 系统语音的输出只允许个人、非商业使用。 |
