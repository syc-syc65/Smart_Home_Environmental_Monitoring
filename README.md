# 智能家居环境监测系统

基于 **STM32F103 + ESP8266 + 中移 OneNET Studio 云平台**的家庭环境监测与远程控制系统。采集温湿度、光照、烟雾等环境参数上云，支持云平台远程下发风扇、阀门、报警器控制指令，并配套 Web 实时数据看板。

## 系统架构

```
┌──────────────┐   串口/TCP:8080 JSON    ┌──────────────┐  MQTT 1883   ┌──────────────┐
│  STM32F103   │ ◄────────────────────▶ │   ESP8266    │ ◀──────────▶ │  OneNET      │
│  从机/主机    │   {"F":1,"V":0,"A":0}  │  (Arduino)   │  属性上报/   │  Studio 云   │
│  传感器采集   │                        │  WiFi 联网    │  指令下发    │              │
│  继电器控制   │                        └──────────────┘              └──────┬───────┘
└──────────────┘                                                            │ HTTPS
┌──────────────┐                                                            ▼
│ DHT22 温湿度 │                                                 ┌──────────────────┐
│ BH1750 光照  │                                                 │  Web 数据看板     │
│ MQ-2  烟雾   │                                                 │ Chart.js / Vue3  │
└──────────────┘                                                 └──────────────────┘
```

## 功能特性

- **环境采集**：温度、湿度、光照强度、烟雾/燃气浓度，每 5 秒上报云端一次
- **远程控制**：OneNET 平台下发指令，经 ESP8266 转发至 STM32，控制风扇、电磁阀、报警器三路继电器
- **断连守护**：ESP8266 同时开启 TCP Server（8080），2 秒检测一次 STM32 连接、10 秒心跳保活
- **NTP 授时**：通过 `pool.ntp.org` 同步 UTC 时间（OneNET 自动转北京时间）
- **可视化看板**：提供 Chart.js 轻量看板与 Vue3 + Element Plus 完整后台两个版本，均可直接在浏览器打开
- **数据回调**：订阅 OneNET 属性设置/上报回复 Topic，指令到达实时生效

## 硬件组成

| 模块 | 型号 | 作用 |
|------|------|------|
| 主控 | STM32F103 | 传感器读取、边缘决策、继电器控制（Keil 工程） |
| WiFi 模块 | ESP8266 | MQTT 上云、与 STM32 TCP 透传（Arduino 固件） |
| 温湿度 | DHT22 | ±0.5°C 精度 |
| 光照 | BH1750 | I²C 数字光强度 |
| 烟雾/燃气 | MQ-2 | 可燃气体/烟雾浓度 |
| 执行器 | 继电器 ×3 | 风扇、电磁阀、报警器 |

**关键引脚分配**（主机，详见设计文档）：ESP8266 通信 `PA2/PA3`，OLED `PB8/PB9`，风扇 `PB0`、灯光 `PB1`、报警器 `PB5`、电磁阀 `PB6`，语音模块 `PB7`。

## 云端配置（OneNET Studio）

固件内预置了产品与设备信息，复用时需替换为自己的：

| 参数 | 固件中的值 | 位置 |
|------|-----------|------|
| WiFi 名称/密码 | `hello` / `12345678` | `.ino` 顶部 `WIFI_SSID`、`WIFI_PASSWORD` |
| Product ID | `K3GGK1Sj7Q` | `PRODUCT_ID` |
| Device Name | `stm` | `DEVICE_NAME` |
| MQTT 地址 | `mqtts.heclouds.com:1883` | `MQTT_SERVER` / `MQTT_PORT` |
| Token | MD5 签名 token（有有效期） | `connectMQTT()` 内，过期需在 OneNET 重新生成 |

物模型需定义属性：`temperature`、`humidity`、`light`、`smoke`，以及 `fan`、`valve`、`alarm` 三个可设置属性。

## 软件目录

```
├── onenet_mqtt(1).ino                # ESP8266 Arduino 固件（MQTT 上云 + TCP 透传）
├── Project.uvprojx / .uvoptx         # Keil 主机工程（STM32 主控）
├── Project_Receiver.uvprojx / .uvoptx# Keil 从机工程（传感器节点）
├── dashboard.html                    # 实时数据看板（Chart.js，单文件）
├── iot-dashboard.html                # 后台管理系统（Vue3+ElementPlus+ECharts CDN 单文件）
├── index.html / index2 .html         # 早期页面版本
├── package.json / vite.config.js     # Vite 工程配置（Vue3 技术栈）
├── 智能家居环境监测系统设计方案.md      # 完整设计文档（架构/引脚/算法）
├── onenet汇总.txt                    # 开发笔记
└── README.md
```

## 快速开始

### 1. 烧录 ESP8266 固件

使用 Arduino IDE（安装 ESP8266 开发板支持包），需安装库：

- `PubSubClient`（MQTT）
- `ArduinoJson`
- `NTPClient`

修改 `.ino` 中的 WiFi 与 OneNET 参数后烧录，串口监视器可观察连接与上报日志。

### 2. 编译 STM32 工程

使用 Keil uVision5 分别打开 `Project.uvprojx`（主机）与 `Project_Receiver.uvprojx`（从机）编译下载。

### 3. 打开 Web 看板

- **快速体验**：直接用浏览器打开 `dashboard.html`（Chart.js 走 CDN）或 `iot-dashboard.html`（Vue3 全家桶走 CDN，无需构建）
- **Vite 开发模式**：仓库中 `package.json` 已声明 vue / vue-router / pinia / element-plus / echarts / vite 依赖；由于 `src/` 源码目录未包含在当前仓库中，`npm run dev` 需配合本地 Vue 工程源码使用

```bash
npm install
npm run dev      # http://localhost:3000
npm run build
```

## 通信协议（ESP8266 ↔ STM32）

TCP 端口 8080，每行一条 JSON 指令：

```json
{"F":1,"V":0,"A":0}
```

| 字段 | 含义 | 0 / 1 |
|------|------|-------|
| `F` | Fan 风扇 | 关 / 开 |
| `V` | Valve 电磁阀 | 关 / 开 |
| `A` | Alarm 报警器 | 关 / 开 |

## 设计亮点

- **云边协同**：云端负责人机交互与数据存储，STM32 本地负责实时采集与控制，断网时本地控制不中断
- **舒适度算法**：设计文档中定义了温湿度加权的 THI 舒适度指数（理想 22–26°C / 40–60%），可在 STM32 端本地决策
- **可扩展架构**：主机 + 多从机通过串口总线组网，支持客厅/卧室/厨房多节点部署

> 更多细节（完整引脚表、从机设计、OTA、语音联动规划等）见仓库内《智能家居环境监测系统设计方案.md》。
