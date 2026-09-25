#include <SPI.h>
#include <Wire.h>
#include <LoRa.h>
#include <SSD1306Wire.h>
#include <fuota_protocol.h>

// ================= HARDWARE PINS (T3 V1.6.1) =================
#define LORA_SCK     5
#define LORA_MISO    19
#define LORA_MOSI    27
#define LORA_CS      18
#define FUOTA_LORA_RST 23
#define LORA_DIO0    26

#define FUOTA_OLED_SDA 21
#define FUOTA_OLED_SCL 22
#define OLED_ADDR    0x3C

// ================= GLOBALS =================
SSD1306Wire display(OLED_ADDR, FUOTA_OLED_SDA, FUOTA_OLED_SCL);

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
int duplicatePackets = 0;
unsigned long lastOledFrameAt = 0;
uint8_t oledFrame = 0;
uint8_t receivedPacketBitmap[FUOTA_RECEIVE_BITMAP_SIZE];
bool oledReady = false;
bool holdOledResult = false;
unsigned long oledResultAt = 0;

const unsigned long OLED_IDLE_INTERVAL_MS = 500;
const unsigned long OLED_RX_INTERVAL_MS = 150;
const unsigned long OLED_RESULT_HOLD_MS = 10000;
const uint8_t MAX_MISSING_IDS_TO_LOG = 32;

void showStatus(String s) {
  if (!oledReady) return;
  display.clear();
  display.setFont(ArialMT_Plain_10);
  display.drawString(0, 0, "Node Status:");
  display.drawString(0, 25, s);
  display.display();
}

// ================= FUNCTIONS =================

void drawProgressBar(uint8_t progressPercent) {
  if (progressPercent > 100) {
    progressPercent = 100;
  }

  int fillWidth = (progressPercent * 126) / 100;
  display.drawRect(0, 54, 128, 8);
  display.fillRect(1, 55, fillWidth, 6);
}

bool isValidMetadata(const FuotaMetadata& metadata) {
  if (metadata.fileSize == 0 || metadata.chunkSize != FUOTA_CHUNK_SIZE) {
    return false;
  }

  uint32_t calculatedPackets = (metadata.fileSize + metadata.chunkSize - 1) / metadata.chunkSize;
  return calculatedPackets > 0 &&
         calculatedPackets <= FUOTA_MAX_TRACKED_PACKETS &&
         metadata.totalPackets == calculatedPackets;
}

void resetReceiveBitmap() {
  for (uint16_t i = 0; i < FUOTA_RECEIVE_BITMAP_SIZE; i++) {
    receivedPacketBitmap[i] = 0;
  }
}

bool isPacketReceived(uint16_t packetId) {
  if (packetId >= FUOTA_MAX_TRACKED_PACKETS) {
    return false;
  }

  uint16_t byteIndex = packetId / 8;
  uint8_t bitMask = 1 << (packetId % 8);
  return (receivedPacketBitmap[byteIndex] & bitMask) != 0;
}

void markPacketReceived(uint16_t packetId) {
  if (packetId >= FUOTA_MAX_TRACKED_PACKETS) {
    return;
  }

  uint16_t byteIndex = packetId / 8;
  uint8_t bitMask = 1 << (packetId % 8);
  receivedPacketBitmap[byteIndex] |= bitMask;
}

int calculateMissingPackets() {
  int missing = 0;
  for (uint16_t packetId = 0; packetId < totalExpectedPackets; packetId++) {
    if (!isPacketReceived(packetId)) {
      missing++;
    }
  }
  return missing;
}

void printMissingPacketPreview() {
  if (lostPackets <= 0) {
    return;
  }

  Serial.print("Missing packet IDs");
  if (lostPackets > MAX_MISSING_IDS_TO_LOG) {
    Serial.printf(" (first %u)", MAX_MISSING_IDS_TO_LOG);
  }
  Serial.print(": ");

  uint8_t printed = 0;
  for (uint16_t packetId = 0; packetId < totalExpectedPackets && printed < MAX_MISSING_IDS_TO_LOG; packetId++) {
    if (!isPacketReceived(packetId)) {
      if (printed > 0) {
        Serial.print(", ");
      }
      Serial.print(packetId);
      printed++;
    }
  }

  Serial.println();
}

void drawListeningAnimation() {
  if (!oledReady) return;
  unsigned long now = millis();
  if (now - lastOledFrameAt < OLED_IDLE_INTERVAL_MS) {
    return;
  }

  lastOledFrameAt = now;
  oledFrame++;

  const char spinner[] = "|/-\\";
  int pulseX = (oledFrame % 8) * 16;

  display.clear();
  display.setFont(ArialMT_Plain_10);
  display.drawString(0, 0, "Node Status:");
  display.drawString(0, 18, "LoRa open " + String(spinner[oledFrame % 4]));
  display.drawString(0, 34, "Waiting FUOTA");
  display.drawRect(0, 54, 128, 8);
  display.fillRect(pulseX, 55, 12, 6);
  display.display();
}

void drawReceivingAnimation(bool force) {
  if (!oledReady) return;
  unsigned long now = millis();
  if (!force && now - lastOledFrameAt < OLED_RX_INTERVAL_MS) {
    return;
  }

  lastOledFrameAt = now;
  oledFrame++;

  const char spinner[] = "|/-\\";
  uint8_t progressPercent = 0;
  if (totalExpectedPackets > 0) {
    progressPercent = (uint8_t)(((uint32_t)validPacketsReceived * 100UL) / totalExpectedPackets);
  }

  display.clear();
  display.setFont(ArialMT_Plain_10);
  display.drawString(0, 0, "FUOTA RX " + String(spinner[oledFrame % 4]));
  display.drawString(0, 13, "Session: " + String(activeSessionId));
  display.drawString(0, 26, "Pkts: " + String(validPacketsReceived) + "/" + String(totalExpectedPackets));
  display.drawString(0, 39, "M:" + String(lostPackets) + " B:" + String(badPackets) + " D:" + String(duplicatePackets));
  drawProgressBar(progressPercent);
  display.display();
}

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
  uint32_t actualFrameCrc32 = fuotaFrameCrc32(buffer, payload, header.payloadLen);
  if (actualFrameCrc32 != header.frameCrc32) {
    badPackets++;
    Serial.printf("Bad frame CRC. Type: %u | ID: %u | Expected: 0x%08X | Got: 0x%08X\n",
                  header.type, header.packetId, (unsigned int)header.frameCrc32,
                  (unsigned int)actualFrameCrc32);
    return;
  }

  if (header.type == FUOTA_FRAME_METADATA) {
    if (header.payloadLen != FUOTA_METADATA_PAYLOAD_SIZE) {
      Serial.printf("Invalid metadata payload size: %u\n", header.payloadLen);
      return;
    }

    FuotaMetadata metadata = fuotaReadMetadata(payload);
    if (!isValidMetadata(metadata)) {
      Serial.println("Invalid metadata values. Ignoring FUOTA session.");
      Serial.printf("File size: %u | Chunk size: %u | Total packets: %u\n",
                    (unsigned int)metadata.fileSize, metadata.chunkSize,
                    metadata.totalPackets);
      showStatus("Bad metadata");
      holdOledResult = true;
      oledResultAt = millis();
      return;
    }

    if (isReceiving && header.sessionId == activeSessionId &&
        metadata.fileSize == expectedFileSize &&
        metadata.chunkSize == expectedChunkSize &&
        metadata.totalPackets == totalExpectedPackets &&
        metadata.imageCrc32 == expectedImageCrc32) {
      return;
    }

    activeSessionId = header.sessionId;
    expectedFileSize = metadata.fileSize;
    expectedChunkSize = metadata.chunkSize;
    totalExpectedPackets = metadata.totalPackets;
    expectedImageCrc32 = metadata.imageCrc32;

    isReceiving = true;
    holdOledResult = false;
    validPacketsReceived = 0;
    highestIdReceived = -1;
    lostPackets = 0;
    badPackets = 0;
    duplicatePackets = 0;
    resetReceiveBitmap();

    Serial.println("\n--- FUOTA METADATA ---");
    Serial.printf("Session: %u\n", activeSessionId);
    Serial.printf("File size: %u\n", (unsigned int)expectedFileSize);
    Serial.printf("Chunk size: %u\n", expectedChunkSize);
    Serial.printf("Total packets: %u\n", totalExpectedPackets);
    Serial.printf("Firmware version: %u\n", (unsigned int)metadata.firmwareVersion);
    Serial.printf("Image CRC32: 0x%08X\n", (unsigned int)expectedImageCrc32);

    drawReceivingAnimation(true);
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
      lostPackets = calculateMissingPackets();
      float finalMissingRate = totalExpectedPackets > 0
        ? ((float)lostPackets / (float)totalExpectedPackets) * 100.0
        : 0.0;

      Serial.println("\n--- STREAM FINISHED ---");
      Serial.printf("Expected: %u\n", totalExpectedPackets);
      Serial.printf("Received: %d\n", validPacketsReceived);
      Serial.printf("Missing: %d\n", lostPackets);
      Serial.printf("Bad CRC: %d\n", badPackets);
      Serial.printf("Duplicates: %d\n", duplicatePackets);
      Serial.printf("Missing Rate: %.2f%%\n", finalMissingRate);

      if (oledReady) {
        display.clear();
        display.setFont(ArialMT_Plain_10);
        display.drawString(0, 0, "--- FINISHED ---");
        display.drawString(0, 15, "Rx: " + String(validPacketsReceived) + "/" + String(totalExpectedPackets));
        display.drawString(0, 30, "M:" + String(lostPackets) + " B:" + String(badPackets) + " D:" + String(duplicatePackets));
        display.drawString(0, 45, "Miss: " + String(finalMissingRate, 1) + "%");
        display.display();
      }
      holdOledResult = true;
      oledResultAt = millis();

      if (lostPackets > 0) {
        printMissingPacketPreview();
        Serial.println("[PLACEHOLDER] Send missing packet IDs back to gateway for retry.");
      } else {
        Serial.println("[PLACEHOLDER] Image complete. Verify staged file before OTA.");
      }
    }
    return;
  }

  if (header.type == FUOTA_FRAME_DATA && isReceiving) {
    if (header.packetId >= totalExpectedPackets) {
      badPackets++;
      Serial.printf("Out-of-range packet ID: %u | Total: %u\n",
                    header.packetId, totalExpectedPackets);
      return;
    }

    uint16_t expectedLength = fuotaExpectedPacketLength(
      expectedFileSize, expectedChunkSize, header.packetId, totalExpectedPackets);
    if (header.payloadLen != expectedLength) {
      badPackets++;
      Serial.printf("Unexpected payload length. ID: %u | Len: %u | Expected: %u\n",
                    header.packetId, header.payloadLen, (unsigned int)expectedLength);
      return;
    }

    if (header.packetId > highestIdReceived) {
      highestIdReceived = header.packetId;
    }

    if (isPacketReceived(header.packetId)) {
      duplicatePackets++;
      drawReceivingAnimation(false);
      return;
    }

    markPacketReceived(header.packetId);
    validPacketsReceived++;
    int expectedUpToNow = highestIdReceived + 1;
    lostPackets = expectedUpToNow > validPacketsReceived
      ? expectedUpToNow - validPacketsReceived
      : 0;
    drawReceivingAnimation(false);

    if (validPacketsReceived % 10 == 0) {
      float currentCorruptRate = ((float)lostPackets / (float)expectedUpToNow) * 100.0;

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
  Wire.beginTransmission(OLED_ADDR);
  oledReady = Wire.endTransmission() == 0;
  if (!oledReady) {
    Serial.printf("OLED not responding at 0x%02X on SDA %d / SCL %d\n",
                  OLED_ADDR, FUOTA_OLED_SDA, FUOTA_OLED_SCL);
  }
  showStatus("Node Booting...");

  // 2. Init LoRa
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
  LoRa.setPins(LORA_CS, FUOTA_LORA_RST, LORA_DIO0);
  
  if (!LoRa.begin(923E6)) {
    Serial.println("LoRa Init Failed!");
    showStatus("LoRa Fail!");
    while(1);
  }
  
  // MATCH GATEWAY EXACTLY
  LoRa.setSpreadingFactor(7);
  LoRa.setSignalBandwidth(125E3);
  LoRa.setCodingRate4(5);
  LoRa.enableCrc();
  
  Serial.println("Node Ready. Profiling Mode.");
  showStatus("Listening..."); 
}

void loop() {
  int packetSize = LoRa.parsePacket();
  if (packetSize) {
    processPacket(packetSize);
  } else if (isReceiving) {
    drawReceivingAnimation(false);
  } else if (holdOledResult && millis() - oledResultAt < OLED_RESULT_HOLD_MS) {
    return;
  } else {
    holdOledResult = false;
    drawListeningAnimation();
  }
}
