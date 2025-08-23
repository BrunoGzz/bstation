#include <SPI.h>
#include <LoRa.h>

void setup() {
  Serial.begin(115200);
  while (!Serial);
  if (!LoRa.begin(433E6)) {
    Serial.println(F("[A] LORA ERROR"));
    while (true);
  }

  LoRa.setSpreadingFactor(12);
  LoRa.setSignalBandwidth(125E3);
  LoRa.setCodingRate4(8);
  LoRa.setPreambleLength(8);
  LoRa.setTxPower(18);

  Serial.println(F("[A] LORA READY"));
}

void loop() {
  // Enviar comandos desde Serial por LoRa
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd.length() > 0) {
      String message = "BS;" + cmd;
      Serial.print(">>> ");
      Serial.println(message);

      LoRa.beginPacket();
      LoRa.print(message);
      LoRa.endPacket();
    }
  }

  int packetSize = LoRa.parsePacket();
  if (packetSize > 0) {
    uint8_t buffer[256];
    int len = 0;

    // Leer todo el paquete recibido
    while (LoRa.available() && len < sizeof(buffer)) {
      buffer[len++] = LoRa.read();
    }

    // Detectar si el buffer empieza con 'P:'
    if (len > 2 && buffer[0] == 'P' && buffer[1] == ':') {
        // Buscar segundo ':'
        int colonCount = 0;
        int headerEnd = -1;
        for (int i = 0; i < len; i++) {
            if (buffer[i] == ':') {
                colonCount++;
                if (colonCount == 2) {
                    headerEnd = i;
                    break;
                }
            }
        }

        if (headerEnd >= 0) {
            // Imprimir header como texto
            Serial.print("<<< ");
            for (int i = 0; i <= headerEnd; i++) {
                Serial.print((char)buffer[i]);
            }

            // Imprimir payload en hex
            int payloadStart = headerEnd + 1;
            int payloadLength = len - payloadStart;

            for (int i = 0; i < payloadLength; i++) {
                byte b = buffer[payloadStart + i];
                if (b < 16) Serial.print('0');
                Serial.print(b, HEX);
            }
            Serial.println();
        } else {
            // No encontramos el segundo ':', imprimir todo como texto
            Serial.print("<<< ");
            for (int i = 0; i < len; i++) {
                Serial.print((char)buffer[i]);
            }
            Serial.println();
        }
    } else {
        // No empieza con P:, imprimir todo como texto
        Serial.print("<<< ");
        for (int i = 0; i < len; i++) {
            Serial.print((char)buffer[i]);
        }
        Serial.println();
    }
  } 
  delay(10);
}
