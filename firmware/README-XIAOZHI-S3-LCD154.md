# 小智兼容郑晨 1.54 寸 Wi‑Fi 板（ESP32-S3 N16R8）

当前分支只支持小智兼容的郑晨 1.54 寸 Wi‑Fi 板：ESP32‑S3、16MB Flash、8MB OPI PSRAM、
240×240 ST7789 SPI 屏和板载 I2S 扬声器。它不适用于 ESP8266 SD2 小电视、M5Stack Core、
Core2、M5Stick 或其他接线不同的开发板。

## 编译与烧录

```sh
cd firmware
pio run -e xiaozhi-s3-lcd154
pio run -e xiaozhi-s3-lcd154 -t upload --upload-port /dev/cu.usbmodem…
pio device monitor -b 115200
```

省略 `--upload-port` 时 PlatformIO 会尝试自动识别串口。`platformio.ini` 已固定此板的
16MB Flash、OPI PSRAM、LittleFS 分区和 ST7789 引脚；不要沿用 M5Stack 或 ESP8266 的环境名及
引脚配置。

固件使用 16MB 分区表：两套各约 6.25MB 的 OTA 应用分区，以及约 3.44MB LittleFS 分区，
用于保存桥接地址、亮度、提示音音量、额度显示方式和上传的 GIF 桌宠。

## 更新完成提示音

提示音源文件是 `assets/construction_complete.wav`（22.05 kHz、16-bit、单声道）。修改它后，
在仓库根目录运行以下命令重新生成固件使用的 PCM 头文件，再正常编译烧录：

```sh
python3 tools/convert_completion_sound.py \
  firmware/assets/construction_complete.wav \
  firmware/include/audio/construction_complete_pcm.h
```

转换会把音频峰值规范到 92%，并以成对平均方式降采样到稳定的 11.025 kHz；不要直接编辑
`include/audio/construction_complete_pcm.h`。

完成提示音音量可在设备网页或桌面桥接的滑块中设为 0–100%，并保存到 LittleFS；默认值为 72%。
