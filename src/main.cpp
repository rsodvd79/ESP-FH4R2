#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <esp_partition.h>
#include <Adafruit_NeoPixel.h>
#include <ArduinoJson.h>

// Hostname
static const char* WIFI_HOST = "esp32s3";

// Simple HTTP server on port 80
WebServer server(80);
static bool fsMounted = false;

// Persisted network config (overrides defaults if /config.json exists)
static bool   cfg_dhcp = true;
static String cfg_ssid = "";
static String cfg_pass = "";
static String cfg_ip   = ""; // only used if dhcp=false
static String cfg_mask = "";
static String cfg_gw   = "";
static String cfg_dns  = "";

static bool parseIP(const String &s, IPAddress &out) {
  return out.fromString(s);
}

static void saveConfig() {
  if (!fsMounted) return;
  StaticJsonDocument<512> doc;
  doc["dhcp"] = cfg_dhcp;
  doc["ssid"] = cfg_ssid;
  doc["pass"] = cfg_pass;
  doc["ip"]   = cfg_ip;
  doc["mask"] = cfg_mask;
  doc["gw"]   = cfg_gw;
  doc["dns"]  = cfg_dns;
  File f = SPIFFS.open("/config.json", "w");
  if (!f) { Serial.println("Errore apertura /config.json in scrittura"); return; }
  serializeJson(doc, f);
  f.close();
}

static bool loadConfig() {
  if (!fsMounted) return false;
  if (!SPIFFS.exists("/config.json")) {
    Serial.println("/config.json non trovato. Avvio in modalità AP per setup.");
    return false;
  }
  File f = SPIFFS.open("/config.json", "r");
  if (!f) { Serial.println("Impossibile aprire /config.json"); return false; }
  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) { Serial.printf("JSON config parse error: %s\n", err.c_str()); return false; }
  if (doc.containsKey("dhcp")) cfg_dhcp = doc["dhcp"].as<bool>();
  if (doc.containsKey("ssid")) cfg_ssid = String(doc["ssid"].as<const char*>());
  if (doc.containsKey("pass")) cfg_pass = String(doc["pass"].as<const char*>());
  if (doc.containsKey("ip"))   cfg_ip   = String(doc["ip"].as<const char*>());
  if (doc.containsKey("mask")) cfg_mask = String(doc["mask"].as<const char*>());
  if (doc.containsKey("gw"))   cfg_gw   = String(doc["gw"].as<const char*>());
  if (doc.containsKey("dns"))  cfg_dns  = String(doc["dns"].as<const char*>());
  return true;
}

// Onboard RGB NeoPixel (Adafruit QT Py ESP32-S3)
// Prefer board-defined PIN_NEOPIXEL when available; fallback is a common default.
static const uint8_t NEOPIXEL_COUNT = 1;
static Adafruit_NeoPixel pixel(NEOPIXEL_COUNT, 21, NEO_GRB + NEO_KHZ800);

static void setPixelColor(uint32_t rgb) {
  pixel.setPixelColor(0, rgb);
  pixel.show();
}

static uint32_t parseHexColor(const String &hex) {
  // Accept: RRGGBB (6 hex chars). If invalid, return black.
  if (hex.length() != 6) return 0;
  char *endptr = nullptr;
  uint32_t value = strtoul(hex.c_str(), &endptr, 16);
  if (endptr == nullptr || *endptr != '\0') return 0;
  uint8_t r = (value >> 16) & 0xFF;
  uint8_t g = (value >> 8) & 0xFF;
  uint8_t b = (value) & 0xFF;
  return pixel.Color(r, g, b);
}

static String getContentType(const String &path) {
  if (path.endsWith(".htm") || path.endsWith(".html")) return "text/html";
  if (path.endsWith(".css")) return "text/css";
  if (path.endsWith(".json")) return "application/json";
  if (path.endsWith(".js")) return "application/javascript";
  if (path.endsWith(".png")) return "image/png";
  if (path.endsWith(".gif")) return "image/gif";
  if (path.endsWith(".jpg") || path.endsWith(".jpeg")) return "image/jpeg";
  if (path.endsWith(".ico")) return "image/x-icon";
  if (path.endsWith(".svg")) return "image/svg+xml";
  if (path.endsWith(".txt")) return "text/plain";
  return "application/octet-stream";
}

static bool handleFileRead(String path) {
  if (!fsMounted) return false;
  if (path.endsWith("/")) path += "index.html";
  String contentType = getContentType(path);
  if (!SPIFFS.exists(path)) return false;
  File file = SPIFFS.open(path, "r");
  server.streamFile(file, contentType);
  file.close();
  return true;
}

static void handleRoot() {
  if (!handleFileRead("/")) {
    server.send(404, "text/plain", "index.html non trovato");
  }
}

// Upload support
static File g_uploadFile;
static String g_uploadName;
static void handleFileUpload() {
  HTTPUpload &upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    g_uploadName = upload.filename;
    if (!g_uploadName.startsWith("/")) g_uploadName = "/" + g_uploadName;
    Serial.printf("Upload start: %s size?\n", g_uploadName.c_str());
    if (SPIFFS.exists(g_uploadName)) SPIFFS.remove(g_uploadName);
    g_uploadFile = SPIFFS.open(g_uploadName, "w");
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (g_uploadFile) g_uploadFile.write(upload.buf, upload.currentSize);
  } else if (upload.status == UPLOAD_FILE_END) {
    if (g_uploadFile) g_uploadFile.close();
    Serial.printf("Upload end: %s (%u bytes)\n", g_uploadName.c_str(), upload.totalSize);
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Serial.println("Upload aborted");
    if (g_uploadFile) { g_uploadFile.close(); }
    if (!g_uploadName.isEmpty() && SPIFFS.exists(g_uploadName)) SPIFFS.remove(g_uploadName);
  }
}

static void handleList() {
  String out;
  out.reserve(512);
  out += "Files in SPIFFS (root):\n";
  File root = SPIFFS.open("/");
  if (!root || !root.isDirectory()) {
    server.send(500, "text/plain", "SPIFFS non montato o root non valido");
    return;
  }
  File file = root.openNextFile();
  while (file) {
    out += String(file.name()) + "\t" + String(file.size()) + " bytes\n";
    file = root.openNextFile();
  }
  server.send(200, "text/plain", out);
}

void setup() {
  Serial.begin(115200);
  // Small delay so early logs are visible on monitor attach
  delay(500);

  // Mount SPIFFS using default params (auto-detect partition)
  {
    // Debug: ensure a 'spiffs' partition exists in the table
    const esp_partition_t* p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                       ESP_PARTITION_SUBTYPE_DATA_SPIFFS,
                                                       nullptr);
    if (!p) {
      Serial.println("Partition 'spiffs' non trovata nella partition table");
    } else {
      Serial.printf("Partition 'spiffs' trovata @0x%06x size=%u bytes\n", (unsigned)p->address, (unsigned)p->size);
    }
  }
  if (!SPIFFS.begin(false)) {
    Serial.println("SPIFFS: mount fallita (no auto-format). Provo a formattare...");
    if (SPIFFS.begin(true)) {
      fsMounted = true;
      Serial.println("SPIFFS: formato e montato. Ricarica i file con 'uploadfs'.");
    } else {
      fsMounted = false;
      Serial.println("SPIFFS: mount ancora fallita.");
    }
  } else {
    fsMounted = true;
    Serial.printf("SPIFFS montato. Usato: %u / %u bytes\n", SPIFFS.usedBytes(), SPIFFS.totalBytes());
    Serial.printf("index.html esiste? %s\n", SPIFFS.exists("/index.html") ? "si" : "no");
  }

  // Init onboard NeoPixel early for startup indications
  #ifdef PIN_NEOPIXEL_POWER
    pinMode(PIN_NEOPIXEL_POWER, OUTPUT);
    digitalWrite(PIN_NEOPIXEL_POWER, HIGH);
  #endif
  pixel.begin();
  pixel.setBrightness(64);
  setPixelColor(pixel.Color(0, 0, 0)); // off

  // Load configuration from SPIFFS and choose mode
  const bool haveConfig = loadConfig();
  if (haveConfig) {
    // Station mode using stored configuration
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(WIFI_HOST);

    // Indicate waiting for WiFi with RED
    setPixelColor(pixel.Color(255, 0, 0));

    // Configure static IP if requested, otherwise ensure DHCP
    if (!cfg_dhcp) {
      IPAddress ip, gw, mask, dns;
      bool ok = parseIP(cfg_ip, ip) && parseIP(cfg_gw, gw) && parseIP(cfg_mask, mask);
      if (!cfg_dns.isEmpty()) parseIP(cfg_dns, dns);
      if (ok) {
        if ((uint32_t)dns != 0) WiFi.config(ip, gw, mask, dns); else WiFi.config(ip, gw, mask);
        Serial.printf("IP statico: %s gw:%s mask:%s dns:%s\n", ip.toString().c_str(), gw.toString().c_str(), mask.toString().c_str(), dns.toString().c_str());
      } else {
        Serial.println("IP statico non valido. Procedo con DHCP.");
        WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);
      }
    } else {
      WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);
    }

    // Connect to WiFi using configured credentials
    WiFi.begin(cfg_ssid.c_str(), cfg_pass.c_str());

    Serial.print("Connessione al WiFi ");
    Serial.print(cfg_ssid);
    Serial.print("...");

    // Wait for connection
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED) {
      delay(500);
      Serial.print(".");
      if (millis() - start > 30000) { // 30s timeout feedback
        Serial.println("\nTimeout di connessione. Riprovo...");
        start = millis();
        WiFi.disconnect();
        delay(200);
        WiFi.begin(cfg_ssid.c_str(), cfg_pass.c_str());
      }
    }

    Serial.println();
    Serial.print("Connesso! IP: ");
    Serial.println(WiFi.localIP());
    Serial.print("Hostname: ");
    Serial.println(WiFi.getHostname());

    // Turn off LED after connection (end of startup)
    setPixelColor(pixel.Color(0, 0, 0));
  } else {
    // Access Point mode (setup)
    WiFi.mode(WIFI_AP);
    const char* AP_SSID = "ESP-FH4R2-Setup";
    const char* AP_PASS = "12345678"; // minimo 8 caratteri per WPA2
    bool ok = WiFi.softAP(AP_SSID, AP_PASS);
    IPAddress apIP = WiFi.softAPIP();
    Serial.printf("AP di setup %s. SSID: %s  IP: %s\n", ok ? "attivo" : "fallito", AP_SSID, apIP.toString().c_str());

    // Indicate AP mode with BLUE
    setPixelColor(pixel.Color(0, 0, 255));
  }

  // HTTP routes
  // Static file serving handled by handlers below
  server.on("/", HTTP_GET, handleRoot);
  server.on("/index.html", HTTP_GET, handleRoot);
  server.on("/ls", HTTP_GET, handleList);
  // File management APIs
  server.on("/api/delete", HTTP_POST, [](){
    if (!fsMounted) { server.send(500, "application/json", "{\"ok\":false,\"error\":\"fs not mounted\"}"); return; }
    if (!server.hasArg("path")) { server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing path\"}"); return; }
    String path = server.arg("path");
    if (!path.startsWith("/")) path = String("/") + path;
    if (!SPIFFS.exists(path)) { server.send(404, "application/json", "{\"ok\":false,\"error\":\"not found\"}"); return; }
    bool ok = SPIFFS.remove(path);
    server.send(ok ? 200 : 500, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
  });
  server.on("/api/download", HTTP_GET, [](){
    if (!fsMounted) { server.send(500, "text/plain", "fs not mounted"); return; }
    if (!server.hasArg("path")) { server.send(400, "text/plain", "missing path"); return; }
    String path = server.arg("path");
    if (!path.startsWith("/")) path = String("/") + path;
    if (!SPIFFS.exists(path)) { server.send(404, "text/plain", "not found"); return; }
    File file = SPIFFS.open(path, "r");
    String name = path;
    int slash = name.lastIndexOf('/');
    if (slash >= 0 && slash + 1 < (int)name.length()) name = name.substring(slash + 1);
    server.sendHeader("Content-Disposition", String("attachment; filename=\"") + name + "\"");
    server.streamFile(file, "application/octet-stream");
    file.close();
  });
  server.on("/upload", HTTP_POST, [](){
    server.send(200, "application/json", "{\"ok\":true}\n");
  }, handleFileUpload);
  // Config APIs
  server.on("/api/get_config", HTTP_GET, [](){
    if (!fsMounted) { server.send(500, "application/json", "{\"ok\":false,\"error\":\"fs not mounted\"}"); return; }
    StaticJsonDocument<512> doc;
    doc["dhcp"] = cfg_dhcp;
    doc["ssid"] = cfg_ssid;
    doc["pass"] = cfg_pass;
    doc["ip"]   = cfg_ip;
    doc["mask"] = cfg_mask;
    doc["gw"]   = cfg_gw;
    doc["dns"]  = cfg_dns;
    String out; out.reserve(256);
    serializeJson(doc, out);
    server.send(200, "application/json", out);
  });
  server.on("/api/save_config", HTTP_POST, [](){
    if (!fsMounted) { server.send(500, "application/json", "{\"ok\":false,\"error\":\"fs not mounted\"}"); return; }
    if (!server.hasArg("plain")) { server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing body\"}"); return; }
    StaticJsonDocument<512> doc;
    DeserializationError err = deserializeJson(doc, server.arg("plain"));
    if (err) { server.send(400, "application/json", String("{\"ok\":false,\"error\":\"json ")+err.c_str()+"\"}"); return; }
    if (doc.containsKey("dhcp")) cfg_dhcp = doc["dhcp"].as<bool>();
    if (doc.containsKey("ssid")) cfg_ssid = String(doc["ssid" ].as<const char*>());
    if (doc.containsKey("pass")) cfg_pass = String(doc["pass" ].as<const char*>());
    if (doc.containsKey("ip"))   cfg_ip   = String(doc["ip"   ].as<const char*>());
    if (doc.containsKey("mask")) cfg_mask = String(doc["mask" ].as<const char*>());
    if (doc.containsKey("gw"))   cfg_gw   = String(doc["gw"   ].as<const char*>());
    if (doc.containsKey("dns"))  cfg_dns  = String(doc["dns"  ].as<const char*>());
    saveConfig();
    server.send(200, "application/json", "{\"ok\":true}\n");
  });
  server.on("/api/reboot", HTTP_POST, [](){
    server.send(200, "application/json", "{\"ok\":true,\"msg\":\"rebooting\"}\n");
    delay(150);
    ESP.restart();
  });
  // Set color via: /color?hex=RRGGBB (no leading '#')
  server.on("/color", HTTP_ANY, [](){
    if (!server.hasArg("hex")) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing hex param\"}");
      return;
    }
    String hex = server.arg("hex");
    if (hex.startsWith("#")) hex.remove(0, 1);
    uint32_t c = parseHexColor(hex);
    setPixelColor(c);
    String resp = String("{\"ok\":true,\"hex\":\"") + hex + "\"}";
    server.send(200, "application/json", resp);
  });
  server.on("/fsinfo", HTTP_GET, [](){
    if (!fsMounted) {
      server.send(200, "text/plain", "SPIFFS non montato");
      return;
    }
    String s;
    s.reserve(128);
    s += "totalBytes=" + String(SPIFFS.totalBytes()) + "\n";
    s += "usedBytes=" + String(SPIFFS.usedBytes()) + "\n";
    s += "indexExists=" + String(SPIFFS.exists("/index.html") ? "si" : "no") + "\n";
    server.send(200, "text/plain", s);
  });
  server.onNotFound([](){
    if (!handleFileRead(server.uri())) {
      server.send(404, "text/plain", "404 - Pagina non trovata");
    }
  });

  // Start HTTP server
  server.begin();
  Serial.println("WebServer avviato sulla porta 80");

  // FS summary after server start (visible even if early logs were missed)
  Serial.printf("FS montato ora: %s\n", fsMounted ? "si" : "no");
  if (fsMounted) {
    Serial.printf("SPIFFS used: %u / %u bytes\n", SPIFFS.usedBytes(), SPIFFS.totalBytes());
    Serial.printf("index.html esiste? %s\n", SPIFFS.exists("/index.html") ? "si" : "no");
  } else {
    Serial.println("Suggerimento: esegui 'uploadfs' e reset per caricare i file.");
  }
}

void loop() {
  server.handleClient();
}
