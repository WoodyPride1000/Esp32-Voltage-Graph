#include <WiFi.h>
#include <WebServer.h>
#include <time.h>
#include <esp_sleep.h>

// デバッグ設定
#define DEBUG_ENABLED true

// ADCピンとLEDピンの定義
const int adcPins[4] = {34, 35, 32, 33}; // V1〜V4
const int greenLED = 2;                   // 正常動作（サンプリング）用
const int redLED = 4;                     // エラー（WiFi接続失敗など）用

// サンプリングと平均化の設定
const int sampleIntervalSec = 10;        // 10秒ごとにサンプリング
const int averageIntervalMin = 10;       // 10分ごとの平均
const int totalHours = 12;               // 12時間分のデータ（メモリ節約）
const int samplesPerAvg = averageIntervalMin * 60 / sampleIntervalSec; // 60サンプル/10分
const int totalSamples = totalHours * 60 * 60 / sampleIntervalSec;    // 4320サンプル
const int graphDataPoints = 30;           // グラフに表示するデータ数
const int refreshIntervalMs = 60000;      // データ更新間隔（60秒）

// データ構造
struct SampleData {
  float voltage[4];
  time_t timestamp;
};
struct AvgData {
  float voltage[4];
  time_t timestamp;
};
SampleData voltageData[totalSamples];
AvgData avgVoltage[totalHours * 6]; // 6 averages per hour
int currentSample = 0;

// WebサーバーとWiFi設定
WebServer server(80);
const char* ssid = "YOUR_SSID";     // 要変更（EEPROMやCaptive Portalを検討）
const char* password = "YOUR_PASSWORD"; // 要変更
const char* pathRoot = "/";
const char* pathData = "/data";
const char* pathCsv = "/csv";

// ADC設定
const float VOLTAGE_DIVIDER_RATIO = (100.0 + 15.0) / 15.0;
const float ADC_REF_VOLTAGE = 3.3;
const int ADC_RESOLUTION = 4095;
const int ADC_AVG_SAMPLES = 10; // ノイズ低減のための平均サンプル数

// LEDとWiFi接続管理
unsigned long greenLedOnTime = 0;
const unsigned long greenLedDuration = 100; // 緑LED点滅時間（ms）
unsigned long wifiConnectStartTime = 0;
const unsigned long wifiConnectTimeout = 30000; // WiFi接続タイムアウト（ms）
bool wifiConnecting = false;

void setup() {
  Serial.begin(115200);
  // ピンの初期化
  for (int i = 0; i < 4; i++) {
    pinMode(adcPins[i], INPUT);
    analogSetAttenuation(ADC_11db); // 電圧範囲0-約3.9Vに対応
  }
  pinMode(greenLED, OUTPUT);
  pinMode(redLED, OUTPUT);
  digitalWrite(greenLED, LOW);
  digitalWrite(redLED, LOW);

  // データ配列の初期化
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
  configTime(9 * 3600, 0, "ntp.nict.jp", "pool.ntp.org"); // 日本標準時
}

void loop() {
  static unsigned long lastSample = 0;
  // サンプリング
  if (millis() - lastSample >= sampleIntervalSec * 1000) {
    sampleVoltages();
    lastSample = millis();
    digitalWrite(greenLED, HIGH); // サンプリング成功時に緑LED点灯
    greenLedOnTime = millis();
    // ライトスリープ（WiFiとメモリを保持）
    esp_sleep_enable_timer_wakeup(sampleIntervalSec * 1000000ULL);
    esp_light_sleep_start();
  }

  // 緑LEDの点滅制御
  if (greenLedOnTime && millis() - greenLedOnTime >= greenLedDuration) {
    digitalWrite(greenLED, LOW);
    greenLedOnTime = 0;
  }

  // WiFi接続の非同期管理
  if (wifiConnecting && millis() - wifiConnectStartTime < wifiConnectTimeout) {
    if (WiFi.status() == WL_CONNECTED) {
      if (DEBUG_ENABLED) {
        Serial.println("\nWiFi接続成功");
        Serial.println(WiFi.localIP());
      }
      digitalWrite(redLED, LOW);
      wifiConnecting = false;
    }
  } else if (wifiConnecting) {
    if (DEBUG_ENABLED) Serial.println("\nWiFi接続失敗");
    digitalWrite(redLED, HIGH);
    wifiConnecting = false;
  }

  // WiFi接続チェック（ライトスリープ復帰後）
  if (!wifiConnecting && WiFi.status() != WL_CONNECTED) {
    connectToWiFi();
  }

  server.handleClient();
}

// 複数回サンプリングして平均値を計算
float readAverageVoltage(int pin) {
  long sum = 0;
  for (int i = 0; i < ADC_AVG_SAMPLES; i++) sum += analogRead(pin);
  return (sum / (float)ADC_AVG_SAMPLES) / ADC_RESOLUTION * ADC_REF_VOLTAGE * VOLTAGE_DIVIDER_RATIO;
}

// リングバッファのインデックス計算
int getRingBufferIndex(int baseIndex, int offset) {
  return (baseIndex + offset + totalSamples) % totalSamples;
}

// 電圧データをサンプリングして保存
void sampleVoltages() {
  time_t now;
  time(&now);
  for (int ch = 0; ch < 4; ch++) {
    voltageData[currentSample].voltage[ch] = readAverageVoltage(adcPins[ch]);
  }
  voltageData[currentSample].timestamp = now;

  // 10分ごとに平均を計算
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

// WiFi接続（非同期）
void connectToWiFi() {
  if (!wifiConnecting) {
    digitalWrite(redLED, HIGH); // 接続試行中に赤LED点灯
    WiFi.begin(ssid, password);
    wifiConnectStartTime = millis();
    wifiConnecting = true;
    if (DEBUG_ENABLED) Serial.print("WiFi接続中...");
  }
}

// Webサーバーの設定
void setupWebServer() {
  server.on(pathRoot, HTTP_GET, handleRoot);
  server.on(pathData, HTTP_GET, handleData);
  server.on(pathCsv, HTTP_GET, handleCsv);
  server.begin();
  if (DEBUG_ENABLED) Serial.println("Webサーバー開始");
}

// HTMLテンプレート（PROGMEMでメモリ節約）
const char htmlTemplate[] PROGMEM = R"rawliteral(
<html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'>
<style>body{font-family:sans-serif;margin:10px;}table{border-collapse:collapse;width:100%%;max-width:600px;}
td,th{padding:5px;border:1px solid #ccc;}canvas{max-width:100%%;height:auto;}
@media(max-width:600px){body{font-size:14px;}h2{font-size:18px;}}</style>
<script src='https://cdn.jsdelivr.net/npm/chart.js'></script></head><body>
<h2>電圧モニター</h2><p>時刻: %s</p>
<table><tr><th>チャンネル</th><th>最新値 (V)</th><th>10分平均 (V)</th></tr>%s</table>
<p><a href='%s'>CSVダウンロード</a></p>
<canvas id='chart' height='300'></canvas>
<script>const ctx=document.getElementById('chart').getContext('2d');
let chart=new Chart(ctx,{type:'line',data:{labels:[...Array(%d).keys()].map(i=>i*10+'秒前'),
datasets:[]},options:{responsive:true,maintainAspectRatio:false,scales:{y:{beginAtZero:true}}}});
function fetchData(){fetch('%s').then(res=>res.json()).then(data=>{
chart.data.datasets=data.map((ch,idx)=>({label:'V'+(idx+1),data:ch,
fill:false,borderColor:['red','blue','green','orange'][idx]}));chart.update();});}
fetchData();setInterval(fetchData,%d);</script></body></html>
)rawliteral";

// ルートエンドポイント
void handleRoot() {
  char html[2048];
  char table[512] = "";
  for (int ch = 0; ch < 4; ch++) {
    float latest = voltageData[getRingBufferIndex(currentSample, -1)].voltage[ch];
    float avg10min = avgVoltage[((currentSample + 1) / samplesPerAvg - 1 + totalHours * 6) % (totalHours * 6)].voltage[ch];
    char row[128];
    snprintf(row, sizeof(row), "<tr><td>V%d</td><td>%.2f</td><td>%.2f</td></tr>", ch + 1, latest, avg10min);
    strncat(table, row, sizeof(table) - strlen(table) - 1);
  }
  snprintf(html, sizeof(html), htmlTemplate, getFormattedTime().c_str(), table, pathCsv, graphDataPoints, pathData, refreshIntervalMs);
  server.send(200, "text/html", html);
}

// データエンドポイント（JSON）
void handleData() {
  char json[1024];
  strcpy(json, "[");
  for (int ch = 0; ch < 4; ch++) {
    strcat(json, "[");
    for (int i = 0; i < graphDataPoints; i++) {
      int index = getRingBufferIndex(currentSample, -graphDataPoints + i);
      char val[16];
      snprintf(val, sizeof(val), "%.2f", voltageData[index].voltage[ch]);
      strcat(json, val);
      if (i < graphDataPoints - 1) strcat(json, ",");
    }
    strcat(json, "]");
    if (ch < 3) strcat(json, ",");
  }
  strcat(json, "]");
  server.send(200, "application/json", json);
}

// CSVエンドポイント（チャンク転送）
void handleCsv() {
  server.sendHeader("Content-Type", "text/csv");
  server.send(200);
  WiFiClient client = server.client();
  client.print("Timestamp,V1,V2,V3,V4\n");
  for (int i = 0; i < totalSamples; i++) {
    char row[128];
    snprintf(row, sizeof(row), "%lld,%.2f,%.2f,%.2f,%.2f\n",
             (long long)voltageData[i].timestamp,
             voltageData[i].voltage[0], voltageData[i].voltage[1],
             voltageData[i].voltage[2], voltageData[i].voltage[3]);
    client.print(row);
  }
  client.print("\n10min Averages\nTimestamp,V1,V2,V3,V4\n");
  for (int i = 0; i < totalHours * 6; i++) {
    char row[128];
    snprintf(row, sizeof(row), "%lld,%.2f,%.2f,%.2f,%.2f\n",
             (long long)avgVoltage[i].timestamp,
             avgVoltage[i].voltage[0], avgVoltage[i].voltage[1],
             avgVoltage[i].voltage[2], avgVoltage[i].voltage[3]);
    client.print(row);
  }
  client.stop();
}

// 時刻取得（エラーハンドリング付き）
String getFormattedTime() {
  time_t now;
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return "時刻同期失敗";
  }
  char buf[30];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(buf);
}
