#include <WiFi.h>
#include <PubSubClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "secrets.h" // Credenciales seguras 
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <ArduinoJson.h>
#include "LittleFS.h"

// --- Configuración del LED Indicador ---
#define LED_PIN 2 // Pin común para el LED integrado azul en ESP32 DevKit

// Configuración de Logs
const char* logPath = "/chiller.log";
const size_t maxLogSize = 51200; // 50 Kilobytes de límite

// --- Configuración OTA (GitHub) ---
String currentVersion = "v1.0.3"; 
String githubAPIURL = "https://api.github.com/repos/JesPezz/Monitor-Temperatura-Laser-C02/releases/latest";

unsigned long lastOTACheck = 0;
const unsigned long OTA_INTERVAL = 43200000; // Revisar cada 12 horas

// --- Configuración de los Sensores DS18B20 ---
#define ONE_WIRE_BUS 4 

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

// --- Configuración de Red ---
WiFiClient espClient;
PubSubClient mqttClient(espClient);

// --- Control de Tiempos (No Bloqueantes) ---
unsigned long lastTempReadTime = 0;
const unsigned long TEMP_INTERVAL = 5000; 

unsigned long lastMqttReconnectAttempt = 0;
unsigned long lastWifiReconnectAttempt = 0; 
const unsigned long RECONNECT_INTERVAL = 5000; 

unsigned long lastAlertTime = 0;
const unsigned long ALERT_INTERVAL = 60000; 
int currentAlertState = 0; 

void setupWiFi() {
    Serial.print("Conectando a WiFi: ");
    Serial.println(WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(); 
    delay(100);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
}

void handleReconnections(unsigned long currentMillis) {
    if (WiFi.status() != WL_CONNECTED) {
        if (currentMillis - lastWifiReconnectAttempt >= RECONNECT_INTERVAL) {
            lastWifiReconnectAttempt = currentMillis;
            Serial.println("WiFi perdido. Intentando reconectar...");
            WiFi.disconnect();
            WiFi.begin(WIFI_SSID, WIFI_PASS);
        }
        return; 
    }

    if (!mqttClient.connected()) {
        if (currentMillis - lastMqttReconnectAttempt >= RECONNECT_INTERVAL) {
            lastMqttReconnectAttempt = currentMillis;
            Serial.print("Intentando conexión MQTT...");
            String clientId = "ESP32-Chiller-" + String(random(0xffff), HEX);
            if (mqttClient.connect(clientId.c_str(), MQTT_USER, MQTT_PASS)) {
                Serial.println(" ¡Conectado!");
            } else {
                Serial.println(" Falló.");
            }
        }
    }
}

void checkForUpdates() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("❌ WiFi desconectado. Omitiendo revisión OTA...");
        return; 
    }

    Serial.println("🔍 Verificando nueva versión en GitHub Releases...");
    
    // Parpadeo rápido doble para indicar que está consultando GitHub
    digitalWrite(LED_PIN, HIGH); delay(100);
    digitalWrite(LED_PIN, LOW);  delay(100);
    digitalWrite(LED_PIN, HIGH); delay(100);
    digitalWrite(LED_PIN, LOW);

    WiFiClientSecure client;
    client.setInsecure(); 

    HTTPClient http;
    http.begin(client, githubAPIURL);
    http.addHeader("User-Agent", "ESP32-Chiller-OTA");
    http.addHeader("Authorization", String("Token ") + GITHUB_TOKEN);
    int httpCode = http.GET();

    if (httpCode == 200) {
        String jsonResponse = http.getString();
        
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, jsonResponse);
        
        if (error) {
            Serial.println("❌ Error al parsear JSON OTA.");
            return;
        }

        String newVersion = doc["tag_name"];
        String downloadURL = doc["assets"][0]["browser_download_url"];

        Serial.printf("📌 Versión actual: %s | Última en GitHub: %s\n", currentVersion.c_str(), newVersion.c_str());
        
        if (newVersion != currentVersion && downloadURL != "null") {
            Serial.println("🚀 Nueva versión detectada. Iniciando OTA...");
            if (mqttClient.connected()) mqttClient.publish("laser/chiller/alerta", "INFO: Descargando nueva versión OTA...");
            
            http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
            http.begin(client, downloadURL);
            int dlCode = http.GET();

            if (dlCode == 200 || dlCode == 302) {
                int contentLength = http.getSize();
                Serial.printf("📥 Tamaño del firmware: %d bytes\n", contentLength);

                if (Update.begin(contentLength, U_FLASH)) { 
                    WiFiClient *stream = http.getStreamPtr();
                    
                    // Mantener LED encendido fijo durante todo el proceso crítico de descarga y flasheo
                    digitalWrite(LED_PIN, HIGH); 

                    size_t written = Update.writeStream(*stream);

                    if (written == contentLength) {
                        Serial.println("✅ Firmware escrito. Validando...");
                        if (Update.end()) {
                            Serial.println("✅ OTA Exitosa. Reiniciando...");
                            if (mqttClient.connected()) mqttClient.publish("laser/chiller/alerta", "ÉXITO: Sistema actualizado. Reiniciando.");
                            
                            // Parpadeo rápido de éxito antes de reiniciar
                            for(int i=0; i<10; i++) {
                                digitalWrite(LED_PIN, !digitalRead(LED_PIN));
                                delay(100);
                            }
                            ESP.restart(); 
                        } else {
                            Serial.printf("❌ Error al finalizar: %s\n", Update.errorString());
                            digitalWrite(LED_PIN, HIGH); // Dejar LED encendido indicando error de actualización
                        }
                    } else {
                        Serial.println("❌ Error: Firmware incompleto.");
                        digitalWrite(LED_PIN, HIGH);
                    }
                } else {
                    Serial.println("❌ Error de espacio para Update.begin");
                    digitalWrite(LED_PIN, HIGH);
                }
            }
            http.end();
        } else {
            Serial.println("✅ El ESP32 ya tiene la versión más reciente.");
            digitalWrite(LED_PIN, LOW); // Apagar indicador al terminar con éxito sin cambios
        }
    } else {
        Serial.printf("❌ Error HTTP %d al consultar GitHub.\n", httpCode);
        if (httpCode == 403) {
            String errorPayload = http.getString(); 
            Serial.println("Respuesta de GitHub: " + errorPayload);
        }
    }
    http.end();
}

void writeLog(String message) {
    Serial.println(message);

    File logFile = LittleFS.open(logPath, FILE_APPEND);
    if (logFile) {
        logFile.println("[" + String(millis()/1000) + "s] " + message);
        logFile.close();
    }

    if (mqttClient.connected()) {
        mqttClient.publish("laser/chiller/logs", message.c_str());
    }

    File checkFile = LittleFS.open(logPath, FILE_READ);
    if (checkFile && checkFile.size() > maxLogSize) {
        checkFile.close();
        LittleFS.remove(logPath); 
        Serial.println("♻️ Log reiniciado para cuidar la memoria flash.");
    }
}

void enviarLogCompleto() {
    writeLog("📋 Enviando historial completo de log...");
    File file = LittleFS.open(logPath, FILE_READ);
    if (!file) {
        if(mqttClient.connected()) mqttClient.publish("laser/chiller/logs/history", "Error: No hay archivo de log.");
        return;
    }

    while (file.available()) {
        String linea = file.readStringUntil('\n');
        if(mqttClient.connected()) {
            mqttClient.publish("laser/chiller/logs/history", linea.c_str());
            mqttClient.loop(); 
        }
        vTaskDelay(20 / portTICK_PERIOD_MS); 
    }
    file.close();
    writeLog("✅ Historial enviado.");
}

void callback(char* topic, byte* payload, unsigned int length) {
    String message;
    for (int i = 0; i < length; i++) message += (char)payload[i];

    if (String(topic) == "laser/chiller/cmd") {
        if (message == "GET_LOGS") {
            enviarLogCompleto();
        }
        if (message == "CLEAR_LOGS") {
            LittleFS.remove(logPath);
            writeLog("🗑️ Log borrado por comando remoto");
        }
    }
}

void setup() {
    Serial.begin(115200);
    
    // Inicializar el pin del LED
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    Serial.println(currentVersion); 
    
    sensors.begin();
    setupWiFi();

    Serial.print("Esperando asignación de IP");
    unsigned long startAttemptTime = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 15000) {
        delay(500);
        Serial.print(".");
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\n¡WiFi Conectado!");
    } else {
        Serial.println("\nError de WiFi en booteo. Iniciando de todos modos...");
    }

    mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
    mqttClient.setCallback(callback); 
    
    if(!LittleFS.begin(true)){
        Serial.println("Error al montar LittleFS");
    }
    
    writeLog("🚀 Sistema Chiller iniciado - Versión " + currentVersion);

    // Verificación inicial de OTA al arrancar y marcar el tiempo de referencia
    checkForUpdates();
    lastOTACheck = millis(); 
}

void loop() {
    unsigned long currentMillis = millis();
    
    // Rutina de multitarea no bloqueante
    handleReconnections(currentMillis);
    
    if (mqttClient.connected()) mqttClient.loop();

    // --- REVISIÓN PERIÓDICA OTA (Cada 12 horas) ---
    if (currentMillis - lastOTACheck >= OTA_INTERVAL) {
        lastOTACheck = currentMillis;
        checkForUpdates();
    }

    // --- LECTURA DE TEMPERATURAS ---
    if (currentMillis - lastTempReadTime >= TEMP_INTERVAL) {
        lastTempReadTime = currentMillis;

        sensors.requestTemperatures(); 

        float tempIn = sensors.getTempCByIndex(0);
        float tempOut = sensors.getTempCByIndex(1);

        if (tempIn == DEVICE_DISCONNECTED_C || tempOut == DEVICE_DISCONNECTED_C) {
            Serial.println("Error: Uno o ambos sensores DS18B20 desconectados.");
            if (mqttClient.connected()) {
                mqttClient.publish("laser/chiller/alerta", "ERROR_SENSOR: DS18B20 desconectado.");
            }
            return;
        }

        float deltaT = abs(tempOut - tempIn); 

        Serial.printf("Temp Entrada: %.2f °C | Temp Salida: %.2f °C | Delta: %.2f °C\n", tempIn, tempOut, deltaT);

        if (mqttClient.connected()) {
            char strIn[8], strOut[8], strDelta[8];
            dtostrf(tempIn, 1, 2, strIn);
            dtostrf(tempOut, 1, 2, strOut);
            dtostrf(deltaT, 1, 2, strDelta);

            mqttClient.publish("laser/chiller/temp_in", strIn);
            mqttClient.publish("laser/chiller/temp_out", strOut);
            mqttClient.publish("laser/chiller/delta", strDelta);
        }

        float tempMax = max(tempIn, tempOut);

        if (tempMax >= 25.0) {
            if (currentAlertState != 2 || (currentMillis - lastAlertTime >= ALERT_INTERVAL)) {
                if (mqttClient.connected()) mqttClient.publish("laser/chiller/alerta", "CRÍTICO: Pausar corte.");
                lastAlertTime = currentMillis;
                currentAlertState = 2;
            }
        } 
        else if (tempMax >= 22.0) {
            if (currentAlertState != 1 || (currentMillis - lastAlertTime >= ALERT_INTERVAL)) {
                if (mqttClient.connected()) mqttClient.publish("laser/chiller/alerta", "ADVERTENCIA: Temperatura subiendo.");
                lastAlertTime = currentMillis;
                currentAlertState = 1;
            }
        } else {
            currentAlertState = 0; 
        }
    }
}