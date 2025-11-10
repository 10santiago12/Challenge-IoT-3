import paho.mqtt.client as mqtt
import sqlite3
import json
import time
import threading
import queue
import logging
from datetime import datetime
import requests
import urllib3

# Suprimir warnings de SSL (solo para entorno de pruebas)
urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)

# Logging
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(levelname)s - %(message)s',
    handlers=[
        logging.FileHandler("gateway_talud.log"),
        logging.StreamHandler()
    ]
)

# Broker MQTT Local (Mosquitto en Raspberry Pi)
LOCAL_MQTT_HOST = "localhost"
LOCAL_MQTT_PORT = 1883
LOCAL_MQTT_TOPIC = "talud/sensors"  # Topic donde ESP32 publica datos

# Broker MQTT Ubidots (Cloud)
UBIDOTS_MQTT_HOST = "industrial.api.ubidots.com"
UBIDOTS_MQTT_PORT = 1883
UBIDOTS_TOKEN = "BBUS-pFREBdzamEXnQvOOpsILJKFsQqeKlE"  # REEMPLAZAR con tu token de Ubidots
UBIDOTS_DEVICE_LABEL = "talud-sistema"

# Topics de Ubidots para cada variable
UBIDOTS_TOPICS = {
    "humidity": f"/v1.6/devices/{UBIDOTS_DEVICE_LABEL}/humidity",
    "rain_raw": f"/v1.6/devices/{UBIDOTS_DEVICE_LABEL}/rain_raw",
    "rain_confirmed": f"/v1.6/devices/{UBIDOTS_DEVICE_LABEL}/rain_confirmed",
    "vibration_rms": f"/v1.6/devices/{UBIDOTS_DEVICE_LABEL}/vibration_rms",
    "vibration_peak": f"/v1.6/devices/{UBIDOTS_DEVICE_LABEL}/vibration_peak",
    "pitch": f"/v1.6/devices/{UBIDOTS_DEVICE_LABEL}/pitch",
    "roll": f"/v1.6/devices/{UBIDOTS_DEVICE_LABEL}/roll",
    "risk_score": f"/v1.6/devices/{UBIDOTS_DEVICE_LABEL}/risk_score",
    "alert_level": f"/v1.6/devices/{UBIDOTS_DEVICE_LABEL}/alert_level",
    "ai_recommendation": f"/v1.6/devices/{UBIDOTS_DEVICE_LABEL}/ai_recommendation"
}

# Base de datos SQLite
DB_PATH = "talud_data.db"

# Configuración de IA (Gemini API)
GEMINI_API_KEY = "AIzaSyDsYpaR6A8y0vT8TqUSlHhbFizRZ5ho_aA"  # API key actualizada
GEMINI_ENABLED = True
AI_ANALYSIS_INTERVAL = 300  # Análisis cada 5 minutos (cuando se reactive)

message_queue = queue.Queue()

local_mqtt_client = mqtt.Client(client_id="gateway_talud_local")
ubidots_mqtt_client = mqtt.Client(client_id="gateway_talud_ubidots")

last_ai_analysis = 0


def connect_db():
    """Conecta a la base de datos SQLite"""
    try:
        conn = sqlite3.connect(DB_PATH, check_same_thread=False)
        return conn
    except sqlite3.Error as e:
        logging.error(f" Error al conectar a SQLite: {e}")
        return None


def init_database():
    """Crea la tabla si no existe"""
    conn = connect_db()
    if conn:
        try:
            cursor = conn.cursor()
            cursor.execute("""
                CREATE TABLE IF NOT EXISTS sensor_data (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    timestamp DATETIME DEFAULT CURRENT_TIMESTAMP,
                    humidity REAL NOT NULL,
                    rain_raw INTEGER NOT NULL,
                    rain_confirmed INTEGER NOT NULL,
                    vibration_rms REAL NOT NULL,
                    vibration_peak REAL NOT NULL,
                    pitch REAL NOT NULL,
                    roll REAL NOT NULL,
                    risk_score REAL NOT NULL,
                    alert_level INTEGER NOT NULL,
                    sent_to_cloud INTEGER DEFAULT 1
                )
            """)
            conn.commit()
            logging.info(" Base de datos inicializada correctamente")
        except sqlite3.Error as e:
            logging.error(f" Error al crear la tabla: {e}")
        finally:
            conn.close()


def save_to_db(humidity, rain_raw, rain_confirmed, vib_rms, vib_peak, 
               pitch, roll, risk_score, alert_level):
    """Guarda los datos de sensores en la base de datos"""
    conn = connect_db()
    if conn:
        try:
            cursor = conn.cursor()
            cursor.execute("""
                INSERT INTO sensor_data 
                (humidity, rain_raw, rain_confirmed, vibration_rms, vibration_peak, 
                 pitch, roll, risk_score, alert_level)
                VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
            """, (humidity, rain_raw, rain_confirmed, vib_rms, vib_peak, 
                  pitch, roll, risk_score, alert_level))
            conn.commit()
            logging.info(f" Datos guardados en BD: Score={risk_score:.2f}, Nivel={alert_level}")
        except sqlite3.Error as e:
            logging.error(f" Error al guardar en SQLite: {e}")
        finally:
            conn.close()


def get_recent_data(limit=60):
    """Obtiene los últimos N registros para análisis de IA"""
    conn = connect_db()
    if conn:
        try:
            cursor = conn.cursor()
            cursor.execute("""
                SELECT humidity, rain_confirmed, vibration_rms, vibration_peak,
                       pitch, roll, risk_score, alert_level, timestamp
                FROM sensor_data
                ORDER BY timestamp DESC
                LIMIT ?
            """, (limit,))
            rows = cursor.fetchall()
            return rows
        except sqlite3.Error as e:
            logging.error(f" Error al leer datos históricos: {e}")
            return []
        finally:
            conn.close()
    return []

def analyze_with_ai():
    """Analiza datos históricos con Gemini y genera recomendaciones"""
    if not GEMINI_ENABLED or not GEMINI_API_KEY:
        logging.warning(" Módulo de IA deshabilitado o sin API key")
        return None
    
    try:
        # Obtener datos recientes
        recent_data = get_recent_data(60)
        
        if not recent_data:
            logging.warning(" No hay datos para análisis de IA")
            return None
        
        # Preparar resumen de datos para el prompt
        avg_humidity = sum([row[0] for row in recent_data]) / len(recent_data)
        rain_count = sum([row[1] for row in recent_data])
        avg_vib_rms = sum([row[2] for row in recent_data]) / len(recent_data)
        max_vib_peak = max([row[3] for row in recent_data])
        avg_pitch = sum([row[4] for row in recent_data]) / len(recent_data)
        avg_roll = sum([row[5] for row in recent_data]) / len(recent_data)
        avg_score = sum([row[6] for row in recent_data]) / len(recent_data)
        max_level = max([row[7] for row in recent_data])
        
        # Construir prompt para Gemini
        prompt = f"""
Eres un experto en geotecnia y análisis de riesgos de deslizamientos de tierra.
Analiza los siguientes datos de un sistema de monitoreo de taludes de las últimas horas:

- Humedad promedio del suelo: {avg_humidity*100:.1f}%
- Eventos de lluvia detectados: {rain_count} en última hora
- Vibración RMS promedio: {avg_vib_rms:.3f} g
- Pico máximo de vibración: {max_vib_peak:.3f} g
- Inclinación promedio (Pitch): {avg_pitch:.1f}°
- Inclinación promedio (Roll): {avg_roll:.1f}°
- Score de riesgo promedio: {avg_score:.2f} (0-1)
- Nivel de alerta máximo: {max_level} (0=Normal, 1=Precaución, 2=Peligro)

Proporciona en MÁXIMO 150 palabras:
1. Evaluación del riesgo actual (bajo/medio/alto)
2. Predicción para las próximas 6 horas
3. Recomendación específica para las autoridades locales

Responde en formato JSON con las claves: "riesgo", "prediccion", "recomendacion"
"""
        
        # Llamar a Gemini API con sesión personalizada que ignora SSL
        # Usar gemini-2.0-flash (modelo sin thinking mode, más directo)
        url = f"https://generativelanguage.googleapis.com/v1/models/gemini-2.0-flash:generateContent?key={GEMINI_API_KEY}"
        headers = {"Content-Type": "application/json"}
        payload = {
            "contents": [{
                "parts": [{"text": prompt}]
            }],
            "generationConfig": {
                "temperature": 0.7,
                "maxOutputTokens": 500  # Aumentado para evitar cortes
            }
        }
        
        # Crear sesión con adaptador SSL personalizado que ignora verificación
        import ssl
        from requests.adapters import HTTPAdapter
        from urllib3.poolmanager import PoolManager
        
        class TLSAdapter(HTTPAdapter):
            def init_poolmanager(self, *args, **kwargs):
                ctx = ssl.create_default_context()
                ctx.check_hostname = False
                ctx.verify_mode = ssl.CERT_NONE
                kwargs['ssl_context'] = ctx
                return super(TLSAdapter, self).init_poolmanager(*args, **kwargs)
        
        session = requests.Session()
        session.mount('https://', TLSAdapter())
        
        response = session.post(url, headers=headers, json=payload, timeout=30, verify=False)
        
        if response.status_code == 200:
            result = response.json()
            
            # Extraer texto de la respuesta (manejo robusto para diferentes estructuras)
            try:
                # Intentar estructura estándar
                ai_text = result["candidates"][0]["content"]["parts"][0]["text"]
            except (KeyError, IndexError):
                try:
                    # Estructura alternativa (algunos modelos)
                    ai_text = result["candidates"][0]["text"]
                except (KeyError, IndexError):
                    # Si todo falla, mostrar estructura completa
                    logging.error(f"⚠️ Estructura inesperada de respuesta: {json.dumps(result, indent=2)}")
                    return None
            
            logging.info(f"🤖 Análisis IA completado: {ai_text[:100]}...")
            
            # Publicar a Ubidots
            publish_ai_recommendation(ai_text)
            
            return ai_text
        else:
            logging.error(f"❌ Error en API Gemini: {response.status_code} - {response.text}")
            return None
            
    except Exception as e:
        logging.error(f"❌ Error en análisis IA: {e}")
        import traceback
        logging.error(traceback.format_exc())
        return None


def publish_ai_recommendation(recommendation_text):
    """Publica la recomendación de IA en Ubidots"""
    try:
        payload = json.dumps({"value": 1, "context": {"recommendation": recommendation_text}})
        topic = UBIDOTS_TOPICS["ai_recommendation"]
        ubidots_mqtt_client.publish(topic, payload)
        logging.info(f" Recomendación IA enviada a Ubidots")
    except Exception as e:
        logging.error(f" Error al publicar recomendación IA: {e}")


def ai_analysis_loop():
    """Hilo que ejecuta análisis de IA periódicamente"""
    global last_ai_analysis
    
    while True:
        try:
            current_time = time.time()
            
            # Ejecutar análisis cada AI_ANALYSIS_INTERVAL segundos
            if current_time - last_ai_analysis >= AI_ANALYSIS_INTERVAL:
                logging.info(" Iniciando análisis con IA...")
                analyze_with_ai()
                last_ai_analysis = current_time
            
            time.sleep(30)  # Revisar cada 30 segundos
            
        except Exception as e:
            logging.error(f" Error en loop de IA: {e}")
            time.sleep(60)


# ============================================================================
# PROCESAMIENTO DE MENSAJES MQTT
# ============================================================================

def process_messages():
    """Hilo que procesa mensajes de la cola"""
    while True:
        try:
            # Obtener mensaje de la cola
            message = message_queue.get()
            payload = message["payload"]
            
            # Parsear JSON recibido del ESP32
            data = json.loads(payload)
            
            # Extraer variables
            humidity = float(data.get("H", 0))
            rain_raw = int(data.get("rainRaw", 0))
            rain_confirmed = int(data.get("rainConf", 0))
            vib_rms = float(data.get("rms", 0))
            vib_peak = float(data.get("peak", 0))
            pitch = float(data.get("pitch", 0))
            roll = float(data.get("roll", 0))
            risk_score = float(data.get("Score", 0))
            alert_level = int(data.get("level", 0))
            
            logging.info(f" Datos recibidos: Score={risk_score:.2f}, Nivel={alert_level}")
            
            # Almacenar en SQLite
            save_to_db(humidity, rain_raw, rain_confirmed, vib_rms, vib_peak,
                      pitch, roll, risk_score, alert_level)
            
            # Publicar a Ubidots
            ubidots_payloads = {
                "humidity": json.dumps({"value": humidity * 100}),  # En porcentaje
                "rain_raw": json.dumps({"value": rain_raw}),
                "rain_confirmed": json.dumps({"value": rain_confirmed}),
                "vibration_rms": json.dumps({"value": vib_rms}),
                "vibration_peak": json.dumps({"value": vib_peak}),
                "pitch": json.dumps({"value": pitch}),
                "roll": json.dumps({"value": roll}),
                "risk_score": json.dumps({"value": risk_score * 100}),  # En porcentaje
                "alert_level": json.dumps({"value": alert_level})
            }
            
            for var, topic in UBIDOTS_TOPICS.items():
                if var != "ai_recommendation":  # IA se publica por separado
                    ubidots_mqtt_client.publish(topic, ubidots_payloads[var])
            
            logging.info(f" Datos enviados a Ubidots (Score={risk_score:.2f})")
            
            message_queue.task_done()
            
        except json.JSONDecodeError as e:
            logging.error(f" Error al decodificar JSON: {e}")
        except Exception as e:
            logging.error(f" Error al procesar mensaje: {e}")


# ============================================================================
# CALLBACKS MQTT
# ============================================================================

def on_local_message(client, userdata, message):
    """Callback cuando llega mensaje del ESP32"""
    try:
        payload = message.payload.decode("utf-8")
        # Enviar a la cola para procesamiento asíncrono
        message_queue.put({"payload": payload})
    except Exception as e:
        logging.error(f" Error al recibir mensaje MQTT local: {e}")


def on_local_connect(client, userdata, flags, rc):
    """Callback cuando se conecta al broker local"""
    if rc == 0:
        logging.info(f" Conectado al broker local (Mosquitto)")
        client.subscribe(LOCAL_MQTT_TOPIC)
        logging.info(f" Suscrito a topic: {LOCAL_MQTT_TOPIC}")
    else:
        logging.error(f" Fallo al conectar al broker local, código: {rc}")


def on_ubidots_message(client, userdata, message):
    """Callback cuando llega mensaje de Ubidots (para control remoto)"""
    try:
        logging.info(" Mensaje recibido desde Ubidots")
        payload = message.payload.decode("utf-8")
        data = json.loads(payload)
        value = data.get("value", 0)
        
        # Ejemplo: Control remoto de silenciar alarma
        if value == 1:
            # Publicar comando al ESP32
            local_mqtt_client.publish("talud/control/silence", "1")
            logging.info(" Comando de silencio enviado al ESP32")
            
    except Exception as e:
        logging.error(f" Error al procesar mensaje de Ubidots: {e}")


def on_ubidots_connect(client, userdata, flags, rc):
    """Callback cuando se conecta al broker de Ubidots"""
    if rc == 0:
        logging.info(f" Conectado al broker de Ubidots")
        # Suscribirse a topic de control (para recibir comandos desde Ubidots)
        control_topic = f"/v1.6/devices/{UBIDOTS_DEVICE_LABEL}/alarm_control/lv"
        client.subscribe(control_topic)
        logging.info(f" Suscrito a topic de control: {control_topic}")
    else:
        logging.error(f" Fallo al conectar a Ubidots, código: {rc}")


# ============================================================================
# INICIALIZACIÓN Y MAIN
# ============================================================================

def start_mqtt_clients():
    """Inicia los clientes MQTT (local y Ubidots)"""
    try:
        # Configurar callbacks
        local_mqtt_client.on_connect = on_local_connect
        local_mqtt_client.on_message = on_local_message
        
        ubidots_mqtt_client.on_connect = on_ubidots_connect
        ubidots_mqtt_client.on_message = on_ubidots_message
        
        # Conectar a brokers
        local_mqtt_client.connect(LOCAL_MQTT_HOST, LOCAL_MQTT_PORT)
        ubidots_mqtt_client.username_pw_set(UBIDOTS_TOKEN, "")
        ubidots_mqtt_client.connect(UBIDOTS_MQTT_HOST, UBIDOTS_MQTT_PORT)
        
        # Iniciar loops
        local_mqtt_client.loop_start()
        ubidots_mqtt_client.loop_start()
        
        logging.info(" Clientes MQTT iniciados correctamente")
        
    except Exception as e:
        logging.error(f" Error al conectar a los brokers: {e}")
        raise


if __name__ == "__main__":
    logging.info("=" * 70)
    logging.info(" Gateway IoT para Sistema de Monitoreo de Taludes")
    logging.info("    Universidad de La Sabana - IoT 2025-2")
    logging.info("=" * 70)
    
    # Inicializar base de datos
    init_database()
    
    # Iniciar hilo para procesar mensajes
    processing_thread = threading.Thread(target=process_messages, daemon=True)
    processing_thread.start()
    logging.info(" Hilo de procesamiento de mensajes iniciado")
    
    # Iniciar hilo para análisis de IA (si está habilitado)
    if GEMINI_ENABLED:
        ai_thread = threading.Thread(target=ai_analysis_loop, daemon=True)
        ai_thread.start()
        logging.info(" Hilo de análisis IA iniciado")
    else:
        logging.warning(" Módulo de IA deshabilitado (configurar API key para activar)")
    
    # Iniciar clientes MQTT
    start_mqtt_clients()
    
    # Bucle principal (monitoreo)
    try:
        logging.info(" Gateway operativo - Presiona Ctrl+C para detener")
        while True:
            queue_size = message_queue.qsize()
            if queue_size > 0:
                logging.info(f"📊 Cola de mensajes: {queue_size} pendientes")
            time.sleep(10)
            
    except KeyboardInterrupt:
        logging.info("\n Deteniendo gateway...")
        local_mqtt_client.loop_stop()
        ubidots_mqtt_client.loop_stop()
        local_mqtt_client.disconnect()
        ubidots_mqtt_client.disconnect()
        logging.info(" Gateway detenido correctamente")
