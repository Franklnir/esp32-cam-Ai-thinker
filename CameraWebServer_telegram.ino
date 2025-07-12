#include "esp_camera.h"
#include <WiFi.h>
#include <HTTPClient.h>         // Diperlukan untuk komunikasi HTTP/HTTPS
#include <WiFiClientSecure.h>   // Diperlukan untuk koneksi HTTPS ke API Telegram
#include <ArduinoJson.h>        // Diperlukan untuk parsing JSON dari API Telegram
#include <FS.h>                 // Diperlukan untuk sistem file (untuk SD_MMC)
#include <SD_MMC.h>             // Diperlukan untuk komunikasi SD_MMC (ESP32-CAM SD card)
#include <vector>               // Diperlukan untuk std::vector (menyimpan daftar path file)
// Tidak perlu lagi <driver/ledc.h> karena fungsionalitas flash sudah dihapus

// Ini harus sesuai dengan board ESP32-CAM Anda.
// #define CAMERA_MODEL_WROVER_KIT
// #define CAMERA_MODEL_ESP_EYE
// #define CAMERA_MODEL_ESP32S3_EYE
// #define CAMERA_MODEL_M5STACK_PSRAM
// #define CAMERA_MODEL_M5STACK_V2_PSRAM
// #define CAMERA_MODEL_M5STACK_WIDE
// #define CAMERA_MODEL_M5STACK_ESP32CAM
// #define CAMERA_MODEL_M5STACK_UNITCAM
// #define CAMERA_MODEL_M5STACK_CAMS3_UNIT
#define CAMERA_MODEL_AI_THINKER // Pilihan yang Anda gunakan
// #define CAMERA_MODEL_TTGO_T_JOURNAL
// #define CAMERA_MODEL_XIAO_ESP32S3
// ** Espressif Internal Boards **
// #define CAMERA_MODEL_ESP32_CAM_BOARD
// #define CAMERA_MODEL_ESP32S2_CAM_BOARD
// #define CAMERA_MODEL_ESP32S3_CAM_LCD
// #define CAMERA_MODEL_DFRobot_FireBeetle2_ESP32S3
// #define CAMERA_MODEL_DFRobot_Romeo_ESP32S3

#include "camera_pins.h" // Menggunakan file camera_pins.h yang berisi definisi pin

// ===========================
// Kredensial WiFi Anda
// ===========================
const char *ssid = "GEORGIA";       // Ganti dengan nama WiFi Anda
const char *password = "Georgia12345"; // Ganti dengan password WiFi Anda

// ===========================
// Kredensial Bot Telegram Anda
// ===========================
const String BOT_TOKEN = "7833050640:AAFbapxzmp_RE_fNnmDssqByh7Ank19prKY"; // Token API bot Telegram Anda
const String CHAT_ID = "1995225615";   // Chat ID pribadi Anda (dari hasil getUpdates)

// ===========================
// Konfigurasi Static IP
// = Pastikan IP ini di luar jangkauan DHCP router Anda
// ===========================
IPAddress staticIP(192, 168, 1, 200);   // Alamat IP statis yang diinginkan
IPAddress gateway(192, 168, 1, 1);     // IP router Anda (gateway)
IPAddress subnet(255, 255, 255, 0);    // Subnet mask (standar untuk sebagian besar jaringan rumah)
IPAddress primaryDNS(8, 8, 8, 8);      // DNS Google (bisa juga IP router Anda)
IPAddress secondaryDNS(8, 8, 4, 4);    // DNS Google sekunder (opsional)

// ===========================
// Variabel untuk Bot Telegram
// ===========================
long lastUpdateID = 0; // Menyimpan ID update terakhir yang diproses dari Telegram
const unsigned long telegramCheckInterval = 5000; // Periksa pesan baru setiap 5 detik
unsigned long lastTelegramCheckTime = 0;

// ===========================
// Variabel untuk Perekaman Video ke SD card
// ===========================
bool isRecording = false;
String currentVideoSessionFolder = "";
unsigned long videoFrameCaptureInterval = 500; // Ambil frame video setiap 0.5 detik (2 FPS)
unsigned long lastVideoFrameCaptureTime = 0;
int frameCounter = 0;
std::vector<String> recordedFramePaths; // Menyimpan path file frame yang direkam untuk pratinjau
const int MAX_PREVIEW_FRAMES = 5; // Jumlah frame terakhir yang akan dikirim sebagai pratinjau saat "stop video"

// ===========================
// Deklarasi Fungsi (dari CameraWebServer dan tambahan Telegram)
// ===========================
void startCameraServer(); // Fungsi untuk memulai server web kamera (dari contoh CameraWebServer)
// void setupLedFlash(int pin); // Dihapus karena flash dinonaktifkan

// Fungsi untuk mengirim pesan teks ke Telegram
void sendTelegramMessage(String message) {
  WiFiClientSecure client;
  client.setInsecure(); // Peringatan: Mengabaikan verifikasi sertifikat SSL! Hanya untuk pengembangan.
  HTTPClient http;

  String telegramUrl = "https://api.telegram.org/bot" + BOT_TOKEN + "/sendMessage";
  http.begin(client, telegramUrl);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  String postData = "chat_id=" + CHAT_ID + "&text=" + message;
  int httpCode = http.POST(postData);

  if (httpCode > 0) {
    Serial.printf("[HTTP] POST Message... code: %d\n", httpCode);
    if (httpCode == HTTP_CODE_OK) {
      String payload = http.getString();
      Serial.println(payload);
    }
  } else {
    Serial.printf("[HTTP] POST Message... failed, error: %s\n", http.errorToString(httpCode).c_str());
  }
  http.end();
}

// Fungsi untuk mendapatkan nama resolusi string
String getResolutionName(framesize_t resolution) {
  switch (resolution) {
    case FRAMESIZE_QQVGA: return "QQVGA (160x120)";
    case FRAMESIZE_QCIF: return "QCIF (176x144)";
    case FRAMESIZE_HQVGA: return "HQVGA (240x160)";
    case FRAMESIZE_QVGA: return "QVGA (320x240)";
    case FRAMESIZE_CIF: return "CIF (352x288)";
    case FRAMESIZE_HVGA: return "HVGA (480x320)";
    case FRAMESIZE_VGA: return "VGA (640x480)";
    case FRAMESIZE_SVGA: return "SVGA (800x600)";
    case FRAMESIZE_XGA: return "XGA (1024x768)";
    case FRAMESIZE_SXGA: return "SXGA (1280x960)";
    case FRAMESIZE_UXGA: return "UXGA (1600x1200)";
    default: return "Unknown";
  }
}

// Fungsi untuk mengirim gambar ke Telegram dengan resolusi tertentu (tanpa flash)
void sendTelegramPhoto(framesize_t resolution) {
  sensor_t *s = esp_camera_sensor_get();
  if (s == NULL) {
    Serial.println("Sensor kamera tidak tersedia.");
    sendTelegramMessage("Error: Sensor kamera tidak tersedia!");
    return;
  }

  // Simpan resolusi saat ini sebelum mengubahnya
  framesize_t originalResolution = s->status.framesize;

  // Atur resolusi kamera sesuai permintaan
  if (s->set_framesize(s, resolution) != ESP_OK) {
    Serial.printf("Gagal mengatur resolusi ke %d\n", resolution);
    sendTelegramMessage("Gagal mengatur resolusi kamera.");
    // Coba kembalikan ke resolusi asli jika gagal
    s->set_framesize(s, originalResolution);
    return;
  }
  
  Serial.printf("Mengatur resolusi kamera ke %s\n", getResolutionName(resolution).c_str());

  camera_fb_t * fb = NULL;
  fb = esp_camera_fb_get(); // Ambil frame buffer dari kamera
  if (!fb) {
    Serial.println("Camera capture failed");
    sendTelegramMessage("Gagal mengambil gambar dari kamera!");
    // Kembalikan ke resolusi asli
    s->set_framesize(s, originalResolution);
    return;
  }

  Serial.printf("Mengambil gambar (%d bytes). Mengirim ke Telegram...\n", fb->len);

  WiFiClientSecure client;
  client.setInsecure(); // Peringatan: Mengabaikan verifikasi sertifikat SSL! Hanya untuk pengembangan.
  HTTPClient http;

  String telegramUrl = "https://api.telegram.org/bot" + BOT_TOKEN + "/sendPhoto";
  http.begin(client, telegramUrl); // Mulai koneksi HTTPS

  // Siapkan header untuk multipart form data
  String boundary = "----WebKitFormBoundary7MA4YWxkTrZu0gW"; // Batas unik
  http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);

  // Buat bagian header dari body POST
  String header = "--" + boundary + "\r\n";
  header += "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n" + CHAT_ID + "\r\n";
  header += "--" + boundary + "\r\n";
  header += "Content-Disposition: form-data; name=\"photo\"; filename=\"image.jpg\"\r\n";
  header += "Content-Type: image/jpeg\r\n\r\n";

  // Buat bagian footer dari body POST
  String footer = "\r\n--" + boundary + "--\r\n";

  // Hitung panjang total body POST
  int totalLength = header.length() + fb->len + footer.length();

  // Alokasikan memori untuk seluruh payload POST
  uint8_t *postBody = (uint8_t *) malloc(totalLength);
  if (!postBody) {
    Serial.println("Gagal mengalokasikan memori untuk payload POST!");
    esp_camera_fb_return(fb);
    // Kembalikan ke resolusi asli
    s->set_framesize(s, originalResolution);
    return;
  }

  // Salin header ke buffer
  memcpy(postBody, header.c_str(), header.length());
  // Salin data gambar ke buffer setelah header
  memcpy(postBody + header.length(), fb->buf, fb->len);
  // Salin footer ke buffer setelah data gambar
  memcpy(postBody + header.length() + fb->len, footer.c_str(), footer.length());

  // Kirim seluruh payload dalam satu panggilan POST
  int httpCode = http.POST(postBody, totalLength);

  // Bebaskan memori yang dialokasikan
  free(postBody);

  if (httpCode == HTTP_CODE_OK) {
    Serial.println("Gambar berhasil dikirim ke Telegram!");
    String payload = http.getString();
    Serial.println(payload);
  } else {
    Serial.printf("Error mengirim gambar ke Telegram: %d - %s\n", httpCode, http.errorToString(httpCode).c_str());
  }

  esp_camera_fb_return(fb); // Kembalikan buffer gambar setelah digunakan
  http.end(); // Akhiri sesi HTTP

  // Kembalikan resolusi kamera ke pengaturan awal (XGA) setelah mengambil gambar
  s->set_framesize(s, originalResolution);
  Serial.printf("Mengembalikan resolusi kamera ke %s\n", getResolutionName(originalResolution).c_str());
}

// Fungsi untuk mengirim gambar yang tersimpan di SD card ke Telegram
void sendTelegramPhotoFromFile(String filePath) {
  File file = SD_MMC.open(filePath, FILE_READ);
  if (!file) {
    Serial.printf("Gagal membuka file %s dari SD card\n", filePath.c_str());
    sendTelegramMessage("Gagal mengirim pratinjau: File tidak ditemukan di SD card.");
    return;
  }

  Serial.printf("Membaca file %s (%d bytes). Mengirim ke Telegram...\n", filePath.c_str(), file.size());

  WiFiClientSecure client;
  client.setInsecure(); // Peringatan: Mengabaikan verifikasi sertifikat SSL!
  HTTPClient http;

  String telegramUrl = "https://api.telegram.org/bot" + BOT_TOKEN + "/sendPhoto";
  http.begin(client, telegramUrl);
  String boundary = "----WebKitFormBoundary7MA4YWxkTrZu0gW";
  http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);

  String header = "--" + boundary + "\r\n";
  header += "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n" + CHAT_ID + "\r\n";
  header += "--" + boundary + "\r\n";
  header += "Content-Disposition: form-data; name=\"photo\"; filename=\"" + filePath.substring(filePath.lastIndexOf('/') + 1) + "\"\r\n";
  header += "Content-Type: image/jpeg\r\n\r\n";

  String footer = "\r\n--" + boundary + "--\r\n";

  int totalLength = header.length() + file.size() + footer.length();

  uint8_t *postBody = (uint8_t *) malloc(totalLength);
  if (!postBody) {
    Serial.println("Gagal mengalokasikan memori untuk payload POST (dari file)!");
    file.close();
    return;
  }

  // Salin header
  memcpy(postBody, header.c_str(), header.length());
  // Salin data file
  file.readBytes((char*)(postBody + header.length()), file.size());
  // Salin footer
  memcpy(postBody + header.length() + file.size(), footer.c_str(), footer.length());
  file.close();

  int httpCode = http.POST(postBody, totalLength);
  free(postBody);

  if (httpCode == HTTP_CODE_OK) {
    Serial.println("Gambar dari SD berhasil dikirim ke Telegram!");
    String payload = http.getString();
    Serial.println(payload);
  } else {
    Serial.printf("Error mengirim gambar dari SD ke Telegram: %d - %s\n", httpCode, http.errorToString(httpCode).c_str());
  }
  http.end();
}

// Fungsi untuk membuat nama folder unik berdasarkan timestamp
String createTimestampFolder() {
  time_t now;
  struct tm timeinfo;
  char strftime_buf[30];

  // Pastikan waktu telah disinkronkan. Jika tidak, waktu akan acak.
  // Untuk penggunaan offline sejati, Anda perlu modul RTC eksternal.
  // Untuk saat ini, kita anggap waktu sudah berjalan (misal: setelah koneksi WiFi).
  time(&now);
  localtime_r(&now, &timeinfo);
  strftime(strftime_buf, sizeof(strftime_buf), "/%Y%m%d_%H%M%S", &timeinfo);
  return String(strftime_buf);
}

// Fungsi untuk memulai perekaman video ke SD card
void startVideoRecording() {
  if (isRecording) {
    sendTelegramMessage("Perekaman video sudah berjalan. Kirim 'stop video' dulu.");
    return;
  }

  // Buat nama folder baru
  currentVideoSessionFolder = createTimestampFolder();
  if (!SD_MMC.mkdir(currentVideoSessionFolder)) {
    Serial.printf("Gagal membuat folder SD: %s\n", currentVideoSessionFolder.c_str());
    sendTelegramMessage("Gagal memulai perekaman: Tidak bisa membuat folder di SD card.");
    return;
  }
  
  isRecording = true;
  frameCounter = 0;
  recordedFramePaths.clear(); // Bersihkan daftar frame sebelumnya
  lastVideoFrameCaptureTime = millis(); // Reset timer
  sendTelegramMessage("Video recording started. Saving frames to: " + currentVideoSessionFolder);
  Serial.printf("Perekaman video dimulai. Folder: %s\n", currentVideoSessionFolder.c_str());
}

// Fungsi untuk menghentikan perekaman video dan mengirim pratinjau
void stopVideoRecording() {
  if (!isRecording) {
    sendTelegramMessage("Perekaman video tidak sedang berjalan. Kirim 'start video' dulu.");
    return;
  }

  isRecording = false;
  Serial.printf("Perekaman video dihentikan. Total frame: %d\n", frameCounter);
  sendTelegramMessage("Video recording stopped. Total frames: " + String(frameCounter) + ". Full video on SD card.");
  
  // Kirim pratinjau beberapa frame terakhir
  if (recordedFramePaths.empty()) {
    sendTelegramMessage("Tidak ada frame yang terekam untuk pratinjau.");
  } else {
    sendTelegramMessage("Mengirim " + String(recordedFramePaths.size()) + " frame terakhir sebagai pratinjau...");
    for (const String& path : recordedFramePaths) {
      sendTelegramPhotoFromFile(path);
      delay(500); // Beri jeda antar pengiriman foto
    }
  }
  recordedFramePaths.clear(); // Bersihkan setelah dikirim
}


// Fungsi untuk memeriksa dan memproses pesan baru dari Telegram
void handleNewMessages() {
  WiFiClientSecure client;
  client.setInsecure(); // Peringatan: Mengabaikan verifikasi sertifikat SSL! Hanya untuk pengembangan.
  HTTPClient http;

  // Gunakan offset untuk hanya mendapatkan pesan baru
  String telegramUrl = "https://api.telegram.org/bot" + BOT_TOKEN + "/getUpdates?offset=" + (lastUpdateID + 1);
  http.begin(client, telegramUrl);

  int httpCode = http.GET();
  if (httpCode > 0) {
    Serial.printf("[HTTP] GET Updates... code: %d\n", httpCode);
    if (httpCode == HTTP_CODE_OK) {
      String payload = http.getString();
      // Serial.println("Telegram API Response:"); // Terlalu banyak output jika sering polling
      // Serial.println(payload);

      // Parsing JSON menggunakan ArduinoJson
      StaticJsonDocument<2048> doc; // Ukuran buffer JSON, sesuaikan jika perlu
      DeserializationError error = deserializeJson(doc, payload);

      if (error) {
        Serial.print(F("deserializeJson() failed: "));
        Serial.println(error.f_str());
        return;
      }

      JsonArray result = doc["result"].as<JsonArray>();

      for (JsonObject update : result) {
        long currentUpdateID = update["update_id"];
        if (currentUpdateID > lastUpdateID) { // Pastikan ini adalah update yang baru
          lastUpdateID = currentUpdateID;

          if (update.containsKey("message")) {
            JsonObject message = update["message"];
            String messageText = message["text"].as<String>();
            long messageChatId = message["chat"]["id"].as<long>(); // ID chat dari pengirim pesan

            Serial.printf("Pesan baru dari Chat ID %ld: %s\n", messageChatId, messageText.c_str());

            // Pastikan pesan datang dari CHAT_ID yang kita inginkan
            if (String(messageChatId) == CHAT_ID) {
              // Ubah perintah ke huruf kecil untuk perbandingan yang tidak case-sensitive
              messageText.toLowerCase();

              if (messageText == "/fotobagus" || messageText == "foto bagus") {
                sendTelegramMessage("Memproses permintaan foto resolusi XGA...");
                sendTelegramPhoto(FRAMESIZE_XGA); 
              } else if (messageText == "/fotorungan" || messageText == "foto ringan") {
                sendTelegramMessage("Memproses permintaan foto resolusi HVGA...");
                sendTelegramPhoto(FRAMESIZE_HVGA); 
              } else if (messageText == "/foto9" || messageText == "foto 9") {
                sendTelegramMessage("Memproses permintaan 9 foto QCIF...");
                for (int i = 0; i < 9; i++) {
                  sendTelegramPhoto(FRAMESIZE_QCIF);
                  delay(1500); // Jeda antar foto agar tidak terlalu cepat dan membebani
                }
                sendTelegramMessage("Selesai mengirim 9 foto QCIF.");
              }
              else if (messageText == "/startvideo" || messageText == "start video") {
                startVideoRecording();
              } else if (messageText == "/stopvideo" || messageText == "stop video") {
                stopVideoRecording();
              } else if (messageText == "/status" || messageText == "status") {
                String statusMsg = "ESP32-CAM Status:\n";
                statusMsg += "IP Lokal: " + WiFi.localIP().toString() + "\n";
                statusMsg += "Kekuatan Sinyal WiFi (RSSI): " + String(WiFi.RSSI()) + " dBm\n";
                
                sensor_t *s = esp_camera_sensor_get();
                if (s != NULL) {
                  statusMsg += "Resolusi Kamera Saat Ini: " + getResolutionName(s->status.framesize) + "\n";
                  statusMsg += "V-Flip: " + String(s->status.vflip ? "Aktif" : "Nonaktif") + "\n";
                  statusMsg += "H-Mirror: " + String(s->status.hmirror ? "Aktif" : "Nonaktif") + "\n";
                } else {
                  statusMsg += "Status Kamera: Tidak Tersedia\n";
                }

                statusMsg += "Status Rekaman Video: " + String(isRecording ? "AKTIF" : "NONAKTIF");
                if (isRecording) {
                  statusMsg += " (" + String(frameCounter) + " frames direkam)\n";
                  statusMsg += "Folder Rekaman: " + currentVideoSessionFolder + "\n";
                } else {
                  statusMsg += "\n"; // Tambahkan baris baru jika tidak merekam untuk konsistensi
                }
                
                // Pastikan SD_MMC sudah diinisialisasi sebelum mengaksesnya
                if (SD_MMC.cardType() != CARD_NONE) {
                  uint64_t totalBytes = SD_MMC.totalBytes() / (1024 * 1024);
                  uint64_t usedBytes = SD_MMC.usedBytes() / (1024 * 1024);
                  statusMsg += "SD Card:\n";
                  statusMsg += "  Total: " + String((unsigned long)totalBytes) + "MB\n";
                  statusMsg += "  Digunakan: " + String((unsigned long)usedBytes) + "MB\n";
                  statusMsg += "  Tersedia: " + String((unsigned long)(totalBytes - usedBytes)) + "MB\n";
                } else {
                  statusMsg += "SD Card: Tidak Terpasang atau Gagal Inisialisasi\n";
                }


                statusMsg += "\nPerintah Tersedia:\n";
                statusMsg += "- 'foto bagus' (Kirim foto XGA)\n";
                statusMsg += "- 'foto ringan' (Kirim foto HVGA)\n";
                statusMsg += "- 'foto 9' (Kirim 9 foto QCIF)\n";
                statusMsg += "- 'start video' (Mulai rekam video ke SD)\n";
                statusMsg += "- 'stop video' (Hentikan rekam video & kirim pratinjau)\n";
                statusMsg += "- 'status' (Tampilkan status ini)";
                
                sendTelegramMessage(statusMsg);
              } else {
                sendTelegramMessage("Perintah tidak dikenali. Coba:\n"
                                    "- 'foto bagus'\n"
                                    "- 'foto ringan'\n"
                                    "- 'foto 9'\n"
                                    "- 'start video'\n"
                                    "- 'stop video'\n"
                                    "- 'status'");
              }
            } else {
              Serial.printf("Pesan dari Chat ID tidak dikenal: %ld\n", messageChatId);
              // Anda bisa menambahkan sendTelegramMessage ke Chat ID yang tidak dikenal di sini jika mau
            }
          }
        }
      }
    }
  } else {
    Serial.printf("[HTTP] GET Updates... failed, error: %s\n", http.errorToString(httpCode).c_str());
  }
  http.end();
}


void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println("\n");
  Serial.println("Memulai ESP32-CAM dengan Integrasi Telegram dan Web Server...");

  // Set konfigurasi IP statis
  if (!WiFi.config(staticIP, gateway, subnet, primaryDNS, secondaryDNS)) {
    Serial.println("Gagal mengkonfigurasi IP statis!");
  }

  // Mulai koneksi WiFi
  WiFi.begin(ssid, password);
  WiFi.setSleep(false); // Menonaktifkan mode tidur WiFi untuk menjaga koneksi tetap aktif

  Serial.print("Menghubungkan ke WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.println("WiFi terhubung!");
  Serial.print("Alamat IP: ");
  Serial.println(WiFi.localIP());

  // === Inisialisasi Kamera (dari kode CameraWebServer) ===
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.frame_size = FRAMESIZE_UXGA; // Resolusi awal tinggi
  config.pixel_format = PIXFORMAT_JPEG; // Untuk streaming
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 10; // Kualitas JPEG default
  config.fb_count = 1;

  // Jika PSRAM IC ada, gunakan konfigurasi yang lebih tinggi
  if (config.pixel_format == PIXFORMAT_JPEG) {
    if (psramFound()) {
      config.jpeg_quality = 10;
      config.fb_count = 2;
      config.grab_mode = CAMERA_GRAB_LATEST;
    } else {
      // Batasi ukuran frame jika PSRAM tidak tersedia
      config.frame_size = FRAMESIZE_SVGA;
      config.fb_location = CAMERA_FB_IN_DRAM;
    }
  } else {
    // Pilihan terbaik untuk deteksi/pengenalan wajah (tidak aktif di sini)
    config.frame_size = FRAMESIZE_240X240;
    #if CONFIG_IDF_TARGET_ESP32S3
    config.fb_count = 2;
    #endif
  }

  // Camera init
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x\n", err);
    sendTelegramMessage("ERROR: Inisialisasi kamera gagal! Memulai ulang...");
    ESP.restart(); // Restart ESP jika kamera gagal diinisialisasi
  }
  Serial.println("Kamera berhasil diinisialisasi.");

  sensor_t *s = esp_camera_sensor_get();
  // Setelan sensor tambahan: flip vertikal dan mirror horizontal
  // Ini adalah pengaturan yang Anda minta sebelumnya, diterapkan ke semua model
  s->set_vflip(s, 1);      // Membalik gambar secara vertikal
  s->set_hmirror(s, 1);    // Mencerminkan gambar secara horizontal

  // Atur ulang ukuran frame ke QVGA jika JPEG, untuk frame rate awal yang lebih tinggi
  // Ini dari kode CameraWebServer, dan sendTelegramPhoto akan mengubahnya jika diperlukan.
  if (config.pixel_format == PIXFORMAT_JPEG) {
    s->set_framesize(s, FRAMESIZE_QVGA);
  }
  // === Akhir Inisialisasi Kamera ===

  // Inisialisasi SD card
  if (!SD_MMC.begin()) {
    Serial.println("Gagal menginisialisasi SD card!");
    sendTelegramMessage("Gagal menginisialisasi SD card. Fitur video tidak akan berfungsi.");
  } else {
    uint64_t cardSize = SD_MMC.cardSize() / (1024 * 1024);
    Serial.printf("SD Card terpasang, ukuran: %lluMB\n", cardSize);
    sendTelegramMessage("SD Card terinisialisasi. Ukuran: " + String((unsigned long)cardSize) + "MB");
  }

  // Matikan pin LED flash secara eksplisit saat startup
  // LED_GPIO_NUM biasanya didefinisikan di camera_pins.h.
  // Ini untuk mengatasi masalah flash yang terus menyala.
  #if defined(LED_GPIO_NUM) && (LED_GPIO_NUM != -1)
    pinMode(LED_GPIO_NUM, OUTPUT);
    digitalWrite(LED_GPIO_NUM, LOW); // Pastikan LED mati
    Serial.println("LED_GPIO_NUM set ke LOW untuk memastikan flash mati.");
  #endif
  
  // Kirim pesan ke Telegram saat online
  sendTelegramMessage("ESP32-CAM Anda sudah online dan siap menerima perintah!\n"
                      "Coba 'foto bagus', 'foto ringan', 'foto 9', 'start video', 'stop video', atau 'status'.\n"
                      "Anda juga bisa akses video stream di http://" + WiFi.localIP().toString() + "/");
  delay(2000); // Beri waktu untuk pesan terkirim

  // Mulai server kamera web (ini akan berjalan di task FreeRTOS terpisah)
  startCameraServer();

  Serial.print("Camera Web Server Ready! Gunakan 'http://");
  Serial.print(WiFi.localIP());
  Serial.println("' untuk terhubung.");
}

void loop() {
  // Loop utama akan digunakan untuk polling Telegram dan tugas non-blocking lainnya.
  // Server kamera web berjalan di task FreeRTOS terpisah.

  // Periksa koneksi WiFi secara berkala
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi terputus. Mencoba menghubungkan kembali...");
    WiFi.begin(ssid, password);
    unsigned long connectAttemptTime = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - connectAttemptTime < 30000) { // Coba selama 30 detik
      delay(500);
      Serial.print(".");
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\nWiFi terhubung kembali!");
      sendTelegramMessage("ESP32-CAM terhubung kembali. IP: " + WiFi.localIP().toString());
    } else {
      Serial.println("\nGagal menghubungkan kembali WiFi. Restarting...");
      ESP.restart(); // Restart jika gagal terhubung kembali
    }
  }

  // Jika perekaman video aktif, ambil dan simpan frame
  if (isRecording && (millis() - lastVideoFrameCaptureTime > videoFrameCaptureInterval)) {
    sensor_t *s = esp_camera_sensor_get();
    if (s == NULL) {
      Serial.println("Sensor kamera tidak tersedia saat perekaman.");
      isRecording = false; // Hentikan perekaman jika sensor tidak tersedia
      sendTelegramMessage("Error: Sensor kamera hilang saat merekam video.");
      return;
    }
    
    // Pastikan resolusi tetap SXGA saat merekam (atau resolusi yang Anda inginkan untuk video)
    // Hindari perubahan resolusi terlalu sering saat merekam.
    if (s->status.framesize != FRAMESIZE_SXGA) { // Anda bisa mengubah ini ke resolusi video default Anda
      s->set_framesize(s, FRAMESIZE_SXGA);
      Serial.println("Mengatur ulang resolusi ke SXGA untuk perekaman video.");
    }

    camera_fb_t * fb = NULL;
    fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Gagal mengambil frame video.");
      return;
    }

    String filePath = currentVideoSessionFolder + "/frame_" + String(frameCounter, DEC) + ".jpg";
    File file = SD_MMC.open(filePath.c_str(), FILE_WRITE);
    if (!file) {
      Serial.printf("Gagal membuka file %s untuk menulis.\n", filePath.c_str());
      esp_camera_fb_return(fb);
      return;
    }
    file.write(fb->buf, fb->len);
    file.close();
    Serial.printf("Frame %d disimpan ke %s\n", frameCounter, filePath.c_str());

    // Tambahkan path ke daftar pratinjau (pertahankan hanya MAX_PREVIEW_FRAMES terakhir)
    recordedFramePaths.push_back(filePath);
    if (recordedFramePaths.size() > MAX_PREVIEW_FRAMES) {
      recordedFramePaths.erase(recordedFramePaths.begin()); // Hapus yang tertua
    }

    esp_camera_fb_return(fb);
    frameCounter++;
    lastVideoFrameCaptureTime = millis();
  }

  // Periksa pesan baru dari Telegram secara berkala
  if (millis() - lastTelegramCheckTime > telegramCheckInterval) {
    handleNewMessages();
    lastTelegramCheckTime = millis();
  }

  delay(10); // Memberi sedikit waktu untuk tugas lain berjalan
}
