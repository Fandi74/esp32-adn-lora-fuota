#include <SPI.h>
#include <LoRa.h>
#include <SSD1306Wire.h>
#include <fuota_protocol.h>

// ================= HARDWARE PINS (T3 V1.6.1) =================
#define LORA_SCK     5
#define LORA_MISO    19
#define LORA_MOSI    27
#define LORA_CS      18
#define LORA_RST     23
#define LORA_DIO0    26

#define OLED_SDA     21
#define OLED_SCL     22
#define OLED_ADDR    0x3C

// ================= GLOBALS =================
SSD1306Wire display(OLED_ADDR, OLED_SDA, OLED_SCL);

bool isReceiving = false;
uint16_t activeSessionId = 0;
uint32_t expectedFileSize = 0;
uint16_t expectedChunkSize = 0;
uint16_t totalExpectedPackets = 0;
uint32_t expectedImageCrc32 = 0;
int validPacketsReceived = 0;
int highestIdReceived = -1;
int lostPackets = 0;
int badPackets = 0;

void showStatus(String s) {
  display.clear();
  display.setFont(ArialMT_Plain_10);
  display.drawString(0, 0, "Node Status:");
  display.drawString(0, 25, s);
  display.display();
}

// ================= FUNCTIONS =================

void processPacket(int packetSize) {
  uint8_t buffer[FUOTA_MAX_FRAME_SIZE];
  int i = 0;
  
  while (LoRa.available() && i < (int)sizeof(buffer)) {
    buffer[i++] = LoRa.read();
  }

  while (LoRa.available()) {
    LoRa.read();
  }
  
  FuotaFrameHeader header;
  if (!fuotaReadFrameHeader(buffer, i, header)) {
    Serial.printf("Invalid FUOTA frame. Size: %d\n", i);
    return;
  }

  const uint8_t* payload = &buffer[FUOTA_HEADER_SIZE];
  uint32_t actualPayloadCrc32 = fuotaCrc32(payload, header.payloadLen);
  if (actualPayloadCrc32 != header.payloadCrc32) {
    badPackets++;
    Serial.printf("Bad packet CRC. Type: %u | ID: %u | Expected: 0x%08X | Got: 0x%08X\n",
                  header.type, header.packetId, (unsigned int)header.payloadCrc32,
                  (unsigned int)actualPayloadCrc32);
    return;
  }

  if (header.type == FUOTA_FRAME_METADATA) {
    if (header.payloadLen != FUOTA_METADATA_PAYLOAD_SIZE) {
      Serial.printf("Invalid metadata payload size: %u\n", header.payloadLen);
      return;
    }

    FuotaMetadata metadata = fuotaReadMetadata(payload);
    activeSessionId = header.sessionId;
    expectedFileSize = metadata.fileSize;
    expectedChunkSize = metadata.chunkSize;
    totalExpectedPackets = metadata.totalPackets;
    expectedImageCrc32 = metadata.imageCrc32;

    isReceiving = true;
    validPacketsReceived = 0;
    highestIdReceived = -1;
    lostPackets = 0;
    badPackets = 0;

    Serial.println("\n--- FUOTA METADATA ---");
    Serial.printf("Session: %u\n", activeSessionId);
    Serial.printf("File size: %u\n", (unsigned int)expectedFileSize);
    Serial.printf("Chunk size: %u\n", expectedChunkSize);
    Serial.printf("Total packets: %u\n", totalExpectedPackets);
    Serial.printf("Firmware version: %u\n", (unsigned int)metadata.firmwareVersion);
    Serial.printf("Image CRC32: 0x%08X\n", (unsigned int)expectedImageCrc32);

    showStatus("Incoming stream...");
    return;
  }

  if (header.sessionId != activeSessionId) {
    Serial.printf("Ignoring old session frame. Active: %u | Got: %u\n",
                  activeSessionId, header.sessionId);
    return;
  }

  if (header.type == FUOTA_FRAME_END) {
    if (isReceiving) {
      isReceiving = false;
      lostPackets = totalExpectedPackets - validPacketsReceived;
      float finalCorruptRate = totalExpectedPackets > 0
        ? ((float)(lostPackets + badPackets) / (float)totalExpectedPackets) * 100.0
        : 0.0;

      Serial.println("\n--- STREAM FINISHED ---");
      Serial.printf("Expected: %u\n", totalExpectedPackets);
      Serial.printf("Received: %d\n", validPacketsReceived);
      Serial.printf("Missing: %d\n", lostPackets);
      Serial.printf("Bad CRC: %d\n", badPackets);
      Serial.printf("Error Rate: %.2f%%\n", finalCorruptRate);

      display.clear();
      display.setFont(ArialMT_Plain_10);
      display.drawString(0, 0, "--- FINISHED ---");
      display.drawString(0, 15, "Rx: " + String(validPacketsReceived) + "/" + String(totalExpectedPackets));
      display.drawString(0, 30, "Miss: " + String(lostPackets) + " Bad: " + String(badPackets));
      display.drawString(0, 45, "Rate: " + String(finalCorruptRate, 1) + "%");
      display.display();

      if (lostPackets > 0 || badPackets > 0) {
        Serial.println("[PLACEHOLDER] Generate missing packet list for retry.");
      } else {
        Serial.println("[PLACEHOLDER] Image complete. Verify staged file before OTA.");
      }
    }
    return;
  }

  if (header.type == FUOTA_FRAME_DATA && isReceiving) {
    if (header.payloadLen > expectedChunkSize) {
      badPackets++;
      Serial.printf("Unexpected payload length. ID: %u | Len: %u\n",
                    header.packetId, header.payloadLen);
      return;
    }

    if (header.packetId > highestIdReceived) {
      highestIdReceived = header.packetId;
    }

    validPacketsReceived++;
    int expectedUpToNow = highestIdReceived + 1;
    lostPackets = expectedUpToNow - validPacketsReceived;

    if (validPacketsReceived % 10 == 0) {
      float currentCorruptRate = ((float)lostPackets / (float)expectedUpToNow) * 100.0;
      
      display.clear();
      display.setFont(ArialMT_Plain_10);
      display.drawString(0, 0, "Receiving OTA...");
      
      // e.g., "Pkts: 450 / 2000"
      display.drawString(0, 15, "Pkts: " + String(validPacketsReceived) + " / " + String(totalExpectedPackets));
      
      // e.g., "Corrupted: 23"
      display.drawString(0, 30, "Corrupted: " + String(lostPackets));
      
      // e.g., "Rate: 4.8%"
      display.drawString(0, 45, "Rate: " + String(currentCorruptRate, 1) + "%");
      display.display();
      
      Serial.printf("ID: %u | Len: %u | Rx: %d | Lost: %d | Bad: %d | Rate: %.1f%%\n",
                    header.packetId, header.payloadLen, validPacketsReceived, lostPackets,
                    badPackets, currentCorruptRate);
    }
    return;
  }

  Serial.printf("Unhandled FUOTA frame type: %u\n", header.type);
}


void setup() {
  Serial.begin(115200);

  // 1. Init OLED
  // pinMode(16, OUTPUT); 
  // digitalWrite(16, LOW); delay(50); digitalWrite(16, HIGH);
  display.init();
  display.flipScreenVertically();
  display.setFont(ArialMT_Plain_10);
  showStatus("Node Booting...");

  // 2. Init LoRa
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
  LoRa.setPins(LORA_CS, LORA_RST, LORA_DIO0);
  
  if (!LoRa.begin(923E6)) {
    Serial.println("LoRa Init Failed!");
    showStatus("LoRa Fail!");
    while(1);
  }
  
  // MATCH GATEWAY EXACTLY
  LoRa.setSpreadingFactor(7);
  LoRa.setSignalBandwidth(125E3);
  LoRa.setCodingRate4(5);
  
  Serial.println("Node Ready. Profiling Mode.");
  showStatus("Listening..."); 
}

void loop() {
  int packetSize = LoRa.parsePacket();
  if (packetSize) {
    processPacket(packetSize);
  }
}
