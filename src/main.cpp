#include <WiFi.h>
#include <PubSubClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "secrets.h"

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