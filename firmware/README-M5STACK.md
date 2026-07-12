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
