#include <WiFi.h>
#include <Arduino.h>
#include <WakeOnLan.h>
#include "PubSubClient.h"  // 加载MQTT库文件
#include <WiFiUdp.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <mbedtls/ccm.h>
#include <string.h>
#include <stdio.h>

/*===========================⇩ ⇩ ⇩ 这部分是需要修改的信息 ⇩ ⇩ ⇩================================*/
#define MQTT_ID  "xxxx"  // 巴法云私钥
const char *ssid = "xxxx";  // WiFi名字
const char *password = "xxxx";  // WiFi密码
const char *MACAddress = "xx:xx:xx:xx:xx:xx";  // PC设备的MAC地址
const char* keyHex = "xxxxxxxxxx";  // BLE Key
const char *targetMac = "xx:xx:xx:xx:xx:xx";  // 目标设备的MAC地址
/*===========================⇧ ⇧ ⇧ 这部分是需要修改的信息 ⇧ ⇧ ⇧================================*/


const int MQTT_SERVER_PORT = 9501;  // MQTT服务器端口
const char* MQTT_SERVER = "bemfa.com";  // MQTT服务器地址
long lastMsg = 0;
char msg[50];
String lastPacket = ""; // 存储上次数据包
String nonce, cipherText, tag;
const char* adataHex = "11";
bool isScanning = true;  // 正在扫描标记
unsigned long lastResetTime = 0; // 上次重置时间
unsigned long resetInterval = 3600000; // 每小时重置一次
unsigned long bleScanTimeout = 5000; // BLE扫描超时时间5秒
unsigned long lastScanStartTime = 0; // 上次扫描开始时间
// 心跳包相关配置
const char *udpIP = "192.168.31.157";  // 替换为你的目标 IP 地址
const int udpPort = 63927;  // 目标 UDP 端口
const char *sn = "sn255e8df93p4f2";  // PC序列号
const char *topic = "PC001";  // topi
const char *name = "Esp-32-BLE sd83ud3";  // topi
// 用于心跳检测的变量
unsigned long lastHeartbeatReceived = 0;  // 上次接收心跳的时间
bool heartbeatLost = false;  // 标志心跳信号是否丢失
char *single_function = "开机";
char *double_function = "重启";
char *long_function = "关机";

// 初始化
WiFiUDP UDP;
WiFiUDP udp;
WakeOnLan WOL(UDP);
WiFiClient MQTTclient;
PubSubClient client(MQTTclient);
BLEScan* pBLEScan;  // BLE扫描对象

// 声明开机函数
void turnOn();

// 解密函数声明
String decryptData(const char* keyHex, const char* nonceHex, const char* adataHex, const char* ciphertextHex, const char* tagHex);

// 字节数组转换
void hexStringToByteArray(const char* hexString, unsigned char* byteArray, size_t byteArrayLen) {
  for (size_t i = 0; i < byteArrayLen; i++) {
    sscanf(hexString + 2 * i, "%2hhx", &byteArray[i]);
  }
}

// BLE回调函数
class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    String deviceMac = advertisedDevice.getAddress().toString().c_str();

    if (deviceMac.equalsIgnoreCase(targetMac)) {
      // 获取广播包中的广告数据 (adv packet)
      const uint8_t* advPacket = advertisedDevice.getPayload();  // 获取广播包
      int advPacketLen = advertisedDevice.getPayloadLength();    // 广播包长度
      //Serial.println(advPacketLen);
      // 将广播包转换为十六进制字符串，便于比对和存储
      String packetData = "";
      for (int i = 0; i < advPacketLen; i++) {
        char hex[3];
        sprintf(hex, "%02X", advPacket[i]);
        packetData += hex;
      }
      // 检测广播包
      if (advPacketLen == 49) {
        // 比较当前包与上次储存的包
        if (packetData != lastPacket) {
          Serial.print("New 36-byte packet detected");
          Serial.println(packetData);
          lastPacket = packetData;  // 更新存储的包
          nonce = packetData.substring(24, 36) + packetData.substring(18, 24) + packetData.substring(42, 48); // nonce
          cipherText = packetData.substring(36, 42); // cipherText
          tag = packetData.substring(48, 56); // tag
          String decryptedData = decryptData(keyHex, nonce.c_str(), adataHex, cipherText.c_str(), tag.c_str());
          // 输出解密结果
          if (decryptedData == "0C4A00") {
            Serial.println("clicked");
            turnOn();
            sendSingle();
          } else if (decryptedData == "0D4A00"){
            Serial.println("Double clicked");
            sendDouble();
          } else{
            Serial.println("Long pressed");
            sendLong();
          }
        } else {
        }
      }
      pBLEScan->stop();  // 停止当前的扫描
      isScanning = false;  // 更新扫描状态
    }
  }
};

// MQTT订阅的主题有消息发布时的回调函数
void callback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] ");
  String Mqtt_Buff = "";
  for (int i = 0; i < length; i++) {
    Mqtt_Buff += (char)payload[i];
  }
  Serial.print(Mqtt_Buff);
  Serial.println();

  if (Mqtt_Buff == "on") {  // 如果接收字符on，开机
    turnOn();
  }
  if (Mqtt_Buff == "off") {  // 如果接收字符on，开机
    turnOff();
  }
  Mqtt_Buff = "";
}

// MQTT重连函数
void MQTT_reconnect() {
  if (WiFi.status() != WL_CONNECTED) WIFI_reconnect();
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    if (client.connect(MQTT_ID)) {
      Serial.println("connected");
      client.subscribe("PC001");
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      delay(5000);
    }
  }
}

// WiFi重连函数
void WIFI_reconnect() {
  while (WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
    Serial.println("Connecting to WiFi..");
    delay(5000);
  }
  Serial.print("Connected, IP address: ");
  Serial.println(WiFi.localIP());
}

// 重置函数
void resetSystem() {
    Serial.println("System resetting...");
    ESP.restart();
}

// 心跳包发送函数
void sendHeartbeat() {
  // 构建心跳消息
  String heartbeatMsg = "HB##" + String(name) + "##" + String(keyHex) + "##" +
                        String(MQTT_ID) + "##" + String(MACAddress) + "##" +
                        String(topic) + "##" + String(ssid) + "##" +
                        String(udpIP) + "##" + String(sn);

  // 将字符串转换为 uint8_t 数组并发送
  udp.beginPacket(udpIP, udpPort);
  udp.write((const uint8_t*)heartbeatMsg.c_str(), heartbeatMsg.length());  // 指定长度
  udp.endPacket();

  Serial.print("UDP Heartbeat sent: ");
  Serial.println(heartbeatMsg);  // 输出心跳消息
}

// 单击发送函数
void sendSingle() {
  // 构建消息
  String heartbeatMsg = "CL##" + String(name) + "##" + String("single") + "##" +
                        String("***");

  // 将字符串转换为 uint8_t 数组并发送
  udp.beginPacket(udpIP, udpPort);
  udp.write((const uint8_t*)heartbeatMsg.c_str(), heartbeatMsg.length());  // 指定长度
  udp.endPacket();

  Serial.print("UDP Heartbeat sent: ");
  Serial.println(heartbeatMsg);  // 输出心跳消息
}

// 双击发送函数
void sendDouble() {
  // 构建消息
  String heartbeatMsg = "CL##" + String(name) + "##" + String("double") + "##" +
                        String("***");

  // 将字符串转换为 uint8_t 数组并发送
  udp.beginPacket(udpIP, udpPort);
  udp.write((const uint8_t*)heartbeatMsg.c_str(), heartbeatMsg.length());  // 指定长度
  udp.endPacket();

  Serial.print("UDP Heartbeat sent: ");
  Serial.println(heartbeatMsg);  // 输出心跳消息
}

// 单击发送函数
void sendLong() {
  // 构建消息
  String heartbeatMsg = "CL##" + String(name) + "##" + String("long") + "##" +
                        String("***");

  // 将字符串转换为 uint8_t 数组并发送
  udp.beginPacket(udpIP, udpPort);
  udp.write((const uint8_t*)heartbeatMsg.c_str(), heartbeatMsg.length());  // 指定长度
  udp.endPacket();

  Serial.print("UDP Heartbeat sent: ");
  Serial.println(heartbeatMsg);  // 输出心跳消息
}

void setup() {
  Serial.begin(115200);
  client.setServer(MQTT_SERVER, MQTT_SERVER_PORT);
  client.setCallback(callback);
  WiFi.begin(ssid, password);

  pinMode(2, OUTPUT);
  WOL.setRepeat(3, 100);

  BLEDevice::init("");
  pBLEScan = BLEDevice::getScan();  // 创建BLE扫描对象
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setActiveScan(true);  // 设置主动扫描
  pBLEScan->setInterval(110);  // 设置扫描间隔
  pBLEScan->setWindow(99);     // 设置扫描窗口
}

void loop() {
  // 检查WiFi和MQTT连接
  if (WiFi.status() != WL_CONNECTED) WIFI_reconnect();
  else if (!client.connected()) MQTT_reconnect();
  client.loop();

  // 检查是否达到系统重置时间间隔
  if (millis() - lastResetTime > resetInterval) {
      resetSystem();
  }

  // 每1秒发送UDP心跳包
  static unsigned long lastSendTime = 0;
  if (millis() - lastSendTime > 10000) {
    lastSendTime = millis();
    sendHeartbeat();
  }

  // 接收UDP消息
  int packetSize = udp.parsePacket();
  if (packetSize) {
    char incomingPacket[255];  // 存储接收到的消息
    int len = udp.read(incomingPacket, 255);
    if (len > 0) {
      incomingPacket[len] = 0;  // 确保字符串结尾
    }
    Serial.printf("Received packet: %s\n", incomingPacket);

    // 处理接收到的消息
    // handleReceivedPacket(incomingPacket);
    
    // 更新最后一次接收到心跳的时间
    lastHeartbeatReceived = millis();
    if (heartbeatLost) {
      heartbeatRecovered();
      heartbeatLost = false;
    }
  }

  // 检查是否超过10秒没有接收到心跳
  if (millis() - lastHeartbeatReceived > 12000) {
    if (!heartbeatLost) {
      heartbeatLost = true;
      handleHeartbeatLoss();
    }
  }

  // BLE扫描超时机制
  if (isScanning) {  // 如果正在扫描
      Serial.println("Loop: Starting BLE scan...");
      lastScanStartTime = millis();
      pBLEScan->start(5, nullptr);  // 5秒后自动停止
      isScanning = false;
  } else {
      // 检查扫描是否超时
      if (millis() - lastScanStartTime > bleScanTimeout) {
          Serial.println("BLE scan timeout. Resetting scan...");
           pBLEScan->stop();
          isScanning = true;  // 重启扫描
      }
      Serial.println("Loop: BLE scan complete. Waiting for next trigger.");
      delay(1000);  // 每次等待1秒
      isScanning = true;  // 重启扫描
  }
}

// 处理心跳信号丢失
void handleHeartbeatLoss() {
  Serial.println("Heartbeat signal lost, executing loss action...");
  client.publish("PC001/up", "off");
}

// 处理心跳信号恢复
void heartbeatRecovered() {
  Serial.println("Heartbeat signal recovered, executing recovery action...");
  client.publish("PC001/up", "on");
}

// 打开PC
void turnOn() {
  Serial.println("Turn on PC");
  digitalWrite(2, HIGH);
  delay(200);
  digitalWrite(2, LOW);
  delay(200);
  WOL.sendMagicPacket(MACAddress);
}

// 关闭PC
void turnOff() {
  Serial.println("Turn on PC");
  digitalWrite(2, HIGH);
  delay(200);
  digitalWrite(2, LOW);
  delay(200);
  // 构建消息
  String heartbeatMsg = "SD##" + String(name);

  // 将字符串转换为 uint8_t 数组并发送
  udp.beginPacket(udpIP, udpPort);
  udp.write((const uint8_t*)heartbeatMsg.c_str(), heartbeatMsg.length());  // 指定长度
  udp.endPacket();

  Serial.print("UDP Heartbeat sent: ");
  Serial.println(heartbeatMsg);  // 输出心跳消息
}

// Beacon解密函数
String decryptData(const char* keyHex, const char* nonceHex, const char* adataHex, const char* ciphertextHex, const char* tagHex) {
  // 计算密文长度
  size_t ciphertextLen = strlen(ciphertextHex) / 2;
  unsigned char decryptedOutput[ciphertextLen];
  
  // 准备解密所需的数据
  unsigned char key[16], nonce[12], tag[4], ciphertext[ciphertextLen], adata[1];
  hexStringToByteArray(keyHex, key, sizeof(key));
  hexStringToByteArray(nonceHex, nonce, sizeof(nonce));
  hexStringToByteArray(ciphertextHex, ciphertext, ciphertextLen);
  hexStringToByteArray(tagHex, tag, sizeof(tag));
  hexStringToByteArray(adataHex, adata, sizeof(adata));

  // 初始化 AES-128-CCM 解密上下文
  mbedtls_ccm_context ctx;
  mbedtls_ccm_init(&ctx);

  // 设置 AES 密钥
  int ret = mbedtls_ccm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 128);
  if (ret != 0) {
    mbedtls_ccm_free(&ctx);
    return "";  // 返回空值表示解密失败
  }

  // 执行解密操作
  ret = mbedtls_ccm_auth_decrypt(&ctx, ciphertextLen, nonce, sizeof(nonce), 
                                 adata, sizeof(adata), ciphertext, decryptedOutput, 
                                 tag, sizeof(tag));

  // 清理上下文
  mbedtls_ccm_free(&ctx);

  // 处理解密结果
  if (ret == 0) {
    String result = "";
    for (size_t i = 0; i < ciphertextLen; i++) {
      char buf[3];
      sprintf(buf, "%02X", decryptedOutput[i]);
      result += buf;
    }
    return result;
  } else {
    return "";  // 返回空值表示解密失败
  }
}
