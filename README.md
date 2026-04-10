## Sistema de Pub-Sub Deportivo en Tiempo Real

Un sistema de **publicación-suscripción** implementado en **C++** para distribuir actualizaciones deportivas en vivo (goles, cambios, tarjetas, etc.) usando múltiples protocolos de transporte.

### 📋 Tabla de Contenidos

- [Descripción General](#descripción-general)
- [Librerías Utilizadas](#librerías-utilizadas)
- [Descripción de Componentes](#descripción-de-componentes)
- [Protocolo de Comunicación](#protocolo-de-comunicación)
- [Implementaciones por Protocolo](#implementaciones-por-protocolo)
  - [TCP](#tcp)
  - [UDP](#udp-próximamente)
  - [QUIC](#quic)
- [Requisitos Generales](#requisitos-generales)
- [Compilación y Ejecución](#compilación-y-ejecución)
- [Características](#características)
- [Limitaciones](#limitaciones)
- [Notas Técnicas](#notas-técnicas)

---

### Descripción General

**Estado Actual**: Implementación completa con **TCP**

Sistema distribuido que implemena el patrón de publicación-suscripción para:

- ✅ **TCP**: Confiabilidad garantizada en tiempo real
- ⏳ **UDP**: Baja latencia con entrega no garantizada
- ✅ **QUIC**: Conexiones multiplexadas con soporte 0-RTT

### Librerías Utilizadas

#### Librerías Estándar de C++ (C++11)

| Librería       | Propósito              | Uso                                            |
| -------------- | ---------------------- | ---------------------------------------------- |
| `<iostream>`   | I/O estándar           | Impresión de mensajes de estado (`std::cout`)  |
| `<string>`     | Manejo de strings      | Almacenamiento de topics, mensajes y nombres   |
| `<vector>`     | Contenedores dinámicos | Listas de suscriptores, eventos, args          |
| `<map>`        | Diccionarios ordenados | Mapeo `topic → lista de suscriptores`          |
| `<thread>`     | Multithreading         | Manejo de clientes concurrentes en broker      |
| `<mutex>`      | Sincronización         | Protección de acceso concurrente a estructuras |
| `<lock_guard>` | RAII para mutex        | Lock automático en bloques sincronizados       |

#### Librerías del Sistema (POSIX)

| Librería         | Propósito                   | Uso                                                                     |
| ---------------- | --------------------------- | ----------------------------------------------------------------------- |
| `<sys/socket.h>` | API de sockets              | Creación de sockets TCP (`socket()`, `bind()`, `listen()`, `connect()`) |
| `<netinet/in.h>` | Estructuras de red          | Direcciones IPv4 (`struct sockaddr_in`)                                 |
| `<arpa/inet.h>`  | Conversiones de direcciones | `inet_pton()`, `inet_ntop()`                                            |
| `<unistd.h>`     | I/O de bajo nivel           | `read()`, `write()`, `close()`, `fork()`                                |
| `<cstring>`      | Operaciones de memoria      | `memset()`, `strlen()`, `strcpy()`                                      |
| `<cstdlib>`      | Utilidades estándar         | `atoi()`, `exit()`, `malloc()`                                          |

#### Compilación con Flags

```bash
g++ -std=c++11 -pthread -o [binary] [source.cpp]
```

- `-std=c++11`: Habilita características de C++11 (threads, mutex, etc.)
- `-pthread`: Enlaza la librería POSIX de threading

### Descripción de Componentes

#### 🖥️ Broker (Servidor Central)

Actúa como intermediario entre publicadores y suscriptores:

- Recibe mensajes `PUBLISH` y los distribuye
- Gestiona suscripciones dinámicas a topics (partidos)
- Maneja múltiples clientes concurrentemente con threads
- Mantiene estado: mapa `topic → [suscriptores]`

#### 📤 Publisher (Publicador)

Simula periodistas deportivos reportando eventos en vivo:

- Se conecta al broker y publica eventos
- Envía 14 eventos deportivos por publicador
- Eventos incluyen: goles, tarjetas, cambios de jugadores, etc.
- Intervalo entre eventos: 1-3 segundos

#### 📥 Subscriber (Suscriptor)

Clientes que se suscriben a uno o múltiples partidos:

- Se conectan al broker y envían suscripciones
- Reciben actualizaciones en tiempo real
- Pueden suscribirse a varios partidos simultáneamente
- Se ejecutan indefinidamente hasta `Ctrl+C`

### Protocolo de Comunicación

#### Formato de Mensajes

```
PUBLISH:topic:message
SUBSCRIBE:topic
```

**Ejemplos:**

```
PUBLISH:Real_Madrid_vs_Barcelona:Goal by Player #7 at minute 12
SUBSCRIBE:Real_Madrid_vs_Barcelona
```

#### Flujo de Comunicación (Independiente del Protocolo)

1. **Suscriptor** → Broker: `SUBSCRIBE:Real_Madrid_vs_Barcelona`
2. **Broker** → Registra suscripción en memoria
3. **Publicador** → Broker: `PUBLISH:Real_Madrid_vs_Barcelona:Goal...`
4. **Broker** → Distribuye a suscriptores del topic
5. **Suscriptor** ← Broker: `[Real_Madrid_vs_Barcelona] Goal...`

---

## Implementaciones por Protocolo

### TCP

Para poder probar al 100% el sistema, toca cambiar la variable local `BROKER_IP` en `tcp/publisher.cpp` y en `tcp/subscriber.cpp` a la IP del broker, esta asignada por default a `192.168.77.148` ya que las pruebas se realizaron usando 3 máquinas virtuales y esa sería la IP de la máquina virtual del Broker; por lo que para probar localmente tocaría cambiarlo a `127.0.0.1` (en sistemas basados en Linux).

#### Requisitos Específicos

- **Compilador**: g++ con soporte C++11
- **Sistema Operativo**: Ubuntu Server 24 (POSIX)
- **Librerías**: pthread (compilada con `-pthread`)
- **Puerto**: 5000 (TCP)

#### Características Específicas TCP

✅ **Confiabilidad**: Los mensajes llegan completos y en orden
✅ **Multithreading**: Broker maneja múltiples clientes concurrentemente
✅ **Suscripciones dinámicas**: Clientes pueden conectar/desconectar en tiempo real
✅ **Control de flujo**: TCP maneja automáticamente backpressure
✅ **Detección de desconexiones**: Conexiones perdidas se detectan inmediatamente

#### Ventajas vs. Desventajas

| Ventaja                     | Desventaja                                  |
| --------------------------- | ------------------------------------------- |
| Garantía de entrega         | Mayor latencia por handshakes               |
| Preserva orden              | Más overhead de conexión                    |
| Control de flujo automático | No adecuado para streaming de baja latencia |

#### Compilación TCP

```bash
cd tcp

g++ -std=c++11 -pthread -o broker broker.cpp
g++ -std=c++11 -pthread -o publisher publisher.cpp
g++ -std=c++11 -pthread -o subscriber subscriber.cpp
```

#### Ejecución TCP

**Terminal 1 - Broker:**

```bash
# Asumiendo que sigues en tcp/
./broker
```

**Terminal 2 - Subscriber:**

```bash
# Asumiendo que sigues en tcp/
./subscriber Fan_John Real_Madrid_vs_Barcelona
```

**Terminal 3 - Publisher:**

```bash
# Asumiendo que sigues en tcp/
./publisher Real_Madrid_vs_Barcelona
```

#### Ejemplo de Salida TCP

```bash
# Terminal 1 (Broker)
[BROKER] Escuchando en puerto 5000
[BROKER] Iniciando servidor...
[BROKER] Cliente 0 suscrito al tema 'Real_Madrid_vs_Barcelona'
[BROKER] Publicado en tema 'Real_Madrid_vs_Barcelona'

# Terminal 2 (Subscriber)
[SUBSCRIBER Fan_John] Conectado al broker en 192.168.77.148:5000
[SUBSCRIBER Fan_John] Suscrito a: Real_Madrid_vs_Barcelona
[SUBSCRIBER Fan_John] ACTUALIZACIÓN: [Real_Madrid_vs_Barcelona] Goal by Player #7

# Terminal 3 (Publisher)
[PUBLISHER Real_Madrid_vs_Barcelona] Conectado al broker
[PUBLISHER Real_Madrid_vs_Barcelona] Enviado: Gol del jugador #7 en el minuto 12
```

---

### UDP
_Archivos: `udp/broker_udp.cpp`, `udp/publisher_udp.cpp`, `udp/subscriber_udp.cpp`_

Todo similar a TCP, salvo los siguientes cambios principales:
#### Librerias adicionales UDP

`<algorithm>`
 Algoritmos STL           
 Búsqueda de suscriptores duplicados                  

`<atomic>`
 Variables atómicas       

`<csignal>`
 Manejo de señales        

`<chrono>`
 Medición de tiempo       


### QUIC

Para poder probar al 100% el sistema, toca cambiar la variable local `BROKER_IP` en `quic/publisher_quic.cpp` y en `quic/subscriber_quic.cpp` a la IP del broker, esta asignada por default a `192.168.199.129` ya que las pruebas se realizaron usando 3 máquinas virtuales y esa sería la IP de la máquina virtual del Broker.

#### Requisitos Específicos

- **Compilador**: g++ con soporte C++11
- **Sistema Operativo**: Ubuntu Server 24 (POSIX)
- **Librerías**: pthread (compilada con `-pthread`)
- **Puertos**:
  - `5001` UDP — broker recibe mensajes de publishers
  - `5002` UDP — broker envía mensajes a subscribers

#### Características Específicas QUIC

✅ **Números de secuencia**: cada `PUBLISH` y `DELIVER` lleva un `seq_num` de 32 bits  
✅ **ACKs bidireccionales**: el broker confirma al publisher y el subscriber confirma al broker  
✅ **Retransmisión automática**: si no llega ACK en 500 ms se reenvía, hasta 5 intentos  
✅ **Reordenamiento**: el subscriber entrega mensajes en orden ascendente de `seq_num`  
✅ **Detección de duplicados**: paquetes ya entregados son descartados silenciosamente  
✅ **Multithreading**: broker corre 3 hilos (publishers, subscribers, retransmisión)  
✅ **Compatibilidad con VMware/NAT**: el subscriber anuncia su IP real al broker 

#### Ventajas vs. Desventajas

| Ventaja                                     | Desventaja                                           |
|---------------------------------------------|------------------------------------------------------|
| Fiabilidad garantizada sobre UDP            | Mayor complejidad de implementación que TCP puro     |
| Menor overhead de conexión que TCP          | ACK por mensaje introduce latencia adicional vs UDP  |
| Orden de entrega garantizado                | Sin control de congestión a nivel de red             |
| Recuperación ante pérdida de paquetes       | Retransmisión consume ancho de banda extra           |
| Funciona con NAT y VMware Host-only         | Requiere que el subscriber conozca su propia IP      |

#### Parámetros Configurables

Los siguientes valores se pueden ajustar en `quic_protocol.h`:

| Constante              | Valor por defecto | Descripción                            |
|------------------------|-------------------|----------------------------------------|
| `ACK_TIMEOUT_MS`       | 500 ms            | Tiempo de espera antes de retransmitir |
| `MAX_RETRIES`          | 5                 | Intentos máximos por paquete           |
| `MAX_PAYLOAD_LEN`      | 900 bytes         | Tamaño máximo del cuerpo del mensaje   |
| `BROKER_PORT_QUIC`     | 5001              | Puerto para publishers                 |
| `BROKER_SUB_PORT_QUIC` | 5002              | Puerto para subscribers                |

---

#### Compilación QUIC

```bash
cd quic

g++ -std=c++17 -pthread -o broker_quic broker_quic.cpp
g++ -std=c++17 -pthread -o publisher_quic publisher_quic.cpp
g++ -std=c++17 -pthread -o subscriber_quic subscriber_quic.cpp
```

#### Ejecución QUIC

> ⚠️ **Orden importante**: siempre arrancar primero el broker, luego el subscriber y por último el publisher.

**Terminal 1 — Broker:**

```bash
# Asumiendo que sigues en quic/
./broker_quic
```

**Terminal 2 — Subscriber:**

```bash
# Asumiendo que sigues en quic/
./subscriber_quic Fan_Juan Real_Madrid_vs_Barcelona 192.168.199.129

```
**Terminal 3 — Publisher:**
```bash
# Asumiendo que sigues en quic/
./publisher_quic Real_Madrid_vs_Barcelona 192.168.199.129
```

#### Ejemplo de Salida QUIC

```bash
# Terminal 1 (Broker)
[BROKER] Escuchando publishers en puerto  5001
[BROKER] Escuchando suscriptores en puerto 5002
[BROKER] Iniciando servidor QUIC-like...
[BROKER] Cliente 192.168.199.131:36602 suscrito al tema 'Real_Madrid_vs_Barcelona'
[BROKER] PUBLISH seq=1 tema='Real_Madrid_vs_Barcelona' msg='Gol del jugador #7 en el minuto 12'
[BROKER] → DELIVER seq=1 a 192.168.199.131:36602
[BROKER] ✔ ACK seq=1 → publisher 192.168.199.131:57278
[BROKER] ✔ SUB_ACK de 192.168.199.131:36602 seq=1

# Terminal 2 (Subscriber)
[SUBSCRIBER Fan_Juan] Conectado al broker en 192.168.199.129:5002
[SUBSCRIBER Fan_Juan] Mi dirección: 192.168.199.131:36602
[SUBSCRIBER Fan_Juan] Suscrito a: Real_Madrid_vs_Barcelona
[SUBSCRIBER Fan_Juan] Esperando actualizaciones...
[SUBSCRIBER Fan_Juan] ACTUALIZACIÓN: [Real_Madrid_vs_Barcelona] Gol del jugador #7 en el minuto 12

# Terminal 3 (Publisher)
[PUBLISHER Real_Madrid_vs_Barcelona] Conectado al broker en 192.168.199.129:5001
[PUBLISHER Real_Madrid_vs_Barcelona] Iniciando publicación de eventos...
[PUBLISHER Real_Madrid_vs_Barcelona] Enviado: Gol del jugador #7 en el minuto 12
[PUBLISHER Real_Madrid_vs_Barcelona] ✔ ACK recibido seq=1
```

#### Ejemplo de Retransmisión (pérdida simulada)

Cuando un subscriber se desconecta a mitad de la transmisión, el broker detecta la falta de `SUB_ACK` y retransmite automáticamente:

```bash
# Terminal 1 (Broker) — cuando el subscriber se cae
[BROKER] ↩ Retransmitiendo DELIVER seq=8 → 192.168.77.150:36602 (intento 1/5)
[BROKER] ↩ Retransmitiendo DELIVER seq=8 → 192.168.77.150:36602 (intento 2/5)
[BROKER] ↩ Retransmitiendo DELIVER seq=8 → 192.168.77.150:36602 (intento 3/5)
[BROKER] ↩ Retransmitiendo DELIVER seq=8 → 192.168.77.150:36602 (intento 4/5)
[BROKER] ↩ Retransmitiendo DELIVER seq=8 → 192.168.77.150:36602 (intento 5/5)
[BROKER] ⚠ Sin SUB_ACK para seq=8 destino=192.168.77.150:36602 — se descarta
```

---

Implementación moderna con multiplex y 0-RTT.
_Archivos: `broker_quic.cpp`, `publisher_quic.cpp`, `subscriber_quic.cpp`, `quic_protocol.h`_


---

## Requisitos Generales

- **Compilador**: g++ con soporte C++11 o superior
- **Sistema Operativo**: Ubuntu Server 24 (POSIX compatible)
- **No requiere librerías externas** (solo estándar de C++ y POSIX)

---

## Compilación y Ejecución

### Archivos del Proyecto

```
Lab3-Redes/
├── tcp/                   # ✅ Implementación TCP completada
│   ├── broker.cpp
│   ├── publisher.cpp
│   ├── subscriber.cpp
│   ├── broker            # Ejecutable
│   ├── publisher         # Ejecutable
│   └── subscriber        # Ejecutable
├── udp/                   # ⏳ En desarrollo
│   ├── broker.cpp
│   ├── publisher.cpp
│   └── subscriber.cpp
├── quic/                  # ✅ Implementación QUIC completada
│   ├── broker_quic.cpp
│   ├── publisher_quic.cpp
|   ├── subscriber_quic.cpp
│   └── quic_protocol.h

└── README.md             # Este archivo
```

## Características

### Implementadas ✅

- ✅ Patrón Pub-Sub completo (TCP y QUIC)
- ✅ Multithreading en broker (QUIC)
- ✅ Sincronización con mutex 
- ✅ Suscripciones dinámicas
- ✅ 14+ eventos por publicador
- ✅ Soporte múltiples topics
- ✅ ACKs bidireccionales (QUIC)
- ✅ Retransmisión automática con timeout (QUIC)
- ✅ Números de secuencia y reordenamiento (QUIC)
- ✅ Detección de duplicados (QUIC)

### En Desarrollo ⏳

- ⏳ UDP (connectionless, baja latencia)

---

## Limitaciones

### TCP

- Máximo 100 clientes simultáneos (configurable)
- Buffer de 1024 bytes por mensaje (configurable)
- Sin persistencia (mensajes se pierden)
- Sin autenticación/encriptación

### UDP (cuando se implemente)

- Sin garantía de entrega
- Sin orden garantizado
- Sin control de flujo automático

### QUIC

- ACK por mensaje introduce latencia adicional frente a UDP puro
- Sin control de congestión a nivel de red
- Máximo 5 reintentos por paquete antes de descartar (configurable)
- Buffer de 900 bytes por mensaje (configurable)
- Sin persistencia (mensajes se pierden si el broker cae)
- Sin autenticación/encriptación

---

## Notas Técnicas

### Seguridad

- **Thread-safe**: Mutex protegen secciones críticas
- **Gestión de memoria**: RAII en constructores/destructores
- **No hay buffer overflows**: Uso de `std::string` (bounds-checked)

### Portabilidad

- **POSIX sockets**: Compatible con Linux, macOS, BSD
- **C++11 standard**: Compatible con gcc, clang
- **Sin dependencias externas**: Solo librería estándar
