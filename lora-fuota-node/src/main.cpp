#include <SPI.h>
#include <LoRa.h>
#include <SSD1306Wire.h>

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
int totalExpectedPackets = 0;
int validPacketsReceived = 0;
int highestIdReceived = -1;
int lostPackets = 0;

void setup() {
  Serial.begin(115200);

  // 1. Init OLED
  pinMode(16, OUTPUT); 
  digitalWrite(16, LOW); delay(50); digitalWrite(16, HIGH);
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

// ================= FUNCTIONS =================

void processPacket(int packetSize) {
  uint8_t buffer[256]; 
  int i = 0;
  
  while (LoRa.available()) {
    buffer[i++] = LoRa.read();
  }
  
  String strData = "";
  for(int k=0; k<i && k<10; k++) strData += (char)buffer[k];
  
  // --- COMMAND: START ---
  if (strData.startsWith("START:")) {
    String sizeStr = strData.substring(6);
    size_t fileSize = sizeStr.toInt();
    
    // Gateway uses 250 bytes per packet.
    totalExpectedPackets = (fileSize / 250) + 1;
    
    // Reset our profiling counters
    isReceiving = true;
    validPacketsReceived = 0;
    highestIdReceived = -1;
    lostPackets = 0;
    
    Serial.printf("START received. Expecting %d packets.\n", totalExpectedPackets);
    showStatus("Incoming stream...");
    return;
  }

  // --- COMMAND: END ---
  if (strData.startsWith("END")) {
    if (isReceiving) {
      isReceiving = false;
      
      // Calculate final stats
      float finalCorruptRate = ((float)lostPackets / (float)totalExpectedPackets) * 100.0;
      
      Serial.println("\n--- STREAM FINISHED ---");
      Serial.printf("Expected: %d\n", totalExpectedPackets);
      Serial.printf("Received: %d\n", validPacketsReceived);
      Serial.printf("Lost/Corrupted: %d\n", lostPackets);
      Serial.printf("Corruption Rate: %.2f%%\n", finalCorruptRate);
      
      // Final OLED Display
      display.clear();
      display.setFont(ArialMT_Plain_10);
      display.drawString(0, 0, "--- FINISHED ---");
      display.drawString(0, 15, "Rx: " + String(validPacketsReceived) + "/" + String(totalExpectedPackets));
      display.drawString(0, 30, "Lost: " + String(lostPackets));
      display.drawString(0, 45, "Rate: " + String(finalCorruptRate, 1) + "%");
      display.display();

      // =========================================================
      // PLACEHOLDER FOR CORRUPTION FIX (NACK / RETRY LOGIC)
      // =========================================================
      if (lostPackets > 0) {
        Serial.println("[PLACEHOLDER] Generating list of missing IDs...");
        Serial.println("[PLACEHOLDER] Sending NACK back to Gateway...");
        // TODO: Build the missing packet array and send request via LoRa TX
      } else {
        Serial.println("[PLACEHOLDER] 100% Intact. Executing OTA Flash...");
      }
      // =========================================================
    }
    return;
  }

  // --- DATA PACKET HANDLING ---
  if (isReceiving && i >= 3) {
    // 1. Extract the 2-byte Sequence ID
    uint16_t packetId = (buffer[0] << 8) | buffer[1];
    
    // 2. Track the highest ID we've seen to detect gaps
    if (packetId > highestIdReceived) {
      highestIdReceived = packetId;
    }
    
    validPacketsReceived++;
    
    // 3. Calculate how many we've lost so far
    // If highest ID is 10, we SHOULD have received 11 packets (0 through 10)
    int expectedUpToNow = highestIdReceived + 1;
    lostPackets = expectedUpToNow - validPacketsReceived;
    
    // 4. Update OLED every 10 packets to keep UI responsive
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
      
      Serial.printf("ID: %d | Rx: %d | Lost: %d | Rate: %.1f%%\n", packetId, validPacketsReceived, lostPackets, currentCorruptRate);
    }
  }
}

void showStatus(String s) {
  display.clear();
  display.setFont(ArialMT_Plain_10);
  display.drawString(0, 0, "Node Status:");
  display.drawString(0, 25, s);
  display.display();
}