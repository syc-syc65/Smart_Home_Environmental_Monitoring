#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <mbedtls/base64.h>
#include <mbedtls/md.h>
#include <NTPClient.h>
#include <WiFiUdp.h>

// ==================== WiFi配置 ====================
const char* WIFI_SSID = "hello";
const char* WIFI_PASSWORD = "12345678";

// ==================== OneNET Studio配置 ====================
const char* PRODUCT_ID = "K3GGK1Sj7Q";
const char* DEVICE_NAME = "stm";

// MQTT服务器
const char* MQTT_SERVER = "mqtts.heclouds.com";
const int MQTT_PORT = 1883;

// ==================== 全局变量 ====================
WiFiClient espClient;
PubSubClient mqttClient(espClient);
unsigned long lastReportTime = 0;
const long REPORT_INTERVAL = 5000;

// NTP时间同步（使用UTC时间，OneNET会自动转换为北京时间）
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 0, 60000);

// TCP服务器配置 - 支持更稳定的连接
WiFiServer tcpServer(8080);
WiFiClient tcpClient;
bool tcpClientConnected = false;
unsigned long lastTcpHeartbeat = 0;
const long TCP_KEEPALIVE_INTERVAL = 10000; // 10秒发送一次心跳
unsigned long lastClientCheck = 0;
const long CLIENT_CHECK_INTERVAL = 2000; // 2秒检查一次连接状态

// 传感器数据存储
struct SensorData {
  float temperature;
  float humidity;
  int light;
  int smoke;
  bool valid;
} sensorData = {0, 0, 0, 0, false};

// 控制状态
struct ControlState {
  bool fan;
  bool valve;
  bool alarm;
} controlState = {false, false, false};

// 上次上报的数据
struct SensorData lastReportedData = {0, 0, 0, 0, false};
struct ControlState lastReportedControlState = {false, false, false};

// 从STM32收到的最新控制状态
struct ControlState stm32ControlState = {false, false, false};

// 接收缓冲区
char tcpBuffer[128];
int tcpBufferIndex = 0;

// ==================== 连接MQTT ====================
bool connectMQTT() {
  String clientId = String(DEVICE_NAME);
  String token = "version=2018-10-31&res=products%2FK3GGK1Sj7Q%2Fdevices%2Fstm&et=1787733887&method=md5&sign=7pieyIabazlMcHuGGmZ8qQ%3D%3D";
  
  Serial.println("正在连接OneNET Studio...");
  Serial.printf("Client ID: %s\n", clientId.c_str());
  Serial.printf("Username: %s\n", PRODUCT_ID);
  
  if (mqttClient.connect(clientId.c_str(), PRODUCT_ID, token.c_str())) {
    Serial.println("OneNET Studio连接成功!");
    
    String postReplyTopic = "$sys/" + String(PRODUCT_ID) + "/" + String(DEVICE_NAME) + "/thing/property/post/reply";
    mqttClient.subscribe(postReplyTopic.c_str());
    Serial.printf("订阅主题: %s\n", postReplyTopic.c_str());
    
    String setTopic = "$sys/" + String(PRODUCT_ID) + "/" + String(DEVICE_NAME) + "/thing/property/set";
    mqttClient.subscribe(setTopic.c_str());
    Serial.printf("订阅主题: %s\n", setTopic.c_str());
    
    return true;
  } else {
    Serial.printf("连接失败, 状态码: %d\n", mqttClient.state());
    return false;
  }
}

// ==================== 发送控制命令到STM32 ====================
void sendControlToSTM32() {
  if (tcpClientConnected && tcpClient.connected()) {
    char cmd[64];
    sprintf(cmd, "{\"F\":%d,\"V\":%d,\"A\":%d}", 
            controlState.fan ? 1 : 0, 
            controlState.valve ? 1 : 0, 
            controlState.alarm ? 1 : 0);
    tcpClient.println(cmd);
    Serial.printf("发送控制命令到STM32: %s\n", cmd);
  }
}

// ==================== MQTT回调 ====================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.print("收到消息 [");
  Serial.print(topic);
  Serial.print("]: ");
  
  String message = "";
  for (int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  Serial.println(message);
  
  String setTopic = "$sys/" + String(PRODUCT_ID) + "/" + String(DEVICE_NAME) + "/thing/property/set";
  if (String(topic) == setTopic) {
    StaticJsonDocument<512> doc;
    DeserializationError error = deserializeJson(doc, message);
    
    if (!error && doc.containsKey("params")) {
      JsonObject params = doc["params"];
      
      if (params.containsKey("fan")) {
        controlState.fan = params["fan"].as<bool>();
        Serial.printf("设置风扇: %s\n", controlState.fan ? "开" : "关");
      }
      if (params.containsKey("valve")) {
        controlState.valve = params["valve"].as<bool>();
        Serial.printf("设置电磁阀: %s\n", controlState.valve ? "开" : "关");
      }
      if (params.containsKey("alarm")) {
        controlState.alarm = params["alarm"].as<bool>();
        Serial.printf("设置报警器: %s\n", controlState.alarm ? "开" : "关");
      }
      
      sendControlToSTM32();
      delay(100);
      sendControlToSTM32();
      delay(100);
      sendControlToSTM32();
      
      String setReplyTopic = "$sys/" + String(PRODUCT_ID) + "/" + String(DEVICE_NAME) + "/thing/property/set_reply";
      String reply = "{\"id\":\"" + String(doc["id"].as<const char*>()) + "\",\"code\":200,\"msg\":\"success\"}";
      mqttClient.publish(setReplyTopic.c_str(), reply.c_str());
      Serial.printf("发送回复: %s\n", reply.c_str());
    }
  }
}

// ==================== 发送TCP心跳包 ====================
void sendTcpHeartbeat() {
  if (tcpClientConnected && tcpClient.connected()) {
    tcpClient.println("{\"heartbeat\":1}");
    lastTcpHeartbeat = millis();
  }
}

// ==================== 处理TCP客户端连接 ====================
void handleTcpClient() {
  if (tcpClientConnected && !tcpClient.connected()) {
    tcpClient.stop();
    tcpClientConnected = false;
    tcpBufferIndex = 0;
    Serial.println("TCP客户端断开连接");
  }
  
  if (tcpServer.hasClient()) {
    WiFiClient newClient = tcpServer.available();
    
    if (tcpClientConnected) {
      if (!tcpClient.connected()) {
        tcpClient.stop();
        tcpClient = newClient;
        tcpClientConnected = true;
        lastTcpHeartbeat = millis();
        Serial.println("替换旧的TCP连接");
        Serial.printf("新客户端IP: %s\n", tcpClient.remoteIP().toString().c_str());
      } else {
        newClient.stop();
        Serial.println("拒绝新的TCP连接（已有活跃客户端）");
      }
    } else {
      tcpClient = newClient;
      tcpClientConnected = true;
      lastTcpHeartbeat = millis();
      Serial.println("TCP客户端连接成功");
      Serial.printf("客户端IP: %s\n", tcpClient.remoteIP().toString().c_str());
      
      delay(100);
      sendControlToSTM32();
    }
  }
  
  if (tcpClientConnected && tcpClient.connected()) {
    if (millis() - lastTcpHeartbeat > TCP_KEEPALIVE_INTERVAL) {
      sendTcpHeartbeat();
    }
    
    while (tcpClient.available()) {
      char c = tcpClient.read();
      
      if (c == '\n' || c == '\r') {
        if (tcpBufferIndex > 0) {
          tcpBuffer[tcpBufferIndex] = '\0';
          parseSensorData(tcpBuffer);
          tcpBufferIndex = 0;
        }
      } else if (tcpBufferIndex < sizeof(tcpBuffer) - 1) {
        tcpBuffer[tcpBufferIndex++] = c;
      }
    }
  }
}

// ==================== 解析传感器数据 ====================
void parseSensorData(char* data) {
  float t, h;
  int l, s, f, v, a;
  
  if (sscanf(data, "{\"T\":%f,\"H\":%f,\"L\":%d,\"S\":%d,\"F\":%d,\"V\":%d,\"A\":%d}", 
              &t, &h, &l, &s, &f, &v, &a) == 7) {
    sensorData.temperature = t;
    sensorData.humidity = h;
    // 限制光照值范围，防止int32溢出
    if (l < 0) l = 0;
    if (l > 65535) l = 65535;
    sensorData.light = l;
    sensorData.smoke = s;
    stm32ControlState.fan = (f == 1);
    stm32ControlState.valve = (v == 1);
    stm32ControlState.alarm = (a == 1);
    sensorData.valid = true;
    Serial.printf("收到数据: T=%.1f, H=%.1f, L=%d, S=%d, F=%d, V=%d, A=%d\n", t, h, l, s, f, v, a);
  }
}

// ==================== 检查是否应该上报数据 ====================
bool shouldReportData() {
  if (!sensorData.valid) {
    return false;
  }
  
  if (!lastReportedData.valid) {
    return true;
  }
  
  const float TEMP_THRESHOLD = 0.5;
  const float HUMI_THRESHOLD = 1.0;
  const int LIGHT_THRESHOLD = 10;
  const int SMOKE_THRESHOLD = 5;
  
  bool tempChanged = abs(sensorData.temperature - lastReportedData.temperature) >= TEMP_THRESHOLD;
  bool humiChanged = abs(sensorData.humidity - lastReportedData.humidity) >= HUMI_THRESHOLD;
  bool lightChanged = abs(sensorData.light - lastReportedData.light) >= LIGHT_THRESHOLD;
  bool smokeChanged = abs(sensorData.smoke - lastReportedData.smoke) >= SMOKE_THRESHOLD;
  
  bool fanChanged = (stm32ControlState.fan != lastReportedControlState.fan);
  bool valveChanged = (stm32ControlState.valve != lastReportedControlState.valve);
  bool alarmChanged = (stm32ControlState.alarm != lastReportedControlState.alarm);
  
  return tempChanged || humiChanged || lightChanged || smokeChanged || fanChanged || valveChanged || alarmChanged;
}

// ==================== 上报数据 ====================
void reportData() {
  if (!shouldReportData()) {
    return;
  }
  
  StaticJsonDocument<512> doc;
  
  doc["id"] = "123";
  doc["version"] = "1.0";
  doc["msgId"] = PRODUCT_ID;
  
  JsonObject params = doc.createNestedObject("params");
  
  unsigned long epoch = timeClient.getEpochTime();
  unsigned long long timestamp = (unsigned long long)epoch * 1000;
  
  if (sensorData.valid) {
    JsonObject temperature = params.createNestedObject("temperature");
    temperature["value"] = sensorData.temperature;
    temperature["time"] = timestamp;
    
    JsonObject humidity = params.createNestedObject("humidity");
    humidity["value"] = sensorData.humidity;
    humidity["time"] = timestamp;
    
    JsonObject light = params.createNestedObject("light");
    // 限制光照值范围，防止int32溢出
    int lightValue = sensorData.light;
    if (lightValue < 0) lightValue = 0;
    if (lightValue > 65535) lightValue = 65535;
    light["value"] = lightValue;
    light["time"] = timestamp;
    
    JsonObject smoke = params.createNestedObject("smoke");
    smoke["value"] = sensorData.smoke;
    smoke["time"] = timestamp;
  }
  
  bool fanChanged = (stm32ControlState.fan != lastReportedControlState.fan);
  bool valveChanged = (stm32ControlState.valve != lastReportedControlState.valve);
  bool alarmChanged = (stm32ControlState.alarm != lastReportedControlState.alarm);
  
  if (fanChanged || !lastReportedData.valid) {
    JsonObject fan = params.createNestedObject("fan");
    fan["value"] = stm32ControlState.fan;
    fan["time"] = timestamp;
  }
  
  if (valveChanged || !lastReportedData.valid) {
    JsonObject valve = params.createNestedObject("valve");
    valve["value"] = stm32ControlState.valve;
    valve["time"] = timestamp;
  }
  
  if (alarmChanged || !lastReportedData.valid) {
    JsonObject alarm = params.createNestedObject("alarm");
    alarm["value"] = stm32ControlState.alarm;
    alarm["time"] = timestamp;
  }
  
  String jsonString;
  serializeJson(doc, jsonString);
  
  String topic = "$sys/" + String(PRODUCT_ID) + "/" + String(DEVICE_NAME) + "/thing/property/post";
  
  Serial.printf("上报数据: %s\n", jsonString.c_str());
  
  if (mqttClient.publish(topic.c_str(), jsonString.c_str(), false)) {
    Serial.println("数据上报成功!");
    if (sensorData.valid) {
      lastReportedData = sensorData;
    }
    lastReportedControlState = stm32ControlState;
  } else {
    Serial.println("数据上报失败!");
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n========== OneNET Studio MQTT ==========");
  
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("连接WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi连接成功!");
  Serial.printf("IP地址: %s\n", WiFi.localIP().toString().c_str());
  
  tcpServer.begin();
  Serial.printf("TCP服务器已启动，监听端口: %d\n", 8080);
  
  timeClient.begin();
  Serial.print("同步NTP时间");
  for (int i = 0; i < 10; i++) {
    if (timeClient.update()) {
      break;
    }
    Serial.print(".");
    delay(1000);
  }
  Serial.println();
  Serial.printf("当前时间戳: %lu\n", timeClient.getEpochTime());
  
  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setKeepAlive(60);
  mqttClient.setBufferSize(1024);
}

// ==================== 主循环 ====================
void loop() {
  handleTcpClient();
  
  if (!mqttClient.connected()) {
    if (connectMQTT()) {
      lastReportTime = millis();
    } else {
      delay(5000);
      return;
    }
  }
  
  mqttClient.loop();
  
  if (millis() - lastReportTime > REPORT_INTERVAL) {
    reportData();
    lastReportTime = millis();
  }
}
