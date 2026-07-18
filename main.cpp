#include <WiFi.h>
#include <WebServer.h>
#include <time.h>
#include <esp_sleep.h>

// デバッグ設定
#define DEBUG_ENABLED true

// ADCピンとLEDピンの定義
const int adcPins[4] = {34, 35, 32, 33}; // V1〜V4
const int greenLED = 2;  // サンプリング動作
const int redLED = 4;    // WiFiエラー

// サンプリング設定
const int sampleIntervalSec = 10;
const int averageIntervalMin = 10;
const int totalHours = 12;

const int samplesPerAvg = averageIntervalMin * 60 / sampleIntervalSec;  // 60
const int totalSamples = totalHours * 60 * 60 / sampleIntervalSec;      // 2160
const int totalAvgSlots = totalHours * 6;                               // 72

const int graphDataPoints = 30;
const int refreshIntervalMs = 60000;

// データ構造
struct SampleData {
  uint16_t voltage[4];  // 0.01V単位
  time_t timestamp;
};

struct AvgData {
  uint16_t voltage[4];
  time_t timestamp;
};

SampleData voltageData[totalSamples];
AvgData avgVoltage[totalAvgSlots];

int currentSample = 0;   // 次に書き込む位置

// Webサーバー
WebServer server(80);
const char* ssid = "TestWiFi";
const char* password = "TestPass123";

// ADC設定
const float VOLTAGE_DIVIDER_RATIO = (100.0 + 15.0) / 15.0;
const float ADC_REF_VOLTAGE = 3.3;
const int ADC_RESOLUTION = 4095;
const int ADC_AVG_SAMPLES = 10;
const float VOLTAGE_SCALE = 100.0;

// LED制御
unsigned long greenLedOnTime = 0;
const unsigned long greenLedDuration = 100;
unsigned long wifiConnectStartTime = 0;
const unsigned long wifiConnectTimeout = 30000;
bool wifiConnecting = false;

// ====================== 関数宣言 ======================
int getRingIndex(int index);
int getLatestSampleIndex();
int getLatestAvgIndex();
float readAverageVoltage(int pin);
void calculate10MinAverage(int avgSlot);
String getFormattedTime();

// ====================== setup ======================
void setup() {
  Serial.begin(115200);

  // ピン初期化
  for (int i = 0; i < 4; i++) {
    pinMode(adcPins[i], INPUT);
    analogSetPinAttenuation(adcPins[i], ADC_11db);  // 修正
  }
  pinMode(greenLED, OUTPUT);
  pinMode(redLED, OUTPUT);

  // データ初期化
  memset(voltageData, 0, sizeof(voltageData));
  memset(avgVoltage, 0, sizeof(avgVoltage));

  connectToWiFi();
  setupWebServer();
  
  configTime(9 * 3600, 0, "ntp.nict.jp", "pool.ntp.org");

  WiFi.setSleep(true);
  if (DEBUG_ENABLED) Serial.println("システム起動完了");
}

// ====================== loop ======================
void loop() {
  static unsigned long lastSample = 0;

  // サンプリング
  if (millis() - lastSample >= sampleIntervalSec * 1000UL) {
    sampleVoltages();
    lastSample = millis();
    
    digitalWrite(greenLED, HIGH);
    greenLedOnTime = millis();
  }

  // 緑LED消灯
  if (greenLedOnTime && millis() - greenLedOnTime >= greenLedDuration) {
    digitalWrite(greenLED, LOW);
    greenLedOnTime = 0;
  }

  // WiFi再接続管理
  manageWiFiConnection();

  server.handleClient();
}

// ====================== ADC読み取り ======================
float readAverageVoltage(int pin) {
  long sum = 0;
  for (int i = 0; i < ADC_AVG_SAMPLES; i++) {
    sum += analogRead(pin);
  }
  float voltage = (sum / (float)ADC_AVG_SAMPLES) / ADC_RESOLUTION * ADC_REF_VOLTAGE * VOLTAGE_DIVIDER_RATIO;
  return voltage;
}

// ====================== リングバッファ ======================
int getRingIndex(int index) {
  return (index + totalSamples) % totalSamples;
}

int getLatestSampleIndex() {
  return getRingIndex(currentSample - 1);
}

// ====================== サンプリング本体 ======================
void sampleVoltages() {
  time_t now;
  time(&now);

  // データ保存
  for (int ch = 0; ch < 4; ch++) {
    float voltage = readAverageVoltage(adcPins[ch]);
    voltageData[currentSample].voltage[ch] = (uint16_t)(voltage * VOLTAGE_SCALE);
  }
  voltageData[currentSample].timestamp = now;

  // 10分平均の計算（1ブロック完了時）
  if ((currentSample % samplesPerAvg) == (samplesPerAvg - 1)) {
    int avgSlot = (currentSample / samplesPerAvg) % totalAvgSlots;
    calculate10MinAverage(avgSlot);
  }

  currentSample = (currentSample + 1) % totalSamples;
}

// 10分平均計算
void calculate10MinAverage(int avgSlot) {
  float sum[4] = {0.0};

  int startIdx = currentSample - samplesPerAvg + 1;  // ブロック開始位置

  for (int i = 0; i < samplesPerAvg; i++) {
    int idx = getRingIndex(startIdx + i);
    for (int ch = 0; ch < 4; ch++) {
      sum[ch] += voltageData[idx].voltage[ch] / VOLTAGE_SCALE;
    }
  }

  for (int ch = 0; ch < 4; ch++) {
    avgVoltage[avgSlot].voltage[ch] = (uint16_t)((sum[ch] / samplesPerAvg) * VOLTAGE_SCALE);
  }
  avgVoltage[avgSlot].timestamp = voltageData[getLatestSampleIndex()].timestamp;
}

// ====================== WiFi ======================
void connectToWiFi() {
  if (wifiConnecting) return;
  
  digitalWrite(redLED, HIGH);
  WiFi.begin(ssid, password);
  wifiConnectStartTime = millis();
  wifiConnecting = true;
  if (DEBUG_ENABLED) Serial.print("WiFi接続中...");
}

void manageWiFiConnection() {
  if (wifiConnecting) {
    if (WiFi.status() == WL_CONNECTED) {
      if (DEBUG_ENABLED) {
        Serial.println("\nWiFi接続成功");
        Serial.println(WiFi.localIP());
      }
      digitalWrite(redLED, LOW);
      wifiConnecting = false;
    } else if (millis() - wifiConnectStartTime > wifiConnectTimeout) {
      if (DEBUG_ENABLED) Serial.println("\nWiFi接続タイムアウト");
      wifiConnecting = false;
    }
  } 
  else if (WiFi.status() != WL_CONNECTED) {
    connectToWiFi();
  }
}

// ====================== Webサーバー ======================
void setupWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/data", HTTP_GET, handleData);
  server.on("/csv", HTTP_GET, handleCsv);
  server.begin();
  if (DEBUG_ENABLED) Serial.println("Webサーバー開始");
}

// HTMLテンプレート（PROGMEM）
const char htmlTemplate[] PROGMEM = R"rawliteral(
<html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'>
<style>body{font-family:sans-serif;margin:10px;}table{border-collapse:collapse;width:100%;max-width:600px;}
td,th{padding:5px;border:1px solid #ccc;}canvas{max-width:100%;height:auto;}
@media(max-width:600px){body{font-size:14px;}h2{font-size:18px;}}</style>
<script src='https://cdn.jsdelivr.net/npm/chart.js'></script></head><body>
<h2>電圧モニター</h2><p>時刻: %s</p>
<table><tr><th>チャンネル</th><th>最新値 (V)</th><th>10分平均 (V)</th></tr>%s</table>
<p><a href='/csv'>CSVダウンロード</a></p>
<canvas id='chart' height='300'></canvas>
<script>const ctx=document.getElementById('chart').getContext('2d');
let chart=new Chart(ctx,{type:'line',data:{labels:[...Array(%d).keys()].map(i=>i*10+'秒前'),datasets:[]},
options:{responsive:true,maintainAspectRatio:false,scales:{y:{beginAtZero:true}}}});
function fetchData(){fetch('/data').then(res=>res.json()).then(data=>{
chart.data.datasets=data.map((ch,idx)=>({label:'V'+(idx+1),data:ch,fill:false,borderColor:['red','blue','green','orange'][idx]}));chart.update();});}
fetchData();setInterval(fetchData,%d);</script></body></html>
)rawliteral";

// ====================== ハンドラ ======================
void handleRoot() {
  static char html[1200];
  static char table[600];
  table[0] = '\0';

  int latestIdx = getLatestSampleIndex();
  int latestAvgIdx = getLatestAvgIndex();

  int offset = 0;
  for (int ch = 0; ch < 4; ch++) {
    float latest = (currentSample > 0) ? voltageData[latestIdx].voltage[ch] / VOLTAGE_SCALE : 0.0;
    float avg10m = (latestAvgIdx >= 0) ? avgVoltage[latestAvgIdx].voltage[ch] / VOLTAGE_SCALE : 0.0;

    char row[120];
    snprintf(row, sizeof(row), "<tr><td>V%d</td><td>%.2f</td><td>%.2f</td></tr>", 
             ch + 1, latest, avg10m);
    offset += snprintf(table + offset, sizeof(table) - offset, "%s", row);
  }

  char timeStr[30];
  snprintf(timeStr, sizeof(timeStr), "%s", getFormattedTime().c_str());

  snprintf(html, sizeof(html), htmlTemplate, timeStr, table, graphDataPoints, refreshIntervalMs);
  server.send(200, "text/html", html);
}

int getLatestAvgIndex() {
  if (currentSample == 0) return -1;
  return ((currentSample - 1) / samplesPerAvg) % totalAvgSlots;
}

void handleData() {
  static char json[1100];
  int offset = snprintf(json, sizeof(json), "[");

  int startIdx = getRingIndex(currentSample - graphDataPoints);

  for (int ch = 0; ch < 4; ch++) {
    offset += snprintf(json + offset, sizeof(json) - offset, "[");
    for (int i = 0; i < graphDataPoints; i++) {
      int idx = getRingIndex(startIdx + i);
      float v = (currentSample > 0) ? voltageData[idx].voltage[ch] / VOLTAGE_SCALE : 0.0;
      offset += snprintf(json + offset, sizeof(json) - offset, "%.2f%s", 
                        v, (i < graphDataPoints - 1) ? "," : "");
    }
    offset += snprintf(json + offset, sizeof(json) - offset, "]%s", (ch < 3) ? "," : "");
  }
  snprintf(json + offset, sizeof(json) - offset, "]");

  server.send(200, "application/json", json);
}

void handleCsv() {
  server.sendHeader("Content-Type", "text/csv");
  server.send(200);
  WiFiClient client = server.client();

  char buf[1024];
  int offset = 0;

  offset += snprintf(buf, sizeof(buf), "Timestamp,V1,V2,V3,V4\n");
  client.write((const uint8_t*)buf, offset);

  // サンプルデータ
  for (int i = 0; i < totalSamples; i++) {
    offset = snprintf(buf, sizeof(buf), "%lld,%.2f,%.2f,%.2f,%.2f\n",
                      (long long)voltageData[i].timestamp,
                      voltageData[i].voltage[0] / VOLTAGE_SCALE,
                      voltageData[i].voltage[1] / VOLTAGE_SCALE,
                      voltageData[i].voltage[2] / VOLTAGE_SCALE,
                      voltageData[i].voltage[3] / VOLTAGE_SCALE);
    client.write((const uint8_t*)buf, offset);
  }

  // 平均データ
  offset = snprintf(buf, sizeof(buf), "\n10min Averages\nTimestamp,V1,V2,V3,V4\n");
  client.write((const uint8_t*)buf, offset);

  for (int i = 0; i < totalAvgSlots; i++) {
    if (avgVoltage[i].timestamp == 0) continue;
    offset = snprintf(buf, sizeof(buf), "%lld,%.2f,%.2f,%.2f,%.2f\n",
                      (long long)avgVoltage[i].timestamp,
                      avgVoltage[i].voltage[0] / VOLTAGE_SCALE,
                      avgVoltage[i].voltage[1] / VOLTAGE_SCALE,
                      avgVoltage[i].voltage[2] / VOLTAGE_SCALE,
                      avgVoltage[i].voltage[3] / VOLTAGE_SCALE);
    client.write((const uint8_t*)buf, offset);
  }

  client.stop();
}

String getFormattedTime() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return "時刻同期失敗";
  char buf[30];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(buf);
}
