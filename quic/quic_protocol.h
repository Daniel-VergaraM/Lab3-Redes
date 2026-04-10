#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <chrono>

// ---- Puertos ----
const int BROKER_PORT_QUIC     = 5001;  // recibe PUBLISH y SUBSCRIBE de clientes
const int BROKER_SUB_PORT_QUIC = 5002;  // envía DELIVER a suscriptores

const int BUFFER_SIZE_QUIC = 1024;
const int MAX_TOPIC_LEN    = 128;
const int MAX_PAYLOAD_LEN  = 900;

// ---- Parámetros de fiabilidad ----
const int ACK_TIMEOUT_MS = 500;   // ms antes de retransmitir
const int MAX_RETRIES    = 5;     // intentos máximos por paquete

// ---- Tipos de paquete ----
enum class PacketType : uint8_t {
    PUBLISH   = 0x01,  // publisher  → broker : nuevo mensaje
    ACK       = 0x02,  // broker     → publisher : confirmación
    SUBSCRIBE = 0x03,  // subscriber → broker : registro de interés
    DELIVER   = 0x04,  // broker     → subscriber : mensaje reenviado
    SUB_ACK   = 0x05,  // subscriber → broker : entrega confirmada
};

// ---- Cabecera binaria fija ----
#pragma pack(push, 1)
struct QuicHeader {
    uint8_t     magic[4];        // identificador de protocolo
    PacketType  type;            // tipo de paquete
    uint32_t    seq_num;         // número de secuencia del emisor
    uint32_t    ack_num;         // secuencia que se está confirmando (en ACK/SUB_ACK)
    char        topic[MAX_TOPIC_LEN];   // nombre del tema/partido
    uint16_t    payload_len;     // longitud del cuerpo (bytes tras la cabecera)
};
#pragma pack(pop)

// ---- Paquete completo ----
struct QuicPacket {
    QuicHeader header;
    char       payload[MAX_PAYLOAD_LEN];

    // Serializar a buffer de red; devuelve bytes totales o -1 si no cabe
    int serialize(char* buf, int buf_size) const {
        int total = (int)sizeof(QuicHeader) + header.payload_len;
        if (total > buf_size) return -1;
        memcpy(buf, &header, sizeof(QuicHeader));
        memcpy(buf + sizeof(QuicHeader), payload, header.payload_len);
        return total;
    }

    // Deserializar desde buffer de red; devuelve false si el magic no coincide
    bool deserialize(const char* buf, int len) {
        if (len < (int)sizeof(QuicHeader)) return false;
        memcpy(&header, buf, sizeof(QuicHeader));
        if (memcmp(header.magic, "QUIC", 4) != 0) return false;
        int payload_bytes = std::min((int)header.payload_len, MAX_PAYLOAD_LEN - 1);
        memcpy(payload, buf + sizeof(QuicHeader), payload_bytes);
        payload[payload_bytes] = '\0';
        return true;
    }
};

// ---- Utilidades ----
inline void set_magic(QuicHeader& h) {
    h.magic[0]='Q'; h.magic[1]='U'; h.magic[2]='I'; h.magic[3]='C';
}

inline int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
