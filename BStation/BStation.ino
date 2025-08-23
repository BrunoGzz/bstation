#include <LoRa.h>
#include <SPI.h>
#include <DHT.h>
#include <WiFi.h>
#include "esp_camera.h"
#include "esp_wifi.h"
#include "esp_bt.h"
#include <vector>
#include <CRC8.h>

#define uS_TO_S_FACTOR 1000000ULL
RTC_DATA_ATTR int bootCount = 0;
RTC_DATA_ATTR int sleepTimeSec = 10;
RTC_DATA_ATTR framesize_t imageResolution = FRAMESIZE_QVGA;
RTC_DATA_ATTR int imageQuality = 15;
RTC_DATA_ATTR int TXP = 2;
RTC_DATA_ATTR int SF = 7;
RTC_DATA_ATTR uint32_t BW = 125E3;
RTC_DATA_ATTR int CR = 8;
RTC_DATA_ATTR int PL = 8;
RTC_DATA_ATTR unsigned long loraRxTimeout = 5000;

#define CAMERA_MODEL_AI_THINKER
#include "camera_pins.h"

// Pines LoRa para SX1278 en ESP32-CAM
#define LORA_SS 2
#define LORA_RST -1
#define LORA_DIO0 12
#define LORA_SCK 14
#define LORA_MISO 15
#define LORA_MOSI 13

// Desactiva WiFi y BT
void disableWiFiBT() {
  WiFi.mode(WIFI_OFF);
  btStop();
  esp_bt_controller_disable();
  esp_wifi_stop();
}

// Enviar mensaje LoRa
void loraTransmit(String message) {
  LoRa.beginPacket();
  LoRa.print(message);
  LoRa.endPacket();
  Serial.println(message);
}

// Recibir mensaje LoRa
bool loraReceive(String& incoming, unsigned long timeout = 5000) {
  unsigned long start = millis();
  while ((millis() - start) < timeout) {
    int packetSize = LoRa.parsePacket();
    if (packetSize) {
      incoming = LoRa.readString();
      return true;
    }
    delay(10);
  }
  return false;
}

// Variantes de sendOk
void sendOk(const String& value = "") {
  String message = "BS;OK;";
  if (value != "") message += value + ";";
  loraTransmit(message);
}
void sendOk(int value) {
  sendOk(String(value));
}
void sendOk(float value) {
  sendOk(String(value));
}
void sendOk(const char* value) {
  sendOk(String(value));
}

void flashCamera(int times) {
  const int flashPin = 4;
  pinMode(flashPin, OUTPUT);
  for (int i = 0; i < times; i++) {
    digitalWrite(flashPin, HIGH);
    delay(250);
    digitalWrite(flashPin, LOW);
    delay(250);
  }
  delay(800);
}

framesize_t parseResolution(String res) {
  if (res == "UXGA") return FRAMESIZE_UXGA;
  else if (res == "SVGA") return FRAMESIZE_SVGA;
  else if (res == "VGA") return FRAMESIZE_VGA;
  else if (res == "CIF") return FRAMESIZE_CIF;
  else if (res == "QVGA") return FRAMESIZE_QVGA;
  else if (res == "QQVGA") return FRAMESIZE_QQVGA;
  else return FRAMESIZE_QVGA;
}

void sendImage(camera_fb_t* fb) {
  const int maxPacketSize = 50;
  int totalLen = fb->len;
  uint8_t* imgData = fb->buf;
  int numPackets = (totalLen + maxPacketSize - 1) / maxPacketSize;

  Serial.printf("Enviando imagen en %d fragmentos...\n", numPackets);
  String header = "IMG;" + String(totalLen) + ";" + String(numPackets);
  loraTransmit(header);
  delay(100);

  std::vector<std::vector<uint8_t>> packets(numPackets);
  CRC8 crc;

  for (int i = 0; i < numPackets; i++) {
    int startIdx = i * maxPacketSize;
    int thisSize = min(maxPacketSize, totalLen - startIdx);
    String packetHeader = "P:" + String(i) + ":";
    int hLen = packetHeader.length();

    std::vector<uint8_t> buffer(hLen + thisSize + 1);
    memcpy(buffer.data(), packetHeader.c_str(), hLen);
    memcpy(buffer.data() + hLen, imgData + startIdx, thisSize);

    crc.restart();
    for (int j = 0; j < hLen + thisSize; j++) {
      crc.add(buffer[j]);
    }
    buffer[hLen + thisSize] = crc.calc();
    packets[i] = buffer;

    LoRa.beginPacket();
    LoRa.write(buffer.data(), buffer.size());
    LoRa.endPacket();
    for (size_t i = 0; i < buffer.size(); ++i) {
      if (buffer[i] < 16) Serial.print("0");
      Serial.print(buffer[i], HEX);
      Serial.print(" ");
    }
    Serial.println();
    delay(100);
  }

  String incoming;
  while (true) {
    if (loraReceive(incoming, 3000)) {
      incoming.trim();
      incoming = incoming.substring(3);
      if (incoming == "END_I") {
        Serial.println("Transmisión de imagen finalizada");
        sendOk("END_I");
        break;
      } else if (incoming.startsWith("RETRY:")) {
        String retryList = incoming.substring(6);
        while (retryList.length()) {
          int idx = retryList.indexOf(',');
          String token = (idx == -1) ? retryList : retryList.substring(0, idx);
          int pkt = token.toInt();
          if (pkt >= 0 && pkt < numPackets) {
            auto& buffer = packets[pkt];
            LoRa.beginPacket();
            LoRa.write(buffer.data(), buffer.size());
            LoRa.endPacket();
            Serial.printf("Reenviado fragmento %d\n", pkt);
            delay(100);
          }
          if (idx == -1) break;
          retryList = retryList.substring(idx + 1);
        }
      }
    } else {
      Serial.println("Timeout esperando comandos de retransmisión.");
      sendOk("END_I");
      break;
    }
  }
}

void setup() {
  Serial.begin(115200);
  disableWiFiBT();
  ++bootCount;
  Serial.println("Welcome! FTU #" + String(bootCount));

  esp_reset_reason_t reason = esp_reset_reason();
  Serial.print("Reset reason: ");
  Serial.println(reason);
  if (reason == ESP_RST_PANIC) {
    Serial.println("Reinicio debido a pánico. Esperando 5s para evitar bucle.");
    delay(5000);
  }

  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.frame_size = imageResolution;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location = CAMERA_FB_IN_DRAM;
  config.jpeg_quality = imageQuality;
  config.fb_count = 1;

  Serial.println("CAM CONFIG DONE");

  if (!psramFound()) {
    Serial.println("PSRAM NO detectada");
  }

  if (config.pixel_format == PIXFORMAT_JPEG) {
    if (psramFound()) {
      config.jpeg_quality = 10;
      config.fb_count = 2;
      config.grab_mode = CAMERA_GRAB_LATEST;
    } else {
      config.fb_location = CAMERA_FB_IN_DRAM;
    }
  } else {
    config.frame_size = FRAMESIZE_240X240;
  }

  Serial.println("CAM2 CONFIG DONE");

  if (esp_camera_init(&config) != ESP_OK) {
    Serial.println("Error iniciando la cámara");
    esp_sleep_enable_timer_wakeup(sleepTimeSec * uS_TO_S_FACTOR);
    esp_deep_sleep_start();
  }

  sensor_t* s = esp_camera_sensor_get();
  if (config.pixel_format == PIXFORMAT_JPEG) {
    s->set_framesize(s, imageResolution);
  }

  Serial.println("CAM3 CONFIG DONE");


  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_SS);
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  if (!LoRa.begin(433E6)) {
    Serial.println("Fallo al iniciar LoRa");
    esp_sleep_enable_timer_wakeup(sleepTimeSec * uS_TO_S_FACTOR);
    esp_deep_sleep_start();
  }

  LoRa.setSpreadingFactor(SF);  // Más sensibilidad
  LoRa.setSignalBandwidth(BW);  // BW: 125 kHz
  LoRa.setCodingRate4(CR);      // Codificación 4/8
  LoRa.setPreambleLength(PL);
  LoRa.setTxPower(TXP);  // Potencia máxima

  Serial.println("LORA CONFIG DONE");

  loraTransmit("BS;OK;FTU:" + String(bootCount) + ";");

  String incoming;
  if (loraReceive(incoming, 5000)) {
    incoming.trim();
    if (!incoming.startsWith("BS;")) {
      Serial.println("Mensaje no dirigido a esta estación");
    } else {
      incoming = incoming.substring(3);
      unsigned long startTime = millis();

      while (incoming != "END") {
        if (incoming.startsWith("SLEEP:")) {
          sleepTimeSec = incoming.substring(6).toInt();
          sendOk(sleepTimeSec);
        } else if (incoming == "AYA") {
          sendOk();
        } else if (incoming == "RES") {
          sendOk("RES:" + String(imageResolution));
        } else if (incoming.startsWith("RES:")) {
          imageResolution = parseResolution(incoming.substring(4));
          sendOk(incoming);
        } else if (incoming == "QTY") {
          sendOk("QTY:" + String(imageQuality));
        }else if (incoming.startsWith("QTY:")) {
          imageQuality = incoming.substring(4).toInt();
          sendOk("QTY:" + String(imageQuality));
        } else if (incoming == "ACK") {
          sendOk("SCO");
        } else if (incoming == "FTU") {
          sendOk("FTU:" + String(bootCount));
        } else if (incoming.startsWith("SET:")) {
          String cmd = incoming.substring(4);
          int sep = cmd.indexOf('=');
          if (sep != -1) {
            String param = cmd.substring(0, sep);
            String val = cmd.substring(sep + 1);

            if (param == "SF") SF = val.toInt();
            else if (param == "TXP") {
              TXP = val.toInt();
              sendOk(param);
            } else if (param == "BW") {
              BW = val.toInt();
              sendOk(param);
            } else if (param == "PL") {
              PL = val.toInt();
              sendOk(param);
            } else if (param == "CR") {
              CR = val.toInt();
              sendOk(param);
            } else if (param == "RXTO") {
              loraRxTimeout = val.toInt();
              sendOk(param);
            } else loraTransmit("BS;ERR;UnknownParam");
          }
        } else if (incoming.startsWith("STATUS")) {
          String status = "STATUS;";
          status += "SF:" + String(SF);
          status += ";BW:" + String(BW);
          status += ";CR:4/" + String(CR);
          status += ";PL:" + String(PL);
          status += ";TXP:" + String(TXP);
          status += ";RXTO:" + String(loraRxTimeout);
          sendOk(status);
        } else if (incoming.startsWith("PSRAM")) {
          sendOk(psramFound());
        } else if (incoming == "RESETREASON") {
          sendOk(reason);
        } else if (incoming == "RESET") {
          ESP.restart();
        } else if (incoming == "SIGNAL") {
          int rssi = LoRa.packetRssi();
          float snr = LoRa.packetSnr();
          Serial.printf("RSSI:%d;SNR:%.2f;\n", rssi, snr);
          String msg = "RSSI:" + String(rssi) + ";SNR:" + String(snr, 2);
          sendOk(msg);
        } else if (incoming == "IMG") {
          camera_fb_t* fb = esp_camera_fb_get();
          if (!fb) {
            Serial.println("Error capturando imagen");
            sendOk("ERROR");
          } else {
            sendImage(fb);
            esp_camera_fb_return(fb);
          }
        }

        incoming = "";
        if (loraReceive(incoming, loraRxTimeout)) {
          incoming.trim();
          if (!incoming.startsWith("BS;")) continue;
          incoming = incoming.substring(3);
          startTime = millis();
        } else {
          if (millis() - startTime >= loraRxTimeout) {
            Serial.println("Timeout esperando comandos.");
            break;
          }
        }
      }

      sendOk("BYE");
    }
  } else {
    Serial.println("No se recibió respuesta");
    sendOk("BYE");
  }

  esp_camera_deinit();
  Serial.println("Durmiendo por " + String(sleepTimeSec) + " s");
  esp_sleep_enable_timer_wakeup(sleepTimeSec * uS_TO_S_FACTOR);
  esp_deep_sleep_start();
}

void loop() {}
