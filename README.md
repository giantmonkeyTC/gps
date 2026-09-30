# GR III BLE GPS

给 RICOH GR III / GR IIIx 用的外挂 GPS 模块：ESP32-C3 通过蓝牙（BLE）把 GPS 位置写进相机，相机拍照时自己把坐标写进照片 EXIF。目标是"无感"：打开相机正常拍照，模块自动重连，不需要手机。

当前阶段：**功能验证**。背景调研、方案取舍、协议细节、采购清单和外观方向见 [docs/conversation-notes.md](docs/conversation-notes.md)。

## 硬件（验证阶段）

| 物品 | 型号 |
| --- | --- |
| 主控 | ESP32-C3 SuperMini（已焊排针） |
| GPS | ATGM336H-5N 带陶瓷天线（已焊排针），或 u-blox M10 迷你模块 |
| 连线 | 母对母杜邦线 4 根 |
| 数据线 | 能传数据的 USB-C 线 |

接线：

| GPS 模块 | ESP32-C3 SuperMini |
| --- | --- |
| VCC | 3V3 |
| GND | GND |
| TX | GPIO20 |
| RX | GPIO21（可不接） |

## 编译和烧录

用 Cursor / VS Code 的 PlatformIO 插件打开本目录，或者命令行：

```bash
pip install platformio
pio run                 # 编译
pio run -t upload       # 烧录
pio device monitor      # 串口监视器，115200
```

- macOS 不需要驱动，插上后设备名类似 `/dev/cu.usbmodem1101`。
- Linux 需要先 `sudo usermod -aG dialout $USER` 并重新登录，设备名一般是 `/dev/ttyACM0`。
- 烧录连不上时：按住 BOOT，按一下 RESET，松开 BOOT，再烧录。

## 配置

在 `src/main.cpp` 顶部：

| 常量 | 默认值 | 说明 |
| --- | --- | --- |
| `DEFAULT_USE_GPS` | `false` | 开机时的位置来源；`false` 为固定坐标，用于先验证蓝牙链路 |
| `FIXED_LAT` / `FIXED_LON` / `FIXED_ALT` | 上海人民广场 | 固定坐标模式写入的位置 |
| `TZ_OFFSET_MINUTES` | `480` | 本地时区（UTC+8），用于换算相机本地时间 |
| `SYNC_CAMERA_CLOCK_FROM_GPS` | `true` | GPS 模式下每次连接用 GPS 时间校准相机时钟 |
| `WRITE_INTERVAL_MS` | `5000` | 写入位置的间隔 |

## 串口命令

| 命令 | 作用 |
| --- | --- |
| `p` | 进入配对模式 |
| `x` | 清除配对记录 |
| `g` | 切换位置来源：固定坐标 / GPS 模块 |
| `w` | 立即写一次位置 |
| `s` | 打印状态 |
| `h` | 帮助 |

## 验证步骤

0. **不花钱先验证（可选）**：相机固件升级到 1.10 以上；有 Android 13+ 手机的话装 [CameraSync](https://github.com/rock3r/CameraSync) 配对 GR III，拍照看 EXIF 有没有坐标。
1. **配对 + 固定坐标**（不接 GPS）
   - 相机：`Bluetooth Setting > Action Mode` 设为 `On when power is on`；`Smartphone Link > Store Location Info` 设为 `On`。
   - 相机：`Bluetooth Setting > Pairing > Execute Pairing`。
   - 串口按 `p`。串口会打印配对码：
     - 如果是"数字比较码"，核对和相机屏幕一致后在相机上确认；
     - 如果提示输入认证码，就在串口输入相机屏幕上的 6 位数字。
   - 看到 `[write] 固定 ... => OK` 后拍一张照片，检查坐标：
     ```bash
     exiftool -gps:all -n R0000123.JPG
     ```
     应该是 `31.230416, 121.473701` 附近。
2. **接 GPS 模块**：放窗边或室外，串口按 `g` 切到 GPS，`s` 看卫星数和定位；定位后拍照，坐标应该是你所在的位置。
3. **无感体验**：相机关机再开机，看串口是否自动 `[scan] 发现相机 → [ble] 已连接`，记下重连耗时，检查开机后第一张照片有没有坐标。

## 已知不确定点

- 协议来自社区逆向（CameraSync 在 GR IIIx 上实测），GR III 理论相同但未在本项目实测。
- 首次配对是"数字比较"还是"输入认证码"取决于相机，两种都已处理。
- 相机可能只保存一个配对设备，ESP32 配对后手机 App 可能需要重新配对。
- 如果相机用随机蓝牙地址，重连靠广播名匹配（配对时记住的名字）。
