#include <WiFi.h>
#include <PubSubClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "secrets.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <ArduinoJson.h>

// --- Configuración OTA (GitHub) ---
String currentVersion = "v1.0.0"; // ⚠️ CAMBIA ESTO EN CADA RELEASE (ej. v1.0.1, v1.0.2)
// Reemplaza con tu usuario y el nombre del repositorio que creamos
String githubAPIURL = "https://api.github.com/repos/TU_USUARIO/Monitor-Temperatura-Laser-CO2/releases/latest";

unsigned long lastOTACheck = 0;
const unsigned long OTA_INTERVAL = 43200000; // Revisar cada 12 horas (en milisegundos)

// --- Configuración de los Sensores DS18B20 ---
#define ONE_WIRE_BUS 4 // ¡CAMBIO DE PIN! Usamos el GPIO 4

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

// --- Configuración de Red ---
WiFiClient espClient;
PubSubClient mqttClient(espClient);

// --- Control de Tiempos (No Bloqueantes) ---
unsigned long lastTempReadTime = 0;
const unsigned long TEMP_INTERVAL = 5000; // 5 segundos

unsigned long lastMqttReconnectAttempt = 0;
const unsigned long RECONNECT_INTERVAL = 5000; // 5 segundos

unsigned long lastAlertTime = 0;
const unsigned long ALERT_INTERVAL = 60000; // 1 minuto
int currentAlertState = 0; // 0 = Óptimo, 1 = Advertencia, 2 = Crítico

void setupWiFi() {
    Serial.print("Conectando a WiFi: ");
    Serial.println(WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
}

void handleReconnections(unsigned long currentMillis) {
    if (WiFi.status() != WL_CONNECTED) return;

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
    Serial.println("🔍 Verificando nueva versión en GitHub Releases...");

    WiFiClientSecure client;
    client.setInsecure(); // Evita problemas de certificados SSL

    HTTPClient http;
    http.begin(client, githubAPIURL);
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
            
            // Seguir redirecciones de GitHub (crítico para descargas)
            http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
            
            http.begin(client, downloadURL);
            int dlCode = http.GET();

            if (dlCode == 200 || dlCode == 302) {
                int contentLength = http.getSize();
                Serial.printf("📥 Tamaño del firmware: %d bytes\n", contentLength);

                if (Update.begin(contentLength, U_FLASH)) { // Iniciar proceso de flasheo
                    WiFiClient *stream = http.getStreamPtr();
                    size_t written = Update.writeStream(*stream);

                    if (written == contentLength) {
                        Serial.println("✅ Firmware escrito. Validando...");
                        if (Update.end()) {
                            Serial.println("✅ OTA Exitosa. Reiniciando...");
                            if (mqttClient.connected()) mqttClient.publish("laser/chiller/alerta", "ÉXITO: Sistema actualizado. Reiniciando.");
                            delay(2000);
                            ESP.restart(); // Reinicio automático
                        } else {
                            Serial.printf("❌ Error al finalizar: %s\n", Update.errorString());
                        }
                    } else {
                        Serial.println("❌ Error: Firmware incompleto.");
                    }
                } else {
                    Serial.println("❌ Error de espacio para Update.begin");
                }
            }
            http.end();
        } else {
            Serial.println("✅ El ESP32 ya tiene la versión más reciente.");
        }
    } else {
        Serial.printf("❌ Error HTTP %d al consultar GitHub.\n", httpCode);
    }
    http.end();
}

void setup() {
    Serial.begin(115200);
    
    // Iniciar los sensores DS18B20
    sensors.begin();
    
    setupWiFi();
    mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
}

void loop() {
    
    unsigned long currentMillis = millis();
    handleReconnections(currentMillis);

    // Revisar actualizaciones automáticamente basado en el temporizador
    if (currentMillis - lastOTACheck >= OTA_INTERVAL) {
        lastOTACheck = currentMillis;
        checkForUpdates();
    }
    
    if (mqttClient.connected()) mqttClient.loop();

    if (currentMillis - lastTempReadTime >= TEMP_INTERVAL) {
        lastTempReadTime = currentMillis;

        // Mandar la orden a todos los sensores de que midan la temperatura
        sensors.requestTemperatures(); 

        // Leer el Sensor 0 y el Sensor 1
        float tempIn = sensors.getTempCByIndex(0);
        float tempOut = sensors.getTempCByIndex(1);

        // Validar que los sensores estén conectados (-127.0 es el error por defecto)
        if (tempIn == DEVICE_DISCONNECTED_C || tempOut == DEVICE_DISCONNECTED_C) {
            Serial.println("Error: Uno o ambos sensores DS18B20 desconectados.");
            if (mqttClient.connected()) {
                mqttClient.publish("laser/chiller/alerta", "ERROR_SENSOR: DS18B20 desconectado.");
            }
            return;
        }

        float deltaT = abs(tempOut - tempIn); // Diferencia de temperatura

        Serial.printf("Temp Entrada: %.2f °C | Temp Salida: %.2f °C | Delta: %.2f °C\n", tempIn, tempOut, deltaT);

        // Publicar por MQTT en diferentes topics
        if (mqttClient.connected()) {
            char strIn[8], strOut[8], strDelta[8];
            dtostrf(tempIn, 1, 2, strIn);
            dtostrf(tempOut, 1, 2, strOut);
            dtostrf(deltaT, 1, 2, strDelta);

            mqttClient.publish("laser/chiller/temp_in", strIn);
            mqttClient.publish("laser/chiller/temp_out", strOut);
            mqttClient.publish("laser/chiller/delta", strDelta);
        }

        // Lógica de Alarmas basada en la temperatura más alta (Salida)
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