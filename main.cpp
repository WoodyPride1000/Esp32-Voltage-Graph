#include <WiFi.h>
#include <WebServer.h>
#include <time.h>

#define DEBUG_ENABLED true

// ================== 設定 ==================
const int adcPins[4] = {34, 35, 32, 33};
const int greenLED = 2;
const int redLED = 4;

const int sampleIntervalSec = 10;
const int averageIntervalMin = 10;
const int totalHours = 12;

const int samplesPerAvg = (averageIntervalMin * 60) / sampleIntervalSec;
const int totalSamples = totalHours * 3600 / sampleIntervalSec;   // 2160
const int totalAvgPoints = totalHours * 6;                        // 72
const int graphDataPoints = 30;
const int refreshIntervalMs = 10000;

// ================== データ構造 ==================
struct DataPoint {
  uint16_t voltage[4];   // 0.01V単位
  time_t timestamp;
};

DataPoint ringBuffer[totalSamples];
DataPoint avgBuffer[totalAvgPoints];

int ringIdx = 0;
int avgIdx = 0;
int runningCount = 0;
int totalCollectedSamples = 0;
uint32_t runningSum[4] = {0};   // 整数累積

// WiFi・サーバー
WebServer server(80);
const char* ssid = "TestWiFi";
const char* password = "TestPass123";

const float VOLTAGE_SCALE = 100.0;

unsigned long greenLedOnTime = 0;
const unsigned long greenLedDuration = 100;
bool wifiConnecting = false;
unsigned long wifiConnectStartTime = 0;
const unsigned long wifiConnectTimeout = 30000;

// ====================== 関数 ======================
int getRingIndex(int i) {
  return (i % totalSamples + totalSamples) % totalSamples;
}

float readAverageVoltage(int pin) {
  long sum = 0;
  for (int i = 0; i < 10; i++) sum += analogRead(pin);
  return (sum / 10.0f) * (3.3f * (115.0f / 15.0f) / 4095.0f);
}

// ====================== setup / loop ======================
void setup() {
  Serial.begin(115200);

  for (int i = 0; i < 4; i++) {
    pinMode(adcPins[i], INPUT);
    analogSetPinAttenuation(adcPins[i], ADC_11db);
  }
  pinMode(greenLED, OUTPUT);
  pinMode(redLED, OUTPUT);

  connectToWiFi();
  setupWebServer();
  configTime(9 * 3600, 0, "ntp.nict.jp", "pool.ntp.org");
  WiFi.setSleep(true);

  if (DEBUG_ENABLED) Serial.println("=== 電圧モニター起動（整数演算版） ===");
}

void loop() {
  static unsigned long lastSample = 0;

  if (millis() - lastSample >= sampleIntervalSec * 1000UL) {
    lastSample = millis();
    sampleVoltages();
  }

  if (greenLedOnTime && millis() - greenLedOnTime >= greenLedDuration) {
    digitalWrite(greenLED, LOW);
    greenLedOnTime = 0;
  }

  manageWiFi();
  server.handleClient();
}

// ====================== サンプリング ======================
void sampleVoltages() {
  time_t now;
  time(&now);

  DataPoint current{};
  current.timestamp = now;

  for (int ch = 0; ch < 4; ch++) {
    float v = readAverageVoltage(adcPins[ch]);
    current.voltage[ch] = (uint16_t)(v * VOLTAGE_SCALE + 0.5f);  // 四捨五入
    runningSum[ch] += current.voltage[ch];   // 整数累積
  }

  ringBuffer[ringIdx] = current;
  ringIdx = (ringIdx + 1) % totalSamples;

  if (totalCollectedSamples < totalSamples) totalCollectedSamples++;

  // 10分平均（整数演算）
  runningCount++;
  if (runningCount >= samplesPerAvg) {
    DataPoint avg{};
    avg.timestamp = now;
    for (int ch = 0; ch < 4; ch++) {
      avg.voltage[ch] = (uint16_t)(runningSum[ch] / samplesPerAvg);
      runningSum[ch] = 0;
    }
    avgBuffer[avgIdx] = avg;
    avgIdx = (avgIdx + 1) % totalAvgPoints;
    runningCount = 0;
  }

  digitalWrite(greenLED, HIGH);
  greenLedOnTime = millis();
}

// ====================== WiFi ======================
void connectToWiFi() {
  if (wifiConnecting) return;
  digitalWrite(redLED, HIGH);
  WiFi.begin(ssid, password);
  wifiConnectStartTime = millis();
  wifiConnecting = true;
}

void manageWiFi() {
  if (wifiConnecting) {
    if (WiFi.status() == WL_CONNECTED) {
      digitalWrite(redLED, LOW);
      wifiConnecting = false;
      if (DEBUG_ENABLED) Serial.println("WiFi接続成功");
    } else if (millis() - wifiConnectStartTime > wifiConnectTimeout) {
      wifiConnecting = false;
      if (DEBUG_ENABLED) Serial.println("WiFi接続失敗");
    }
  } else if (WiFi.status() != WL_CONNECTED) {
    connectToWiFi();
  }
}

// ====================== Webサーバー ======================
void setupWebServer() {
  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/csv", handleCsv);
  server.begin();
  if (DEBUG_ENABLED) Serial.println("Webサーバー開始");
}

// ================== HTMLテンプレート ==================
const char htmlTemplate[] PROGMEM = R"rawliteral(
<html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'>
<style>body{font-family:sans-serif;margin:10px;}table{border-collapse:collapse;width:100%%;max-width:600px;}
td,th{padding:5px;border:1px solid #ccc;}canvas{max-width:100%%;height:auto;}</style>
<script src='https://cdn.jsdelivr.net/npm/chart.js'></script></head><body>
<h2>電圧モニター</h2><p>現在時刻: %s</p>
<table><tr><th>CH</th><th>最新値 (V)</th><th>10分平均 (V)</th></tr>%s</table>
<p><a href='/csv'>CSVダウンロード (12時間分)</a></p>
<div style='position:relative;max-width:600px;height:300px;'><canvas id='chart'></canvas></div>
<script>
const ctx=document.getElementById('chart').getContext('2d');
let chart=new Chart(ctx,{type:'line',data:{labels:[...Array(%d).keys()].reverse().map(i=>i*10+'秒前'),datasets:[]},
options:{responsive:true,maintainAspectRatio:false,scales:{y:{beginAtZero:true}}}});
function fetchData(){fetch('/data').then(res=>res.json()).then(data=>{
chart.data.datasets=data.map((ch,idx)=>({label:'V'+(idx+1),data:ch,fill:false,borderColor:['red','blue','green','orange'][idx]}));
chart.update();});}fetchData();setInterval(fetchData,%d);
</script></body></html>
)rawliteral";

// ====================== ハンドラ ======================
void formatTime(char* buf, size_t maxLen, time_t t) {
  if (t < 100000) {
    snprintf(buf, maxLen, "時刻同期未完了");
    return;
  }
  struct tm* tm_info = localtime(&t);
  strftime(buf, maxLen, "%Y-%m-%d %H:%M:%S", tm_info);
}

void handleRoot() {
  char timeStr[30];
  time_t now;
  time(&now);
  formatTime(timeStr, sizeof(timeStr), now);

  int latestRing = getRingIndex(ringIdx - 1);
  int latestAvg = (avgIdx - 1 + totalAvgPoints) % totalAvgPoints;

  bool hasRing = (ringBuffer[latestRing].timestamp != 0);
  bool hasAvg = (avgBuffer[latestAvg].timestamp != 0);

  char tableRows[512] = "";
  int offset = 0;
  for (int ch = 0; ch < 4; ch++) {
    char vLatest[16] = "データなし";
    char vAvg[16] = "データなし";
    if (hasRing) snprintf(vLatest, sizeof(vLatest), "%.2f", ringBuffer[latestRing].voltage[ch] / VOLTAGE_SCALE);
    if (hasAvg)  snprintf(vAvg,    sizeof(vAvg),    "%.2f", avgBuffer[latestAvg].voltage[ch] / VOLTAGE_SCALE);

    offset += snprintf(tableRows + offset, sizeof(tableRows) - offset,
                       "<tr><td>V%d</td><td>%s</td><td>%s</td></tr>", ch + 1, vLatest, vAvg);
  }

  char* responseBuf = (char*)malloc(1500);
  if (!responseBuf) {
    server.send(500, "text/plain", "Memory Error");
    return;
  }

  snprintf(responseBuf, 1500, htmlTemplate, timeStr, tableRows, graphDataPoints, refreshIntervalMs);
  server.send(200, "text/html", responseBuf);
  free(responseBuf);
}

void handleData() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  server.sendContent("[");

  int count = min(graphDataPoints, totalCollectedSamples);
  int startIdx = getRingIndex(ringIdx - count);

  char buf[32];
  for (int ch = 0; ch < 4; ch++) {
    server.sendContent("[");
    for (int i = 0; i < count; i++) {
      int idx = getRingIndex(startIdx + i);
      snprintf(buf, sizeof(buf), "%.2f%s",
               ringBuffer[idx].voltage[ch] / VOLTAGE_SCALE,
               (i < count - 1) ? "," : "");
      server.sendContent(buf);
    }
    server.sendContent(ch < 3 ? "]," : "]");
  }

  server.sendContent("]");
  server.sendContent("");
}

void handleCsv() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/csv", "");
  server.sendContent("Type,Timestamp,DateTime,V1,V2,V3,V4\n");

  char buf[128];
  char timeStr[20];

  // Rawデータ（最新→過去）
  for (int i = 0; i < totalSamples; i++) {
    int idx = getRingIndex(ringIdx - 1 - i);
    if (ringBuffer[idx].timestamp == 0) continue;

    struct tm* tm_info = localtime(&ringBuffer[idx].timestamp);
    strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", tm_info);

    snprintf(buf, sizeof(buf), "Raw,%lld,%s,%.2f,%.2f,%.2f,%.2f\n",
             (long long)ringBuffer[idx].timestamp, timeStr,
             ringBuffer[idx].voltage[0] / VOLTAGE_SCALE,
             ringBuffer[idx].voltage[1] / VOLTAGE_SCALE,
             ringBuffer[idx].voltage[2] / VOLTAGE_SCALE,
             ringBuffer[idx].voltage[3] / VOLTAGE_SCALE);
    server.sendContent(buf);
  }

  server.sendContent("\nAverage10min\nType,Timestamp,DateTime,V1,V2,V3,V4\n");

  // 平均データ（最新→過去）
  for (int i = 0; i < totalAvgPoints; i++) {
    int idx = (avgIdx - 1 - i + totalAvgPoints) % totalAvgPoints;
    if (avgBuffer[idx].timestamp == 0) continue;

    struct tm* tm_info = localtime(&avgBuffer[idx].timestamp);
    strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", tm_info);

    snprintf(buf, sizeof(buf), "Avg,%lld,%s,%.2f,%.2f,%.2f,%.2f\n",
             (long long)avgBuffer[idx].timestamp, timeStr,
             avgBuffer[idx].voltage[0] / VOLTAGE_SCALE,
             avgBuffer[idx].voltage[1] / VOLTAGE_SCALE,
             avgBuffer[idx].voltage[2] / VOLTAGE_SCALE,
             avgBuffer[idx].voltage[3] / VOLTAGE_SCALE);
    server.sendContent(buf);
  }
  
  server.sendContent("");
}
