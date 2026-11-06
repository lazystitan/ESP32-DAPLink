# 通过 ESP32-S31 USBIP 探针读取目标芯片 RTT 日志

本文档记录在 ESP32-S31-Korvo-1 板子上，通过 USBIP 无线方式，用 OpenOCD
连接目标 STM32F4（实际为极海 APM32F402）芯片并读取 SEGGER RTT 日志的完整
流程、踩过的坑和原因。

## 背景与结论

- ESP32-DAPLink 原生支持两种对外暴露调试探针的方式：USB Device（HID/Bulk
  CMSIS-DAP）和 USBIP（WiFi）。
- **本板子（ESP32-S31-Korvo-1）的 USB Type-A 口硬件上是 Host-only 设计**
  （TPS2051C 只对外供电，无防倒灌保护），不能接电脑当 Device 用，USB 直连
  探针这条路在硬件层面走不通，必须用 USBIP。
- macOS 没有内核级 usbip 客户端（缺 `vhci-hcd`），必须借助 Linux 环境
  （本文用 Colima + QEMU 建的一个独立 VM）。
- RTT 本身不依赖 J-Link，任何能做 SWD 内存读写的探针（包括 CMSIS-DAP）都
  能通过 OpenOCD/pyOCD 等工具读 RTT。

## 整体链路

```
STM32F4 目标芯片 (RTT 日志)
  ↕ SWD (GPIO8=SWDIO, GPIO10=SWCLK, GND, 3.3V)
ESP32-S31 探针 (CMSIS-DAP + USBIP server, 监听 TCP 3240)
  ↕ WiFi
Colima Linux VM "esp32dbg" (usbip client + OpenOCD)
  ↕ nc 127.0.0.1:9090 (RTT channel 0)
Mac 本机
```

## 前置条件：ESP32-S31 固件配置

### 1. Target 与分区表

`esp32s31` 是 preview target，需要 `idf.py --preview set-target esp32s31`。

`sdkconfig.defaults.esp32s31` 关键内容：

```
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions-16m.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions-16m.csv"
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y

# USB_DEBUG_PROBE 故意不开：这块板子的 USB Type-A 口是 Host-only，
# 接电脑有 VBUS 倒灌风险（见下方"USB Device 模式为何不可行"）。

# Dedicated GPIO 的寄存器级 bit-banging（debug_gpio_dedic.c）是照着
# ESP32-S3 的 GPIO/GPIO_SD 寄存器布局写的，从没在 S31 上验证过。
# 实测 SWD 连接成功率只有 ~1/10（cannot read IDR）。换成标准
# gpio_set_level()/gpio_get_level() 的 Regular GPIO 后连接基本稳定。
CONFIG_DEBUG_PROBE_USE_REGULAR_GPIO=y
```

`main/idf_component.yml` 需要把 `espressif/tinyusb` / `espressif/esp_tinyusb`
的版本约束放宽到能覆盖已支持 S31 的版本（`0.21.0~3` / `3.0.0` 起），target
规则加上 `esp32s31`——即使最终没启用 USB_DEBUG_PROBE，这个约束放宽本身是
为了让依赖求解不报错；不想要 USB Device 功能的话也可以保持 target 规则不
含 esp32s31，二者都可以编译通过。

### 2. SWD 引脚（Kconfig 默认值）

| 信号 | GPIO |
|---|---|
| SWDIO | 8 |
| SWCLK | 10 |

### 3. USB Device 模式为何不可行（已验证，不要再尝试）

- ESP32-S31-Korvo-1 的原理图确认：USB Type-A 口的 USB_DP/USB_DM 直连芯片
  原生 USB_DP/USB_DM（只有 ESD 保护二极管，无串联匹配电阻），VBUS 由
  TPS2051C 限流后输出给外部设备，**VBUS 和 FLG（过流检测）都没有接回芯片
  监测**，没有任何防倒灌设计。
- 把这个口接到电脑/充电头，对方输出的 5V 会通过 TPS2051C 内部体二极管倒灌
  进板子自己的 VCC_5V 轨，有损坏风险。**不要把这块板子的 USB Type-A 口接电
  脑。**
- 如果以后换一块真正支持 Device 模式的板子，`main/main.cpp` 已经按
  esp_tinyusb 3.0 新 API（`tinyusb_driver_install` 用 `tinyusb_config_t`
  嵌套 `descriptor` 字段，CDC 用 `tinyusb_cdcacm_init`，MSC 用
  `tinyusb_msc_install_driver` + `tinyusb_msc_new_storage_spiflash`）完成
  了迁移，代码保留在仓库里，只需把 `USB_DEBUG_PROBE` 配置打开即可复用。

## WiFi 配网

- 首次开机或 NVS 无 WiFi 配置时，板子开热点 `DAPLink-XXXX`（XXXX = MAC 后
  两字节），无密码。
- 连上热点后访问 `http://192.168.4.1/settings`，POST `/api/wifi-set`
  （JSON `{"ssid":..., "password":...}`）写入 NVS 后自动重启。
- **已修复**：原固件 `WIFI_EVENT_STA_DISCONNECTED` 处理没有重试上限，连接
  失败会永久重试、永不回退到配网模式。现在 `main/main.cpp` 加了
  `WIFI_STA_MAXIMUM_RETRY = 10`，超过后 `esp_wifi_stop()` +
  `esp_wifi_deinit()` + 重新 `wifi_init_softap()`，并打印
  `disconnected, reason=<code>`（如 201 = `WIFI_REASON_NO_AP_FOUND`，常见
  原因是 SSID 打错）方便定位。
- 手动清空配网重来：`python3 -m esptool --chip esp32s31 -p <串口>
  erase-region 0x9000 0xA000`（NVS 分区，不影响 app 固件）。

## Colima VM 搭建（一次性）

**不要**在已有工作环境的 default profile 上改动（里面可能有其他项目的
Docker 镜像）。用独立 profile：

```bash
# 建一个独立的、桥接到 WiFi 的 Colima 实例
colima start esp32dbg --vm-type qemu --network-address --network-mode bridged \
  --cpu 2 --memory 2 --disk 15 --runtime containerd
```

注意事项（踩过的坑）：

- `vm-type` 和 `network-mode` 都是**只在实例首次创建时生效**，之后用
  `colima start` 加参数或者改 `~/.colima/<profile>/colima.yaml` 都不会更新
  已存在的实例（会提示 `cannot be updated after initial setup, discarded`）。
  改了要生效必须 `colima delete <profile>` 删了重建。
- `--network-mode bridged` 在公司/企业 WiFi 下可能因为路由器的 DHCP/MAC
  白名单策略导致桥接出来的虚拟网卡拿不到 IPv4（`networkctl status` 显示
  `degraded (configuring)`，只有 IPv6 link-local）。**换成手机热点后立刻
  正常**，所以这是网络侧策略问题，不是 Colima 的 bug。
- `--network-address` 首次执行需要 sudo 授权配置 `vmnet`，必须在真实终端
  （非自动化脚本）里跑一次输入密码，之后才会写入 `/etc/sudoers.d/`。

VM 内安装工具：

```bash
colima ssh --profile esp32dbg
sudo apt-get update
sudo apt-get install -y linux-tools-$(uname -r) usbutils openocd netcat-openbsd
sudo apt-get install -y linux-modules-extra-$(uname -r)   # 关键！vhci-hcd 在这个包里
sudo modprobe vhci-hcd
```

**最容易漏的坑**：Ubuntu 24.04 cloud image 的基础 `linux-image-*-generic`
不含 `vhci-hcd`，它被打包在单独的 `linux-modules-extra-<kernel 版本>` 里，
必须额外装。`linux-tools-generic` 这个 meta 包有时会要求比当前内核更新的
版本号（`Depends: linux-tools-X but it is not installable`），直接装具体
版本号的包（`linux-tools-<uname -r>`）即可绕开。

## 每次使用：连接 USBIP + 跑 OpenOCD + 读 RTT

### 1. 确认板子 IP、USBIP 可达

```bash
# 板子串口日志里会打印 got ip:x.x.x.x
colima ssh --profile esp32dbg -- sudo usbip list -r <板子IP>
```

### 2. Attach 设备

```bash
colima ssh --profile esp32dbg -- sudo modprobe vhci-hcd   # VM 重启/长时间空闲后模块可能需要重新加载
colima ssh --profile esp32dbg -- sudo usbip attach -r <板子IP> -b 2-2   # 2-2 = Bulk CMSIS-DAP
colima ssh --profile esp32dbg -- sudo usbip port          # 确认 "Port in Use"
```

`2-1` 是 HID CMSIS-DAP，`2-2` 是 Bulk CMSIS-DAP（更稳定，OpenOCD 用这个）。
`Attach Request ... failed - Device busy (exported)` 这条报错在 `usbip
port` 显示已挂载时可以忽略，是误报。

### 3. OpenOCD 配置文件（`/tmp/openocd_rtt.cfg`，本机和 VM 内各放一份）

```tcl
source [find interface/cmsis-dap.cfg]
transport select swd

source [find target/stm32f4x.cfg]

adapter speed 500

init
reset run

# 直接给精确地址，不要用自动扫描（见下方"关键坑"）
rtt setup 0x20006584 0x48 "SEGGER RTT"
rtt start
rtt server start 9090 0
```

传到 VM：
```bash
cat /tmp/openocd_rtt.cfg | colima ssh --profile esp32dbg -- sh -c 'cat > /tmp/openocd_rtt.cfg'
```

### 4. 启动 OpenOCD（持续占用，开一个窗口跑着）

```bash
colima ssh --profile esp32dbg
sudo openocd -f /tmp/openocd_rtt.cfg
```

看到以下内容说明成功：
```
Info : SWD DPIDR 0x2ba01477
Info : [stm32f4x.cpu] Cortex-M4 r0p1 processor detected
Info : rtt: Control block found at 0x20006584
Info : Listening on port 9090 for rtt connections
```

### 5. 另开一个终端读日志

```bash
colima ssh --profile esp32dbg
nc 127.0.0.1 9090
```

保存到文件（在本机跑，不是在 VM 里）：
```bash
colima ssh --profile esp32dbg -- timeout 15 nc 127.0.0.1 9090 2>/dev/null > /tmp/rtt_log.txt
```
注意 `2>/dev/null` 不能省，否则 `timeout` 的 exit status 信息会混进文件。

## 关键坑：`rtt setup` 自动扫描在 USBIP 链路下会卡死

症状：`rtt setup 0x20000000 0x8000 "SEGGER RTT"`（32KB 主 SRAM 全范围自动
扫描）会卡在 `rtt: Searching for control block 'SEGGER RTT'` 超过 60 秒不
返回，无论目标是 `reset run` 还是 `reset halt` 都一样。

根因排查过程：
1. 一度怀疑是目标 CPU 执行 `__WFI()` 睡眠导致调试访问窗口异常——用 `reset
   halt` 排除：halt 后扫描依然卡死，排除此假设。
2. 用 `mdw 0x20006584 4` 直接读内存（不走 `rtt setup` 的扫描逻辑），立刻拿
   到 `47474553 52205245 00005454 00000000`，解出来正是 `"SEGGER RTT\0"`
   ——**证明目标固件早就正确初始化了 RTT 控制块，问题出在 OpenOCD
   的自动扫描机制本身**，大范围内存扫描叠加 USBIP 的网络往返延迟导致单字
   节级别的重复读取开销被放大到不可接受。
3. 解法：从目标固件的 `.map` 文件里查到 `_SEGGER_RTT` 符号的精确地址
   （本例是 `0x20006584`，大小 72 字节 = `0x48`），直接把这个地址喂给
   `rtt setup`，扫描范围从 32KB 缩小到 72 字节，几乎瞬间完成。

**如果换了目标芯片/固件，需要重新从它的 `.map` 文件（或者用支持同类
link map 的工具）查 `_SEGGER_RTT` 符号的地址，不要依赖自动扫描。**

## 其他踩过的坑（简记）

- **`adapter speed` 要写在 `source [find target/...]` 之后**，否则会被
  target 配置文件里的默认值覆盖（`stm32f4x.cfg` 自己设了
  `adapter speed 2000`）。
- **`reset halt` 会让目标 CPU 停止执行**（这块板子上接的 LED 会停止闪烁），
  是预期行为不是故障；不想打断目标程序运行用 `reset run`。
- 杀 OpenOCD 进程要用 `colima ssh --profile esp32dbg -- sudo pkill -9 -f
  openocd`，本机 `kill` SSH 客户端进程不会杀掉远端真正的 openocd 进程，
  残留进程会导致下次启动报 `could not claim interface: Resource busy`。
- USBIP 的 attach 在板子重启、WiFi 断线重连后会失效，需要重新
  `usbip attach`；`vhci-hcd` 内核模块在 VM 长时间空闲后似乎会被卸载，需要
  重新 `modprobe`。
- 用公司/企业 WiFi 做桥接网络给 VM 用，在 DHCP 层面可能被拦截（未知 MAC
  地址得不到 IP），换手机热点验证是网络策略问题还是配置问题。

## 验证结果

成功读到的 RTT 日志示例（节选）：
```
Time:162858->[TP] Send: ID=0x505, DLC=8, Data=10 10 04 E7 0C 00 00 00
Time:162860->CAN send failed after 10 retries
Time:162860->SendFirstFrame failed
Time:162860->[TP] Send failed, re-enqueuing...
```

包含传感器初始化（QMI8658 IMU）、CAN 总线 TP 层传输日志等真实运行时信息，
证明整条 USBIP + OpenOCD + RTT 链路可用。
