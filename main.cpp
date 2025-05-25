 #include <WiFi.h>
#include <WebServer.h>
#include <time.h>
#include <esp_sleep.h>

// Debug settings  // デバッグ設定
#define DEBUG_ENABLED true

// ADC and LED pin definitions  // ADCピンとLEDピンの定義
const int adcPins[4] = {34, 35, 32, 33}; // V1~V4
const int greenLED = 2;                   // Normal operation (sampling)  // 正常動作（サンプリング）用
const int redLED = 4;                     // Error (WiFi fail, etc.)     // エラー（WiFi接続失敗など）用

// Sampling and averaging settings  // サンプリングと平均化の設定
const int sampleIntervalSec = 10;        // Sample every 10 seconds      // 10秒ごとにサンプリング
const int averageIntervalMin = 10;       // 10-min averages              // 10分ごとの平均
const int totalHours = 12;               // 12 hours of data (memory saving) // 12時間分のデータ（メモリ節約）
const int samplesPerAvg = averageIntervalMin * 60 / sampleIntervalSec; // 60 samples/10min // 60サンプル/10分
const int totalSamples = totalHours * 60 * 60 / sampleIntervalSec;    // 4320 samples     // 4320サンプル
const int graphDataPoints = 30;           // Number of data points for graph // グラフに表示するデータ数
const int refreshIntervalMs = 60000;      // Data update interval (60sec)   // データ更新間隔（60秒）

// Data structures  // データ構造
struct SampleData {
  float voltage[4];
  time_t timestamp;
};
struct AvgData {
  float voltage[4];
  time_t timestamp;
};
SampleData voltageData[totalSamples];
AvgData avgVoltage[totalHours * 6]; // 6 averages per hour // 1時間あたり6個の平均値
int currentSample = 0;

// Web server and WiFi settings  // WebサーバーとWiFi設定
WebServer server(80);
const char* ssid = "YOUR_SSID";       // Change required (consider EEPROM or captive portal) // 要変更（EEPROMやCaptive Portalを検討）
const char* password = "YOUR_PASSWORD"; // Change required // 要変更
const char* pathRoot = "/";
const char* pathData = "/data";
const char* pathCsv = "/csv";

// ADC settings  // ADC設定
const float VOLTAGE_DIVIDER_RATIO = (100.0 + 15.0) / 15.0;
const float ADC_REF_VOLTAGE = 3.3;
const int ADC_RESOLUTION = 4095;
const int ADC_AVG_SAMPLES = 10; // Averaging samples for noise reduction // ノイズ低減のための平均サンプル数

// LED and WiFi connection management  // LEDとWiFi接続管理
unsigned long greenLedOnTime = 0;
const unsigned long greenLedDuration = 100; // Green LED blink duration (ms) // 緑LED点滅時間（ms）
unsigned long wifiConnectStartTime = 0;
const unsigned long wifiConnectTimeout = 30000; // WiFi connect timeout (ms) // WiFi接続タイムアウト（ms）
bool wifiConnecting = false;

void setup() {
  Serial.begin(115200);
  // Pin initialization  // ピンの初期化
  for (int i = 0; i < 4; i++) {
    pinMode(adcPins[i], INPUT);
    analogSetAttenuation(ADC_11db); // Voltage range: 0-~3.9V // 電圧範囲0-約3.9Vに対応
  }
  pinMode(greenLED, OUTPUT);
  pinMode(redLED, OUTPUT);
  digitalWrite(greenLED, LOW);
  digitalWrite(redLED, LOW);

  // Data arrays initialization  // データ配列の初期化
  for (int i = 0; i < totalSamples; i++) {
    for (int ch = 0; ch < 4; ch++) voltageData[i].voltage[ch] = 0.0;
    voltageData[i].timestamp = 0;
  }
  for (int i = 0; i < totalHours * 6; i++) {
    for (int ch = 0; ch < 4; ch++) avgVoltage[i].voltage[ch] = 0.0;
    avgVoltage[i].timestamp = 0;
  }

  connectToWiFi();
  setupWebServer();
  configTime(9 * 3600, 0, "ntp.nict.jp", "pool.ntp.org"); // JST // 日本標準時
}

void loop() {
  static unsigned long lastSample = 0;
  // Sampling  // サンプリング
  if (millis() - lastSample >= sampleIntervalSec * 1000) {
    sampleVoltages();
    lastSample = millis();
    digitalWrite(greenLED, HIGH); // Turn on green LED on sampling success // サンプリング成功時に緑LED点灯
    greenLedOnTime = millis();
    // Light sleep (keep WiFi and memory) // ライトスリープ（WiFiとメモリを保持）
    esp_sleep_enable_timer_wakeup(sampleIntervalSec * 1000000ULL);
    esp_light_sleep_start();
  }

  // Green LED blink control  // 緑LEDの点滅制御
  if (greenLedOnTime && millis() - greenLedOnTime >= greenLedDuration) {
    digitalWrite(greenLED, LOW);
    greenLedOnTime = 0;
  }

  // WiFi connection management (async)  // WiFi接続の非同期管理
  if (wifiConnecting && millis() - wifiConnectStartTime < wifiConnectTimeout) {
    if (WiFi.status() == WL_CONNECTED) {
      if (DEBUG_ENABLED) {
        Serial.println("\nWiFi connected"); // WiFi接続成功
        Serial.println(WiFi.localIP());
      }
      digitalWrite(redLED, LOW);
      wifiConnecting = false;
    }
  } else if (wifiConnecting) {
    if (DEBUG_ENABLED) Serial.println("\nWiFi connection failed"); // WiFi接続失敗
    digitalWrite(redLED, HIGH);
    wifiConnecting = false;
  }

  // WiFi connection check (after light sleep)  // WiFi接続チェック（ライトスリープ復帰後）
  if (!wifiConnecting && WiFi.status() != WL_CONNECTED) {
    connectToWiFi();
  }

  server.handleClient();
}

// Read and average multiple ADC samples  // 複数回サンプリングして平均値を計算
float readAverageVoltage(int pin) {
  long sum = 0;
  for (int i = 0; i < ADC_AVG_SAMPLES; i++) sum += analogRead(pin);
  return (sum / (float)ADC_AVG_SAMPLES) / ADC_RESOLUTION * ADC_REF_VOLTAGE * VOLTAGE_DIVIDER_RATIO;
}

// Ring buffer index calculation  // リングバッファのインデックス計算
int getRingBufferIndex(int baseIndex, int offset) {
  return (baseIndex + offset + totalSamples) % totalSamples;
}

// Get latest indices  // 最新インデックスの取得
void getLatestIndices(int& latestSampleIdx, int& latestAvgIdx) {
  latestSampleIdx = getRingBufferIndex(currentSample, -1);
  int latestAvgBaseIndex = (currentSample > 0) ? ((currentSample - 1) / samplesPerAvg) : -1;
  latestAvgIdx = (latestAvgBaseIndex >= 0) ? (latestAvgBaseIndex % (totalHours * 6)) : -1;
}

// Sample voltage and save to buffer  // 電圧データをサンプリングして保存
void sampleVoltages() {
  time_t now;
  time(&now);
  for (int ch = 0; ch < 4; ch++) {
    voltageData[currentSample].voltage[ch] = readAverageVoltage(adcPins[ch]);
  }
  voltageData[currentSample].timestamp = now;

  // Calculate average every 10 minutes  // 10分ごとに平均を計算
  if ((currentSample + 1) >= samplesPerAvg && (currentSample + 1) % samplesPerAvg == 0) {
    int avgIndex = (currentSample + 1) / samplesPerAvg - 1;
    float sum[4] = {0};
    for (int i = 0; i < samplesPerAvg; i++) {
      int idx = getRingBufferIndex(currentSample + 1 - samplesPerAvg + i, 0);
      for (int ch = 0; ch < 4; ch++) {
        sum[ch] += voltageData[idx].voltage[ch];
      }
    }
    for (int ch = 0; ch < 4; ch++) {
      avgVoltage[avgIndex % (totalHours * 6)].voltage[ch] = sum[ch] / samplesPerAvg;
    }
    avgVoltage[avgIndex % (totalHours * 6)].timestamp = now;
  }

  currentSample = (currentSample + 1) % totalSamples;
}
