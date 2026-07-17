# M5Stack Core Basic / Gray（ESP32 16MB）

本分支 `m5stack-core-esp32-16mb` 面向原版 M5Stack Core Basic / Gray：ESP32、16MB Flash、
内置 320×240 ILI9341 屏幕。它不适用于 Core2（触摸屏型号）或 M5Stick 系列。

## 编译与烧录

```sh
cd firmware
~/.platformio/penv/bin/pio run -t upload
~/.platformio/penv/bin/pio device monitor
```

如需手动指定串口，在命令末尾增加 `--upload-port /dev/cu.usbserial-...`。

固件使用 16MB 分区表：两套各约 6.25MB 的 OTA 应用分区，以及约 3.44MB LittleFS 分区，
用于保存桥接地址、亮度设置和上传的 GIF 桌宠。

## 更新工作开始与完成提示音

工作开始时播放 `assets/building.wav`，任务完成时播放
`assets/construction_complete.wav`。两个源文件都必须是 22.05 kHz、16-bit、单声道 PCM WAV。
修改后，在仓库根目录运行以下命令重新生成固件使用的 PCM 头文件，再正常编译烧录：

```sh
python3 tools/convert_completion_sound.py \
  firmware/assets/construction_complete.wav \
  firmware/include/audio/construction_complete_pcm.h
python3 tools/convert_completion_sound.py \
  firmware/assets/building.wav \
  firmware/include/audio/building_pcm.h \
  --symbol building_pcm
```

转换会把音频峰值规范到 92%，并以成对平均方式降采样到稳定的 11.025 kHz；不要直接编辑
`include/audio/construction_complete_pcm.h`。

提示音音量可在设备网页或桌面桥接的滑块中设为 0–100%，并保存到 LittleFS；该设置同时作用于
工作开始和完成提示音，默认值为 72%。
