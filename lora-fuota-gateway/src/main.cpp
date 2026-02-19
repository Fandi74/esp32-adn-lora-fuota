#include <WiFi.h>
#include <PubSubClient.h>
#include <HTTPClient.h>
#include <SD.h>
#include <SPI.h>
#include <LoRa.h>
#include <SSD1306Wire.h>

// #include <mysecrets.h>

#define WIFI_SSID "Daya Tani-Net"
#define WIFI_PASSWORD "DayaTani-2025!"
#define BROKER_SERVER "broker.hivemq.com"
#define LORA_TOPIC "lora/ota/url"
#define DATA_SIZE 102400

// ================= USER CONFIG =================
const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;
const char* mqtt_server = BROKER_SERVER ; //broker.hivemq.com
const char* mqtt_topic = LORA_TOPIC; //lora/ota/url

#define LORA_SCK     5
#define LORA_MISO    19
#define LORA_MOSI    27
#define LORA_CS      18
#define LORA_RST     23
#define LORA_DIO0    26

#define SD_SCK       14
#define SD_MISO      2
#define SD_MOSI      15
#define SD_CS        13

#define OLED_SDA     21
#define OLED_SCL     22
#define OLED_ADDR    0x3C

// ================= GLOBALS =================
SSD1306Wire display(OLED_ADDR, OLED_SDA, OLED_SCL);
WiFiClient espClient;
PubSubClient client(espClient);
SPIClass sdSPI(HSPI); // Create a separate SPI instance for SD Card

String downloadUrl = "";
bool startProcess = false;

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
  display.clear();
  display.setFont(ArialMT_Plain_10);
  display.drawString(0, 0, "GW Status:");
  display.drawString(0, 25, s);
  display.display();
}

// Download HTTP -> SD Card
bool downloadToSD(String url) {
  HTTPClient http;
  showStatus("Downloading...");
  Serial.println("Downloading " + url);
  
  http.begin(url);
  int httpCode = http.GET();

  if (httpCode == 200) {
    // Open file on SD (Overwrite mode)
    // Note: SD library uses O_WRITE | O_CREAT | O_TRUNC by default for FILE_WRITE? 
    // Actually SD.open usually appends. We must remove first.
    if (SD.exists("/update.bin")) SD.remove("/update.bin");
    
    File f = SD.open("/update.bin", FILE_WRITE);
    if (!f) {
      Serial.println("SD Write Error");
      return false;
    } 

    int len = http.getSize();
    WiFiClient * stream = http.getStreamPtr();
    uint8_t buff[512];
    int total = 0;
    
    while (http.connected() && (len > 0 || len == -1)) {
      size_t size = stream->available();
      if (size) {
        int c = stream->readBytes(buff, ((size > sizeof(buff)) ? sizeof(buff) : size));
        f.write(buff, c);
        
        if (len > 0) len -= c;
        total += c;
        
        if (total % DATA_SIZE == 0) { // Update log every 100KB
           showStatus("DL: " + String(total/1024) + " KB");
        }
      }
      delay(1);
    }
    f.close();
    http.end();
    
    Serial.printf("Download Success: %d bytes\n", total);
    showStatus("DL Complete!");
    return true;
  } 
  
  Serial.printf("HTTP Failed: %d\n", httpCode);
  http.end();
  return false;
}

// Read SD -> LoRa Broadcast
void broadcastFromSD() {
  File f = SD.open("/update.bin", FILE_READ);
  if (!f) {
    Serial.println("Cannot open file for broadcast");
    return;
  }

  size_t fileSize = f.size();
  int packetSize = 250; 
  int totalPackets = (fileSize / packetSize) + 1;
  uint8_t buffer[packetSize];

  showStatus("Starting Broadcast...");
  Serial.println("Start LoRa Broadcast...");

  // 1. Send START Header
  LoRa.beginPacket();
  LoRa.print("START:");
  LoRa.print(fileSize);
  LoRa.endPacket();
  
  // Give Node time to erase flash (important!)
  delay(2000); 

  // 2. Loop Chunks
  for (int i = 0; i < totalPackets; i++) {
    int bytesRead = f.read(buffer, packetSize);
    
    LoRa.beginPacket();
    // Packet ID (2 Bytes)
    LoRa.write((uint8_t)(i >> 8));
    LoRa.write((uint8_t)(i & 0xFF));
    // Data
    LoRa.write(buffer, bytesRead);
    LoRa.endPacket();

    // Update OLED every 20 packets (don't slow down too much)
    if (i % 20 == 0) {
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

  // 3. Send END
  LoRa.beginPacket();
  LoRa.print("END");
  LoRa.endPacket();
  
  f.close();
  showStatus("Broadcast Success!");
  Serial.println("Broadcast Finished.");
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
  showStatus("Booting...");

  // 2. Init SD Card (on separate SPI bus)
  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, sdSPI)) {
    Serial.println("SD Mount Failed!");
    showStatus("SD Fail!");
    // while(1); // Stop if no storage
  }
  Serial.println("SD Card Ready.");

  // 3. Init LoRa (on default SPI bus)
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
  LoRa.setPins(LORA_CS, LORA_RST, LORA_DIO0);
  if (!LoRa.begin(923E6)) {
    Serial.println("LoRa Init Failed!");
    showStatus("LoRa Fail!");
    while(1);
  }
  LoRa.setSpreadingFactor(7);
  LoRa.setSignalBandwidth(125E3);
  LoRa.setCodingRate4(5); // 4/5
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
  client.setServer(mqtt_server, 1883);
  client.setCallback(mqttCallback);
  
  unsigned long mqttStart = millis();
  showStatus("Connecting Broker...");
  
  while (!client.connected()) {
    if (client.connect("T3_Gateway_Client")) {
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
     if (client.connect("T3_Gateway_Client")) {
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
      broadcastFromSD();
    } else {
      showStatus("Download Error");
    }
    
    // Resume listening
    showStatus("Job Done. Waiting...");
  }
}