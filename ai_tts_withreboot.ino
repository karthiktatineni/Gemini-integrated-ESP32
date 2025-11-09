#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

#include <AudioOutputI2S.h>
#include <AudioGeneratorMP3.h>
#include <AudioFileSourceHTTPStream.h>

// WiFi credentials
const char* ssid = "SwapnaHome";
const char* password = "NoPassword$2022";

// Gemini API key
const char* Gemini_Token = "AIzaSyBuu8wyrp9XGDR-qBE1wgxx5YPeOuYGilc";
const char* Gemini_Max_Tokens = "30";

// Web server
WebServer server(80);

// Audio objects
AudioGeneratorMP3 *mp3 = nullptr;
AudioFileSourceHTTPStream *file = nullptr;
AudioOutputI2S *out = nullptr;

// WiFi status LED and reboot button pins
const int WIFI_LED   = 2;  // onboard LED (GPIO2)
const int REBOOT_BTN = 4;  // physical button (GPIO4 → GND)

// --- URL encoder ---
String urlEncode(const String &str) {
  String encoded = "";
  for (size_t i = 0; i < str.length(); i++) {
    char c = str[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += c;
    } else if (c == ' ') {
      encoded += '+';
    } else {
      char buf[4];
      sprintf(buf, "%%%02X", (uint8_t)c);
      encoded += buf;
    }
  }
  return encoded;
}

// --- HTML page ---
const char htmlPage[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head><meta charset="UTF-8"><title>Gemini AI Q&A with TTS</title></head>
<body style="font-family:Arial;padding:20px;background:#f4f4f4">
  <div style="max-width:600px;margin:auto;background:#fff;padding:20px;border-radius:10px;box-shadow:0 0 10px #aaa;">
    <h2>Ask Gemini AI</h2>
    <textarea id="question" style="width:100%;height:80px"></textarea><br>
    <button onclick="askQuestion()">Ask</button>
    <div id="answer" style="margin-top:20px;padding:10px;background:#eee;border-radius:5px;white-space:pre-wrap;"></div>
    <p><b>Note:</b> Use the <u>physical button</u> on GPIO4 to reboot ESP anytime.</p>
  </div>
<script>
async function askQuestion() {
  const q=document.getElementById("question").value.trim();
  if(!q) return alert("Type something!");
  document.getElementById("answer").innerText="Thinking...";
  const r=await fetch("/ask",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({question:q})});
  const d=await r.json();
  document.getElementById("answer").innerText=d.answer||"No answer.";
}
</script>
</body></html>
)rawliteral";

// --- Serve HTML ---
void handleRoot() {
  server.send_P(200, "text/html", htmlPage);
}

// --- Start TTS ---
void startTTS(const String &text) {
  if (mp3) { mp3->stop(); delete mp3; mp3=nullptr; }
  if (file) { delete file; file=nullptr; }
  if (out)  { delete out;  out=nullptr; }

  String encoded = urlEncode(text);
  String ttsUrl = "http://translate.google.com/translate_tts?ie=UTF-8&client=tw-ob&q=" + encoded + "&tl=en";

  Serial.println("TTS URL: " + ttsUrl);

  file = new AudioFileSourceHTTPStream(ttsUrl.c_str());
  out  = new AudioOutputI2S(0, AudioOutputI2S::INTERNAL_DAC);
  out->SetOutputModeMono(true);
  out->SetGain(0.4);

  mp3 = new AudioGeneratorMP3();
  mp3->begin(file, out);

  Serial.println("TTS started...");
}

// --- Gemini handler ---
// --- Gemini handler ---
void handleAsk() {
  String body = server.arg("plain");
  DynamicJsonDocument reqDoc(1024);
  if (deserializeJson(reqDoc, body)) {
    server.send(400, "text/plain", "Bad JSON");
    return;
  }
  String question = reqDoc["question"].as<String>();

  HTTPClient https;
  String url = "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.0-flash:generateContent?key=" + String(Gemini_Token);
  https.begin(url);
  https.addHeader("Content-Type", "application/json");

  String payload = "{\"contents\":[{\"parts\":[{\"text\":\"" + question + "\"}]}],\"generationConfig\":{\"maxOutputTokens\":" + String(Gemini_Max_Tokens) + "}}";
  int httpCode = https.POST(payload);

  String answerText = "Error";
  if (httpCode == HTTP_CODE_OK) {
    String resp = https.getString();
    DynamicJsonDocument doc(8192);
    if (!deserializeJson(doc, resp)) {
      if (doc.containsKey("candidates")) {
        answerText = doc["candidates"][0]["content"]["parts"][0]["text"].as<String>();
        // --- Limit to 30 words ---
        int wordCount = 0;
        String limitedAnswer = "";
        for (int i = 0; i < answerText.length(); i++) {
          char c = answerText[i];
          if (isspace(c)) wordCount++;
          if (wordCount >= 30) break;
          limitedAnswer += c;
        }
        answerText = limitedAnswer;
      }
    }
  } else {
    answerText = "Gemini API error: " + https.errorToString(httpCode);
  }
  https.end();

  // respond immediately
  DynamicJsonDocument respDoc(2048);
  respDoc["answer"] = answerText;
  String respStr; serializeJson(respDoc, respStr);
  server.send(200, "application/json", respStr);

  if (answerText.length()) startTTS(answerText);
}


void setup() {
  Serial.begin(115200);
  pinMode(WIFI_LED, OUTPUT);
  pinMode(REBOOT_BTN, INPUT_PULLUP); // active LOW

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) { 
    delay(500); 
    Serial.print(".");
    digitalWrite(WIFI_LED, LOW);
  }
  Serial.println("\nWiFi connected. IP=" + WiFi.localIP().toString());
  digitalWrite(WIFI_LED, HIGH);

  server.on("/", handleRoot);
  server.on("/ask", HTTP_POST, handleAsk);
  server.begin();
  Serial.println("Server started");
}

void loop() {
  server.handleClient();

  if (mp3) {
    if (mp3->isRunning()) {
      if (!mp3->loop()) mp3->stop();
    } else {
      Serial.println("TTS finished.");
      delete mp3; mp3=nullptr;
      delete file; file=nullptr;
      delete out; out=nullptr;
    }
  }

  // WiFi LED
  digitalWrite(WIFI_LED, (WiFi.status() == WL_CONNECTED) ? HIGH : LOW);

  // --- Physical reboot button always active ---
  if (digitalRead(REBOOT_BTN) == LOW) {
    Serial.println("Physical button pressed → Rebooting ESP...");
    delay(300); // debounce
    ESP.restart();
  }
}
