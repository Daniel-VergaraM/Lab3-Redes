#include "quic_protocol.h"

#include <iostream>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <thread>
#include <chrono>
#include <vector>

using namespace std;

const char* BROKER_IP_DEFAULT = "192.168.77.148";

class Publisher {
private:
    string match_name;
    int    socket_fd;

    vector<string> events = {
        "Gol del jugador #7 en el minuto 12",
        "Tarjeta amarilla al jugador #5",
        "Sustitución: jugador #10 sale, entra jugador #11",
        "Gol del jugador #9 en el minuto 28",
        "Saque de esquina para el equipo A",
        "Penal concedido al equipo B",
        "Gol del jugador #3 en el minuto 35",
        "Tarjeta amarilla al jugador #8",
        "Gol del jugador #7 en el minuto 42",
        "Descanso: Marcador 2-2",
        "Comienza la segunda mitad",
        "Gol del jugador #6 en el minuto 58",
        "Tarjeta roja al jugador #4",
        "Gol del jugador #9 en el minuto 65"
    };

    sockaddr_in broker_addr;

public:
    Publisher(const string& match, const char* broker_ip)
        : match_name(match), socket_fd(-1)
    {
        memset(&broker_addr, 0, sizeof(broker_addr));
        broker_addr.sin_family = AF_INET;
        broker_addr.sin_port   = htons(BROKER_PORT_QUIC);
        inet_pton(AF_INET, broker_ip, &broker_addr.sin_addr);
    }

    // connect_to_broker: crea socket UDP
    bool connect_to_broker() {
        socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (socket_fd < 0) {
            cerr << "[PUBLISHER " << match_name << "] Error al crear socket" << endl;
            return false;
        }

        struct timeval tv{ 0, ACK_TIMEOUT_MS * 1000 };
        setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &broker_addr.sin_addr, ip_str, sizeof(ip_str));
        cout << "[PUBLISHER " << match_name << "] Conectado al broker en "
             << ip_str << ":" << BROKER_PORT_QUIC << endl;
        return true;
    }

    void publish_events() {
        cout << "[PUBLISHER " << match_name << "] Iniciando publicación de eventos..." << endl;

        uint32_t seq = 1;

        for (int i = 0; i < (int)events.size(); i++) {
            string msg_content = events[i];

            cout << "[PUBLISHER " << match_name << "] Enviado: " << msg_content << endl;

            bool ok = publish_with_ack(match_name, msg_content, seq);
            if (!ok) {
                cerr << "[PUBLISHER " << match_name << "] Mensaje seq=" << seq
                     << " perdido tras " << MAX_RETRIES << " intentos" << endl;
            }
            seq++;

            // Retraso variable 1-3 s 
            sleep(1 + (rand() % 3));
        }

        cout << "[PUBLISHER " << match_name << "] Finalizada publicación de eventos" << endl;
    }

    void close_connection() {
        if (socket_fd >= 0) {
            close(socket_fd);
            cout << "[PUBLISHER " << match_name << "] Desconectado del broker" << endl;
            socket_fd = -1;
        }
    }

    ~Publisher() {
        close_connection();
    }

private:
    bool publish_with_ack(const string& topic, const string& message, uint32_t seq) {
        QuicPacket pkt{};
        set_magic(pkt.header);
        pkt.header.type        = PacketType::PUBLISH;
        pkt.header.seq_num     = seq;
        pkt.header.ack_num     = 0;
        pkt.header.payload_len = (uint16_t)min((int)message.size(), MAX_PAYLOAD_LEN - 1);
        strncpy(pkt.header.topic, topic.c_str(), MAX_TOPIC_LEN - 1);
        memcpy(pkt.payload, message.c_str(), pkt.header.payload_len);

        char wire_buf[sizeof(QuicHeader) + MAX_PAYLOAD_LEN];
        int  wire_len = pkt.serialize(wire_buf, sizeof(wire_buf));

        for (int attempt = 0; attempt <= MAX_RETRIES; attempt++) {
            if (attempt > 0) {
                cout << "[PUBLISHER " << match_name << "] ↩ Retransmitiendo seq="
                     << seq << " (intento " << attempt << "/" << MAX_RETRIES << ")" << endl;
            }

            // Enviar paquete PUBLISH
            sendto(socket_fd, wire_buf, wire_len, 0,
                   (sockaddr*)&broker_addr, sizeof(broker_addr));

            // Esperar ACK del broker
            char      ack_buf[sizeof(QuicHeader) + MAX_PAYLOAD_LEN];
            sockaddr_in from{};
            socklen_t   from_len = sizeof(from);
            int n = recvfrom(socket_fd, ack_buf, sizeof(ack_buf), 0,
                             (sockaddr*)&from, &from_len);
            if (n < 0) continue;  // timeout → reintentar

            QuicPacket ack_pkt;
            if (!ack_pkt.deserialize(ack_buf, n)) continue;

            if (ack_pkt.header.type   == PacketType::ACK &&
                ack_pkt.header.ack_num == seq) {
                cout << "[PUBLISHER " << match_name << "] ✔ ACK recibido seq="
                     << seq << endl;
                return true;
            }
        }
        return false;
    }
};

int main(int argc, char* argv[]) {
    if (argc < 2) {
        cerr << "Uso: " << argv[0] << " <nombre_partido> [ip_broker]" << endl;
        cerr << "Ejemplo: " << argv[0] << " Real_Madrid_vs_Barcelona 192.168.77.148" << endl;
        return 1;
    }

    string match_name = argv[1];
    const char* broker_ip = (argc >= 3) ? argv[2] : BROKER_IP_DEFAULT;

    Publisher publisher(match_name, broker_ip);

    if (!publisher.connect_to_broker()) {
        cerr << "Fallo al conectar con broker" << endl;
        return 1;
    }

    publisher.publish_events();
    publisher.close_connection();

    return 0;
}