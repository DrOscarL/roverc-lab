"""
╔══════════════════════════════════════════════════════════════╗
║   LABORATORIO IoT — Misión Autónoma RoverC.Pro               ║
║   Especialización IoT — Corporación Universitaria Americana  ║
╚══════════════════════════════════════════════════════════════╝

OBJETIVO:
  Programar una secuencia de comandos MQTT que haga al robot
  completar la misión asignada sin control manual.

INSTRUCCIONES:
  1. Instalar dependencia:  pip install paho-mqtt
  2. Completar los TODO marcados en el código
  3. Ejecutar:  python mision_rover.py
  4. Observar el robot y ajustar tiempos hasta completar la misión

TOPICS DISPONIBLES:
  Publicar en  rover/cmd     → {"dir": "fwd|bck|left|right|cw|ccw|stop", "spd": 0-127}
  Publicar en  rover/grip    → {"angle": 30-150}  (30=abierta, 150=cerrada)
  Publicar en  rover/beep    → {"freq": 440, "dur": 200}
  Suscribir    rover/telemetry → recibe JSON con bat_v, ax, ay, az, rssi, etc.
  Suscribir    rover/turno   → indica el client_id que tiene el turno activo

MISIÓN DE EJEMPLO:
  [ ] Avanzar hasta el objeto
  [ ] Cerrar pinza (recoger objeto)
  [ ] Girar 180°
  [ ] Volver al origen
  [ ] Abrir pinza (depositar objeto)
  [ ] Emitir beep de misión completada
"""

import paho.mqtt.client as mqtt
import json
import time
import sys

# ─── CONFIGURACIÓN — no modificar ────────────────────────────
BROKER   = "5dd303f2e6fc4da88f4f15cd6837e63a.s1.eu.hivemq.cloud"
PORT     = 8883
USER     = "rover_user"
PASSWORD = "PruebaNumero1**"

# TODO: Cambia este ID por tu nombre (sin espacios ni caracteres especiales)
# Ejemplo: "estudiante_carlos", "grupo_3"
MI_ID    = "estudiante_TUNOMBRE"

# ─── Topics ───────────────────────────────────────────────────
T_CMD    = "rover/cmd"
T_GRIP   = "rover/grip"
T_BEEP   = "rover/beep"
T_TELEM  = "rover/telemetry"
T_TURNO  = "rover/turno"

# ─── Variables globales ───────────────────────────────────────
telemetria_actual = {}
tengo_turno       = False
mision_iniciada   = False

# ─── Callbacks MQTT ───────────────────────────────────────────
def on_connect(client, userdata, flags, rc, properties=None):
    if rc == 0:
        print(f"[✓] Conectado al broker como '{MI_ID}'")
        client.subscribe(T_TELEM)
        client.subscribe(T_TURNO)
        print(f"[✓] Suscrito a {T_TELEM} y {T_TURNO}")
    else:
        print(f"[✗] Error de conexión: código {rc}")
        sys.exit(1)

def on_message(client, userdata, msg):
    global telemetria_actual, tengo_turno

    if msg.topic == T_TELEM:
        try:
            telemetria_actual = json.loads(msg.payload.decode())
        except:
            pass

    elif msg.topic == T_TURNO:
        turno_id = msg.payload.decode()
        if turno_id == MI_ID:
            if not tengo_turno:
                print(f"\n[▶] ¡Es tu turno, {MI_ID}!")
            tengo_turno = True
        else:
            if tengo_turno:
                print(f"[■] Turno cedido a {turno_id}")
            tengo_turno = False

def on_disconnect(client, userdata, rc, properties=None):
    print("[!] Desconectado del broker")

# ─── Helpers de control ───────────────────────────────────────
def cmd(direccion: str, velocidad: int = 80):
    """Envía comando de movimiento al robot."""
    if not tengo_turno:
        print(f"  [!] No tienes el turno — comando '{direccion}' ignorado")
        return
    payload = json.dumps({"dir": direccion, "spd": velocidad})
    client.publish(T_CMD, payload)
    print(f"  → CMD: {direccion:6s}  spd={velocidad}")

def grip(angulo: int):
    """Controla la pinza. 30=abierta, 150=cerrada."""
    if not tengo_turno:
        print(f"  [!] No tienes el turno — grip ignorado")
        return
    payload = json.dumps({"angle": angulo})
    client.publish(T_GRIP, payload)
    estado = "ABIERTA" if angulo <= 30 else "CERRADA" if angulo >= 150 else f"{angulo}°"
    print(f"  → GRIP: {estado}")

def beep(frecuencia: int = 880, duracion: int = 300):
    """Emite un pitido en el robot."""
    client.publish(T_BEEP, json.dumps({"freq": frecuencia, "dur": duracion}))

def parar():
    """Detiene el robot inmediatamente."""
    cmd("stop", 0)

def esperar(segundos: float, motivo: str = ""):
    """Pausa la ejecución. Muestra telemetría mientras espera."""
    if motivo:
        print(f"  ⏱  Esperando {segundos}s — {motivo}")
    else:
        print(f"  ⏱  Esperando {segundos}s...")
    time.sleep(segundos)

def leer_telemetria():
    """Retorna un snapshot de la última telemetría recibida."""
    return telemetria_actual.copy()

def solicitar_turno():
    """Publica tu ID en rover/turno para pedir el control."""
    client.publish(T_TURNO, MI_ID)
    print(f"[→] Solicitando turno como '{MI_ID}'...")
    time.sleep(1.5)
    if tengo_turno:
        print("[✓] Turno obtenido")
    else:
        print("[!] Esperando turno... (otro estudiante puede tener el control)")

def liberar_turno():
    """Libera el turno para que otro estudiante pueda controlar."""
    parar()
    client.publish(T_TURNO, "libre")
    print("[◻] Turno liberado")

# ─── MISIÓN — COMPLETA ESTA FUNCIÓN ───────────────────────────
def ejecutar_mision():
    """
    Aquí defines la secuencia de tu misión autónoma.

    CONSEJOS:
    - Empieza con velocidades bajas (60-80) para calibrar tiempos
    - Siempre llama parar() antes de cambiar de dirección
    - Usa esperar() para dar tiempo al robot a ejecutar cada movimiento
    - Llama leer_telemetria() para verificar el estado del robot
    - Los tiempos dependen de la superficie — calibra empíricamente

    MISIÓN ASIGNADA:
    Partiendo desde la base:
      1. Avanzar hasta el objeto (distancia ~40 cm)
      2. Recoger el objeto con la pinza
      3. Girar 180° y volver al origen
      4. Depositar el objeto
      5. Beep de misión completada
    """

    print("\n" + "═"*50)
    print("  INICIANDO MISIÓN AUTÓNOMA")
    print("═"*50)

    # Verificar batería antes de empezar
    telem = leer_telemetria()
    if telem:
        bat = telem.get("bat_pct", 100)
        print(f"  Batería: {bat}%")
        if bat < 15:
            print("  [!] Batería baja — misión cancelada")
            return

    # ── PASO 1: Abrir pinza antes de avanzar ─────────────────
    print("\n[1/5] Abriendo pinza...")
    grip(30)                        # pinza abierta
    esperar(0.8, "pinza abre")

    # ── PASO 2: Avanzar hacia el objeto ──────────────────────
    print("\n[2/5] Avanzando hacia el objeto...")
    cmd("fwd", 80)

    # TODO: Ajusta este tiempo según la distancia real al objeto
    esperar(2.0, "avanzando")       # ← CALIBRAR

    parar()
    esperar(0.3, "freno")

    # ── PASO 3: Cerrar pinza (recoger objeto) ────────────────
    print("\n[3/5] Recogiendo objeto...")
    grip(150)                       # pinza cerrada
    esperar(1.0, "pinza cierra")

    # ── PASO 4: Girar 180° ───────────────────────────────────
    print("\n[4/5] Girando 180°...")
    cmd("cw", 70)

    # TODO: Ajusta este tiempo para conseguir exactamente 180°
    esperar(1.6, "girando")         # ← CALIBRAR

    parar()
    esperar(0.3, "freno")

    # ── Volver al origen ─────────────────────────────────────
    print("  Volviendo al origen...")
    cmd("fwd", 80)
    esperar(2.0, "regresando")      # mismo tiempo que el avance
    parar()
    esperar(0.3, "freno")

    # ── PASO 5: Depositar objeto ─────────────────────────────
    print("\n[5/5] Depositando objeto...")
    grip(30)                        # pinza abierta
    esperar(0.8, "depositando")

    # ── Misión completada ────────────────────────────────────
    print("\n" + "═"*50)
    print("  ✓ MISIÓN COMPLETADA")
    print("═"*50)

    # Beep de celebración
    beep(523, 100); time.sleep(0.12)
    beep(659, 100); time.sleep(0.12)
    beep(784, 100); time.sleep(0.12)
    beep(1047, 300)

    # Mostrar telemetría final
    telem = leer_telemetria()
    if telem:
        print(f"\n  Telemetría final:")
        print(f"    Batería : {telem.get('bat_v','--')}V ({telem.get('bat_pct','--')}%)")
        print(f"    RSSI    : {telem.get('rssi','--')} dBm")
        print(f"    Accel Z : {telem.get('az','--')} g")


# ─── MAIN ─────────────────────────────────────────────────────
if __name__ == "__main__":

    if MI_ID == "estudiante_TUNOMBRE":
        print("[✗] Debes cambiar MI_ID por tu nombre antes de ejecutar.")
        sys.exit(1)

    # Crear cliente MQTT
    client = mqtt.Client(
        client_id  = MI_ID,
        protocol   = mqtt.MQTTv311,
        transport  = "tcp"
    )
    client.tls_set()
    client.username_pw_set(USER, PASSWORD)
    client.on_connect    = on_connect
    client.on_message    = on_message
    client.on_disconnect = on_disconnect

    # Conectar
    print(f"[→] Conectando a {BROKER}:{PORT}...")
    client.connect(BROKER, PORT, keepalive=30)
    client.loop_start()
    time.sleep(2)  # esperar conexión y primera telemetría

    try:
        # Solicitar turno
        solicitar_turno()

        if tengo_turno:
            ejecutar_mision()
        else:
            print("[!] No se obtuvo el turno. Espera a que el turno sea liberado.")
            print("    Puedes ejecutar el script nuevamente cuando esté libre.")

    except KeyboardInterrupt:
        print("\n[!] Interrupción manual — deteniendo robot...")
        parar()

    finally:
        liberar_turno()
        time.sleep(0.5)
        client.loop_stop()
        client.disconnect()
        print("[◻] Desconectado. Fin de sesión.")
