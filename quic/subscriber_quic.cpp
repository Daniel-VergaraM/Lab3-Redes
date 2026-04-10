#include "quic_protocol.h"

#include <iostream>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <thread>
#include <mutex>
#include <atomic>
#include <map>
#include <set>
#include <vector>
#include <chrono>
#include <csignal>

using namespace std;

const char* BROKER_IP_DEFAULT = "192.168.199.129";

static atomic<bool> sub_running(true);

class Subscriber {
private:
    string subscriber_name;
    int    socket_fd;
    vector<string> subscriptions;  

    sockaddr_in broker_sub_addr;    // broker BROKER_SUB_PORT_QUIC

    // IP y puerto propios 
    string   local_ip;
    uint16_t local_port = 0;

    map<uint32_t, string> recv_buffer;
    set<uint32_t>         delivered;   // seq ya mostrados (anti-duplicado)
    uint32_t              next_seq;    // próximo seq esperado en orden
    mutex                 buf_mutex;

public:
    Subscriber(const string& name, const char* broker_ip)
        : subscriber_name(name), socket_fd(-1), next_seq(1)
    {
        memset(&broker_sub_addr, 0, sizeof(broker_sub_addr));
        broker_sub_addr.sin_family = AF_INET;
        broker_sub_addr.sin_port   = htons(BROKER_SUB_PORT_QUIC);
        inet_pton(AF_INET, broker_ip, &broker_sub_addr.sin_addr);
    }

    // connect_to_broker: crea socket UDP y lo enlaza a un puerto libre
    bool connect_to_broker() {
        socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (socket_fd < 0) {
            cerr << "[SUBSCRIBER " << subscriber_name << "] Error al crear socket" << endl;
            return false;
        }

        // Enlazar a puerto libre para que el broker sepa dónde responder
        sockaddr_in local_addr{};
        local_addr.sin_family      = AF_INET;
        local_addr.sin_addr.s_addr = INADDR_ANY;
        local_addr.sin_port        = 0;
        if (bind(socket_fd, (sockaddr*)&local_addr, sizeof(local_addr)) < 0) {
            cerr << "[SUBSCRIBER " << subscriber_name << "] Error al enlazar socket" << endl;
            return false;
        }

        // Descubrir IP y puerto reales asignados por el SO
        socklen_t len = sizeof(local_addr);
        getsockname(socket_fd, (sockaddr*)&local_addr, &len);
        local_port = ntohs(local_addr.sin_port);

        // Obtener IP propia mediante conexión UDP sin enviar datos
        {
            int tmp = ::socket(AF_INET, SOCK_DGRAM, 0);
            sockaddr_in tmp_addr = broker_sub_addr;
            ::connect(tmp, (sockaddr*)&tmp_addr, sizeof(tmp_addr));
            sockaddr_in me{};
            socklen_t me_len = sizeof(me);
            getsockname(tmp, (sockaddr*)&me, &me_len);
            char ip_buf[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &me.sin_addr, ip_buf, sizeof(ip_buf));
            local_ip = string(ip_buf);
            close(tmp);
        }

        // Timeout para que receive_messages pueda revisar sub_running
        struct timeval tv{ 0, 200000 };
        setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        char broker_ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &broker_sub_addr.sin_addr, broker_ip_str, sizeof(broker_ip_str));
        cout << "[SUBSCRIBER " << subscriber_name << "] Conectado al broker en "
             << broker_ip_str << ":" << BROKER_SUB_PORT_QUIC << endl;
        cout << "[SUBSCRIBER " << subscriber_name << "] Mi dirección: "
             << local_ip << ":" << local_port << endl;
        return true;
    }

    void subscribe_to_match(const string& match_name) {
        // Incluir "ip:port" en el payload para que el broker sepa dónde enviar DELIVER
        string self_addr = local_ip + ":" + to_string(local_port);

        QuicPacket pkt{};
        set_magic(pkt.header);
        pkt.header.type        = PacketType::SUBSCRIBE;
        pkt.header.seq_num     = 0;
        pkt.header.ack_num     = 0;
        pkt.header.payload_len = (uint16_t)min((int)self_addr.size(), MAX_PAYLOAD_LEN - 1);
        strncpy(pkt.header.topic, match_name.c_str(), MAX_TOPIC_LEN - 1);
        memcpy(pkt.payload, self_addr.c_str(), pkt.header.payload_len);

        char wire_buf[sizeof(QuicHeader) + MAX_PAYLOAD_LEN];
        int  wire_len = pkt.serialize(wire_buf, sizeof(wire_buf));

        // Timeout temporal para esperar confirmación de suscripción
        struct timeval tv{ 0, ACK_TIMEOUT_MS * 1000 };
        setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        bool confirmed = false;
        for (int attempt = 0; attempt <= MAX_RETRIES && !confirmed; attempt++) {
            if (attempt > 0)
                cout << "[SUBSCRIBER " << subscriber_name
                     << "] Reintentando SUBSCRIBE a " << match_name << endl;

            sendto(socket_fd, wire_buf, wire_len, 0,
                   (sockaddr*)&broker_sub_addr, sizeof(broker_sub_addr));

            char      ack_buf[sizeof(QuicHeader) + MAX_PAYLOAD_LEN];
            sockaddr_in from{};
            socklen_t   from_len = sizeof(from);
            int n = recvfrom(socket_fd, ack_buf, sizeof(ack_buf), 0,
                             (sockaddr*)&from, &from_len);
            if (n < 0) continue;

            QuicPacket ack;
            if (!ack.deserialize(ack_buf, n)) continue;
            if (ack.header.type == PacketType::ACK) {
                confirmed = true;
                subscriptions.push_back(match_name);
                cout << "[SUBSCRIBER " << subscriber_name
                     << "] Suscrito a: " << match_name << endl;
            }
        }
        if (!confirmed)
            cerr << "[SUBSCRIBER " << subscriber_name
                 << "] No se pudo confirmar suscripción a " << match_name << endl;

        // Restaurar timeout largo para receive_messages
        struct timeval tv2{ 0, 200000 };
        setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &tv2, sizeof(tv2));
    }

    void receive_messages() {
        cout << "[SUBSCRIBER " << subscriber_name << "] Esperando actualizaciones..." << endl;

        char buf[sizeof(QuicHeader) + MAX_PAYLOAD_LEN + 16];

        while (sub_running) {
            sockaddr_in from{};
            socklen_t   from_len = sizeof(from);

            int n = recvfrom(socket_fd, buf, sizeof(buf), 0,
                             (sockaddr*)&from, &from_len);
            if (n < 0) continue;    // timeout, volver a intentar

            QuicPacket pkt;
            if (!pkt.deserialize(buf, n)) continue;
            if (pkt.header.type != PacketType::DELIVER) continue;

            uint32_t seq   = pkt.header.seq_num;
            string   topic(pkt.header.topic);
            string   msg(pkt.payload, pkt.header.payload_len);

            // SUB_ACK inmediato
            send_sub_ack(seq, topic, from);

            lock_guard<mutex> lock(buf_mutex);

            // Descartar duplicados
            if (delivered.count(seq)) continue;

            // Almacenar en buffer de reordenamiento 
            recv_buffer[seq] = "[" + topic + "] " + msg;

            // Vaciar buffer en orden
            drain_buffer();
        }
    }

    void start_listening() {
        thread(&Subscriber::receive_messages, this).detach();
    }

    void close_connection() {
        if (socket_fd >= 0) {
            close(socket_fd);
            cout << "[SUBSCRIBER " << subscriber_name << "] Conexión cerrada" << endl;
            socket_fd = -1;
        }
    }

    ~Subscriber() {
        close_connection();
    }

private:
    // Enviar SUB_ACK al broker 
    void send_sub_ack(uint32_t acked_seq, const string& topic,
                      const sockaddr_in& broker_addr) {
        QuicPacket ack{};
        set_magic(ack.header);
        ack.header.type        = PacketType::SUB_ACK;
        ack.header.seq_num     = 0;
        ack.header.ack_num     = acked_seq;
        ack.header.payload_len = 0;
        strncpy(ack.header.topic, topic.c_str(), MAX_TOPIC_LEN - 1);

        char buf[sizeof(QuicHeader)];
        ack.serialize(buf, sizeof(buf));
        sendto(socket_fd, buf, sizeof(QuicHeader), 0,
               (sockaddr*)&broker_addr, sizeof(broker_addr));
    }

    // Vaciar recv_buffer entregando mensajes en orden ascendente de seq_num
        while (true) {
            auto it = recv_buffer.find(next_seq);
            if (it == recv_buffer.end()) break;

            // Mismo formato de log que subscriber.cpp
            cout << "[SUBSCRIBER " << subscriber_name
                 << "] ACTUALIZACIÓN: " << it->second << endl;

            delivered.insert(next_seq);
            recv_buffer.erase(it);
            next_seq++;
        }
    }
};

// Manejador de señales 
void signal_handler(int) {
    sub_running = false;
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        cerr << "Uso: " << argv[0]
             << " <nombre_suscriptor> <partido1> [<partido2> ...] [ip_broker]" << endl;
        cerr << "Ejemplo: " << argv[0]
             << " Fan_Juan Real_Madrid_vs_Barcelona Liverpool_vs_ManCity" << endl;
        return 1;
    }

    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);

    string subscriber_name = argv[1];

    // Último argumento: si contiene '.' se asume que es una IP
    const char* broker_ip = BROKER_IP_DEFAULT;
    int last_match_idx = argc - 1;
    if (string(argv[argc-1]).find('.') != string::npos) {
        broker_ip = argv[argc-1];
        last_match_idx = argc - 2;
    }

    Subscriber subscriber(subscriber_name, broker_ip);

    if (!subscriber.connect_to_broker()) {
        cerr << "Fallo al conectar con broker" << endl;
        return 1;
    }

    // Suscribirse a todos los partidos proporcionados 
    for (int i = 2; i <= last_match_idx; i++) {
        subscriber.subscribe_to_match(argv[i]);
    }

    // Iniciar escucha en hilo separado 
    subscriber.start_listening();

    cout << "[SUBSCRIBER " << subscriber_name << "] Presiona Ctrl+C para salir" << endl;
    while (sub_running) {
        sleep(1);
    }

    subscriber.close_connection();
    return 0;
}
