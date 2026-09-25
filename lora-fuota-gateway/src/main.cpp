#include <WiFi.h>
#include <PubSubClient.h>
#include <HTTPClient.h>
#include <SD.h>
#include <SPI.h>
#include <Wire.h>
#include <LoRa.h>
#include <SSD1306Wire.h>
#include <fuota_protocol.h>

// #include <mysecrets.h>

#define WIFI_SSID "Damai 2-2"
#define WIFI_PASSWORD "Damaimei2026"
#define BROKER_SERVER "broker.emqx.io"
#define BROKER_PORT 1883
#define MQTT_CLIENT_ID "fuota-gateway-esp32"
#define LORA_TOPIC "lora/ota/url"

// ================= USER CONFIG =================
const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;
const char* mqtt_server = BROKER_SERVER;
const char* mqtt_topic = LORA_TOPIC; //lora/ota/url
const char* mqtt_client_id = MQTT_CLIENT_ID;

#define LORA_SCK     5
#define LORA_MISO    19
#define LORA_MOSI    27
#define LORA_CS      18
#define FUOTA_LORA_RST 23
#define LORA_DIO0    26

#define SD_SCK       14
#define SD_MISO      2
#define SD_MOSI      15
#define SD_CS        13

#define FUOTA_OLED_SDA 21
#define FUOTA_OLED_SCL 22
#define OLED_ADDR    0x3C

// ================= GLOBALS =================
SSD1306Wire display(OLED_ADDR, FUOTA_OLED_SDA, FUOTA_OLED_SCL);
WiFiClient espClient;
PubSubClient client(espClient);
SPIClass sdSPI(HSPI); // Create a separate SPI instance for SD Card

String downloadUrl = "";
bool startProcess = false;
bool oledReady = false;

// ================= FUNCTIONS =================

// MQTT Callback
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg;
  for (int i = 0; i < length; i++) msg += (char)payload[i];
  
  Serial.print("Received URL: ");
  Serial.println(msg);
  
  downloadUrl = msg;
  startProcess = true; // Trigger loop
}

// Helper for OLED
void showStatus(String s) {
  if (!oledReady) return;
  display.clear();
  display.setFont(ArialMT_Plain_10);
  display.drawString(0, 0, "GW Status:");
  display.drawString(0, 25, s);
  display.display();
}

bool calculateFileCrc32(File& f, uint32_t& result) {
  uint8_t buffer[256];
  uint32_t crc = 0xFFFFFFFFUL;
  size_t remaining = f.size();

  if (!f.seek(0)) return false;
  while (remaining > 0) {
    size_t toRead = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
    int bytesRead = f.read(buffer, toRead);
    if (bytesRead != (int)toRead) return false;
    crc = fuotaCrc32Update(crc, buffer, bytesRead);
    remaining -= bytesRead;
  }

  result = ~crc;
  return f.seek(0);
}

bool sendFuotaFrame(uint8_t type, uint16_t sessionId, uint16_t packetId,
                    const uint8_t* payload, uint8_t payloadLen) {
  if (payloadLen > FUOTA_MAX_PAYLOAD_SIZE) {
    Serial.println("FUOTA payload too large");
    return false;
  }

  uint8_t frame[FUOTA_MAX_FRAME_SIZE];
  size_t frameLen = fuotaWriteFrame(frame, type, sessionId, packetId, payload, payloadLen);

  if (!LoRa.beginPacket()) return false;
  if (LoRa.write(frame, frameLen) != frameLen) return false;
  return LoRa.endPacket() == 1;
}

// Download HTTP -> SD Card
bool downloadToSD(String url) {
  HTTPClient http;
  showStatus("Downloading...");
  Serial.println("Downloading " + url);

  if (!http.begin(url)) {
    Serial.println("Invalid download URL");
    return false;
  }

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("HTTP Failed: %d\n", httpCode);
    http.end();
    return false;
  }

  int expectedSize = http.getSize();
  const size_t maxImageSize = (size_t)FUOTA_CHUNK_SIZE * FUOTA_MAX_TRACKED_PACKETS;
  if (expectedSize == 0 || (expectedSize > 0 && (size_t)expectedSize > maxImageSize)) {
    Serial.printf("Invalid image size: %d\n", expectedSize);
    http.end();
    return false;
  }

  if (SD.exists("/update.tmp") && !SD.remove("/update.tmp")) {
    Serial.println("Could not remove previous temporary download");
    http.end();
    return false;
  }
  File f = SD.open("/update.tmp", FILE_WRITE);
  if (!f) {
    Serial.println("SD Write Error");
    http.end();
    return false;
  }

  int written = http.writeToStream(&f);
  f.flush();
  size_t stored = f.size();
  f.close();
  http.end();

  if (written <= 0 || stored != (size_t)written || stored > maxImageSize ||
      (expectedSize >= 0 && written != expectedSize)) {
    Serial.printf("Incomplete download: HTTP=%d SD=%u expected=%d\n",
                  written, (unsigned int)stored, expectedSize);
    SD.remove("/update.tmp");
    return false;
  }

  if ((SD.exists("/update.bin") && !SD.remove("/update.bin")) ||
      !SD.rename("/update.tmp", "/update.bin")) {
    Serial.println("Could not replace update.bin");
    return false;
  }

  Serial.printf("Download Success: %u bytes\n", (unsigned int)stored);
  showStatus("DL Complete!");
  return true;
}

// Read SD -> LoRa Broadcast
bool broadcastFromSD() {
  File f = SD.open("/update.bin", FILE_READ);
  if (!f) {
    Serial.println("Cannot open file for broadcast");
    return false;
  }

  size_t fileSize = f.size();
  uint32_t calculatedPackets = (fileSize + FUOTA_CHUNK_SIZE - 1) / FUOTA_CHUNK_SIZE;
  uint16_t sessionId = (uint16_t)(millis() & 0xFFFF);
  uint32_t imageCrc32 = 0;
  uint8_t buffer[FUOTA_CHUNK_SIZE];

  if (calculatedPackets == 0 || calculatedPackets > FUOTA_MAX_TRACKED_PACKETS) {
    Serial.printf("Invalid packet count: %u. Prototype limit is %u packets.\n",
                  (unsigned int)calculatedPackets, FUOTA_MAX_TRACKED_PACKETS);
    f.close();
    return false;
  }

  if (!calculateFileCrc32(f, imageCrc32)) {
    Serial.println("Could not read complete image for CRC32");
    f.close();
    return false;
  }

  uint16_t totalPackets = (uint16_t)calculatedPackets;

  showStatus("Starting Broadcast...");
  Serial.println("Start LoRa Broadcast...");
  Serial.printf("Session: %u | Size: %u | Chunk: %u | Packets: %u | CRC32: 0x%08X\n",
                sessionId, (unsigned int)fileSize, FUOTA_CHUNK_SIZE, totalPackets,
                (unsigned int)imageCrc32);

  // 1. Send firmware metadata frame
  FuotaMetadata metadata;
  metadata.fileSize = (uint32_t)fileSize;
  metadata.chunkSize = FUOTA_CHUNK_SIZE;
  metadata.totalPackets = totalPackets;
  metadata.firmwareVersion = 1;
  metadata.imageCrc32 = imageCrc32;

  uint8_t metadataPayload[FUOTA_METADATA_PAYLOAD_SIZE];
  fuotaWriteMetadata(metadataPayload, metadata);
  for (uint8_t attempt = 0; attempt < 3; attempt++) {
    if (!sendFuotaFrame(FUOTA_FRAME_METADATA, sessionId, 0, metadataPayload, sizeof(metadataPayload))) {
      Serial.println("Metadata TX failed");
      f.close();
      return false;
    }
    delay(200);
  }
  
  // Give node time to prepare for the stream.
  delay(2000); 

  // 2. Loop Chunks
  for (uint16_t i = 0; i < totalPackets; i++) {
    size_t expectedBytes = fuotaExpectedPacketLength(
      fileSize, FUOTA_CHUNK_SIZE, i, totalPackets);
    int bytesRead = f.read(buffer, expectedBytes);
    if (bytesRead != (int)expectedBytes ||
        !sendFuotaFrame(FUOTA_FRAME_DATA, sessionId, i, buffer, (uint8_t)bytesRead)) {
      Serial.printf("Packet %u read/TX failed\n", i);
      f.close();
      return false;
    }

    // Update OLED every 20 packets (don't slow down too much)
    if (oledReady && i % 20 == 0) {
      String status = "Tx: " + String(i) + "/" + String(totalPackets);
      display.clear();
      display.drawString(0, 0, "Broadcasting...");
      display.drawString(0, 20, status);
      display.display();
      Serial.println(status);
    }

    // PACE YOURSELF: 50ms delay is vital for T3 V1.6 reliability
    delay(50); 
  }

  // 3. Send END frame
  for (uint8_t attempt = 0; attempt < 3; attempt++) {
    if (!sendFuotaFrame(FUOTA_FRAME_END, sessionId, totalPackets, NULL, 0)) {
      Serial.println("END TX failed");
      f.close();
      return false;
    }
    delay(200);
  }
  
  f.close();
  Serial.println("Broadcast Finished.");
  return true;
}

// ================= SETUP =================

void setup() {
  Serial.begin(115200);
  
  // 1. Init OLED
  // pinMode(16, OUTPUT); // OLED Reset pin for some boards
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
  showStatus("Booting...");

  // 2. Init SD Card (on separate SPI bus)
  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, sdSPI)) {
    Serial.println("SD Mount Failed!");
    showStatus("SD Fail!");
    while(1); // Stop if no storage
  }
  Serial.println("SD Card Ready.");

  // 3. Init LoRa (on default SPI bus)
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
  LoRa.setPins(LORA_CS, FUOTA_LORA_RST, LORA_DIO0);
  if (!LoRa.begin(923E6)) {
    Serial.println("LoRa Init Failed!");
    showStatus("LoRa Fail!");
    while(1);
  }
  LoRa.setSpreadingFactor(7);
  LoRa.setSignalBandwidth(125E3);
  LoRa.setCodingRate4(5); // 4/5
  LoRa.enableCrc();
  Serial.println("LoRa Ready (923MHz).");

  // 4. Connect WiFi (Timeout: 60s)
  unsigned long wifiStart = millis();
  WiFi.begin(ssid, password);
  showStatus("Connecting WiFi...");
  
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - wifiStart > 60000) {
      Serial.println("WiFi Failed: Timeout > 60s");
      showStatus("WiFi Failed!");
      while(1) { delay(100); } // Stop here forever
    }
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi Success.");
  showStatus("WiFi OK.");

  // 5. Connect MQTT (Timeout: 120s)
  client.setServer(mqtt_server, BROKER_PORT);
  client.setCallback(mqttCallback);
  
  unsigned long mqttStart = millis();
  showStatus("Connecting Broker...");
  
  while (!client.connected()) {
    if (client.connect(mqtt_client_id)) {
      Serial.println("Broker Connected");
      client.subscribe(mqtt_topic, 1); // QoS 1
    } else {
      if (millis() - mqttStart > 120000) {
        Serial.println("Broker Failed: Timeout > 120s");
        showStatus("Broker Failed!");
        while(1) { delay(100); } // Stop here forever
      }
      Serial.print(".");
      delay(2000);
    }
  }
  showStatus("Ready! Waiting...");
}

// ================= MAIN LOOP =================
void loop() {
  // Keep MQTT alive
  if (!client.connected()) {
     // Optional: Reconnect logic if you want, or just fail based on your strict rules.
     // For now, we assume if it drops, we try to reconnect simply.
     if (client.connect(mqtt_client_id)) {
        client.subscribe(mqtt_topic, 1);
     }
  }
  client.loop();

  // If callback set the flag, start the job
  if (startProcess) {
    startProcess = false; // Reset flag
    
    // Step A: Download
    if (downloadToSD(downloadUrl)) {
      // Step B: Broadcast
      if (broadcastFromSD()) {
        showStatus("Sent. Waiting...");
      } else {
        showStatus("Broadcast Error");
      }
    } else {
      showStatus("Download Error");
    }
    
  }
}
