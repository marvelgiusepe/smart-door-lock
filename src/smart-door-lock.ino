#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ESP32Servo.h>
#include <time.h>
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"

// =============================================
//  KONFIGURASI — UBAH BAGIAN INI
// =============================================

#define WIFI_SSID       "esp33"
#define WIFI_PASSWORD   "12345678"

#define FIREBASE_API_KEY      "AIzaSyAodz5Nn06fJNhh8Yu9yRQdIlXTst7EAW4"
#define FIREBASE_DATABASE_URL "https://marvel-sayang-josephine-default-rtdb.asia-southeast1.firebasedatabase.app"

// UID kartu RFID yang diizinkan — scan kartu dulu, lihat UID di Serial Monitor
const String ALLOWED_CARDS[] = {
  "42008669",  // ganti dengan UID kartu kamu
  "D0050F5F"
};
const int CARD_COUNT = sizeof(ALLOWED_CARDS) / sizeof(ALLOWED_CARDS[0]);

// =============================================
//  PIN
// =============================================
#define SS_PIN    5
#define RST_PIN   4
#define SERVO_PIN 13
#define IR_PIN    14

// =============================================
//  OBJEK
// =============================================
MFRC522 mfrc522(SS_PIN, RST_PIN);
LiquidCrystal_I2C lcd(0x27, 16, 2);
Servo myservo;

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// =============================================
//  VARIABEL
// =============================================
bool doorOpen   = false;
bool signupOK   = false;
unsigned long lastCommandTime = 0;
unsigned long lastFirebaseCheck = 0;
#define FIREBASE_CHECK_INTERVAL 1000  // cek Firebase tiap 1 detik

// =============================================
//  SETUP
// =============================================
void setup() {
  Serial.begin(115200);
  delay(1000);

  // RFID
  SPI.begin(18, 19, 23, 5);
  mfrc522.PCD_Init();

  // IR
  pinMode(IR_PIN, INPUT);

  // LCD
  lcd.init();
  lcd.backlight();
  lcdPrint("Menghubungkan", "WiFi...");

  // Servo — posisi terkunci
  myservo.attach(SERVO_PIN);
  myservo.write(0);

  // WiFi
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Konek WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(300);
  }
  Serial.println();
  Serial.print("IP ESP32: ");
  Serial.println(WiFi.localIP());

  // NTP — untuk timestamp waktu yang akurat (WIB = UTC+7)
  configTime(7 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print("Sinkronisasi waktu");
  while (time(nullptr) < 100000) {
    Serial.print(".");
    delay(500);
  }
  Serial.println(" OK");

  // Firebase
  lcdPrint("Konek Firebase", "...");
  config.api_key = FIREBASE_API_KEY;
  config.database_url = FIREBASE_DATABASE_URL;
  config.token_status_callback = tokenStatusCallback;

  if (Firebase.signUp(&config, &auth, "", "")) {
    Serial.println("Firebase: OK");
    signupOK = true;
  } else {
    Serial.printf("Firebase Error: %s\n", config.signer.signupError.message.c_str());
  }

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  delay(2000);

  // Set status awal ke Firebase
  updateFirebaseStatus("closed");
  lcdPrint("Sistem Siap", "Scan Kartu...");
  Serial.println("Sistem siap!");
}

// =============================================
//  LOOP
// =============================================
void loop() {
  if (!Firebase.ready() || !signupOK) {
    delay(100);
    return;
  }

  // Cek perintah dari website (setiap 1 detik)
  if (millis() - lastFirebaseCheck >= FIREBASE_CHECK_INTERVAL) {
    lastFirebaseCheck = millis();
    checkFirebaseCommand();
  }

  // Cek kartu RFID
  if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
    handleRFID();
  }

  delay(50);
}

// =============================================
//  CEK PERINTAH DARI WEBSITE
// =============================================
void checkFirebaseCommand() {
  if (!Firebase.RTDB.getJSON(&fbdo, "/door/command")) return;

  FirebaseJson &json = fbdo.jsonObject();
  FirebaseJsonData result;

  json.get(result, "timestamp");
  long cmdTime = result.to<long>();

  // Hanya proses perintah baru
  if (cmdTime <= lastCommandTime) return;
  lastCommandTime = cmdTime;

  json.get(result, "action");
  String action = result.to<String>();

  Serial.println("Perintah website: " + action);

  if (action == "open") {
    openDoor("Website");
  } else if (action == "close") {
    closeDoor("Website");
  }
}

// =============================================
//  HANDLE RFID
// =============================================
void handleRFID() {
  // Baca UID
  String uid = "";
  for (byte i = 0; i < mfrc522.uid.size; i++) {
    if (mfrc522.uid.uidByte[i] < 0x10) uid += "0";
    uid += String(mfrc522.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();
  Serial.println("Kartu UID: " + uid);

  // Cek kartu
  bool allowed = false;
  for (int i = 0; i < CARD_COUNT; i++) {
    if (uid == ALLOWED_CARDS[i]) { allowed = true; break; }
  }

  if (allowed) {
    Serial.println("Kartu VALID");
    openDoor("RFID");

    // Kirim lastEvent ke Firebase (untuk website)
    FirebaseJson eventJson;
    eventJson.set("action", "open");
    eventJson.set("source", "rfid");
    eventJson.set("uid", uid);
    eventJson.set("timestamp", getTimestamp());
    Firebase.RTDB.setJSON(&fbdo, "/door/lastEvent", &eventJson);

    sendLog("Kartu RFID Diterima", "RFID", "Akses diizinkan", uid);

  } else {
    Serial.println("Kartu DITOLAK: " + uid);
    lcdPrint("Akses DITOLAK", uid.c_str());
    sendLog("Kartu RFID Ditolak", "RFID", "Akses ditolak", uid);
    delay(2000);
    lcdPrint("Sistem Siap", "Scan Kartu...");
  }

  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();
}

// =============================================
//  BUKA PINTU
// =============================================
void openDoor(String source) {
  doorOpen = true;

  lcdPrint("Akses OK", "Pintu Dibuka");
  Serial.println("Pintu DIBUKA oleh: " + source);

  myservo.write(90);  // buka
  updateFirebaseStatus("open");

  if (source != "RFID") {
    sendLog("Pintu Dibuka", source, "Dikontrol via " + source, "");
  }

  // Tunggu sampai IR sensor aman (tidak ada objek)
  while (digitalRead(IR_PIN) == LOW) {
    lcdPrint("Ada Objek...", "Menunggu aman");
    delay(200);
  }

  // Pintu aman, kunci kembali
  delay(1000); // jeda sebentar
  closeDoor(source);
}

// =============================================
//  KUNCI PINTU
// =============================================
void closeDoor(String source) {
  doorOpen = false;

  myservo.write(0);  // kunci
  updateFirebaseStatus("closed");

  lcdPrint("Pintu Terkunci", "Sistem Siap");
  Serial.println("Pintu TERKUNCI");

  if (source != "") {
    sendLog("Pintu Dikunci", source, "Dikunci oleh " + source, "");
  }

  delay(1000);
  lcdPrint("Sistem Siap", "Scan Kartu...");
}

// =============================================
//  FIREBASE HELPERS
// =============================================
void updateFirebaseStatus(String status) {
  Firebase.RTDB.setString(&fbdo, "/door/status", status);
}

void sendLog(String event, String source, String note, String uid) {
  FirebaseJson logJson;
  logJson.set("event", event);
  logJson.set("source", source);
  logJson.set("note", note);
  if (uid != "") logJson.set("uid", uid);
  logJson.set("rawTime", getTimestamp());
  logJson.set("time", getTimeString());
  Firebase.RTDB.pushJSON(&fbdo, "/logs", &logJson);

  // Update stats
  updateStats(source);
}

void updateStats(String source) {
  // Baca stats lama dulu
  long total = 0, web = 0, rfid = 0;
  if (Firebase.RTDB.getInt(&fbdo, "/stats/total")) total = fbdo.intData();
  if (Firebase.RTDB.getInt(&fbdo, "/stats/web"))   web   = fbdo.intData();
  if (Firebase.RTDB.getInt(&fbdo, "/stats/rfid"))  rfid  = fbdo.intData();

  total++;
  if (source == "WEB" || source == "Website") web++;
  if (source == "RFID") rfid++;

  Firebase.RTDB.setInt(&fbdo, "/stats/total", total);
  Firebase.RTDB.setInt(&fbdo, "/stats/web",   web);
  Firebase.RTDB.setInt(&fbdo, "/stats/rfid",  rfid);
}

// =============================================
//  LCD HELPER
// =============================================
void lcdPrint(const char* line1, const char* line2) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(line1);
  lcd.setCursor(0, 1);
  lcd.print(line2);
}

// =============================================
//  WAKTU
// =============================================
long getTimestamp() {
  return (long)time(nullptr) * 1000L;
}

String getTimeString() {
  time_t now = time(nullptr);
  struct tm* t = localtime(&now);
  char buf[25];
  strftime(buf, sizeof(buf), "%d %b %Y, %H:%M:%S", t);
  return String(buf);
}
