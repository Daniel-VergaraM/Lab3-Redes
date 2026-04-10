#include "quic_protocol.h"

#include <iostream>
#include <cstring>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <thread>
#include <mutex>
#include <atomic>
#include <csignal>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

using namespace std;

// ---- Variable global para señales (igual que broker.cpp) ----
atomic<bool> server_running(true);

// ---- Información de un suscriptor ----
struct SubscriberInfo {
    sockaddr_in addr;
    string      addr_str;   // "ip:puerto" para logs
};

// ---- Entrega pendiente de SUB_ACK ----
struct PendingDelivery {
    QuicPacket     packet;       // copia del DELIVER enviado
    SubscriberInfo subscriber;
    int64_t        sent_at_ms;  // momento del último envío
    int            retries;     // intentos realizados hasta ahora
    bool           acked;       // true cuando llega SUB_ACK
};

// =============================================================================
class Broker {
private:
    int pub_socket;   // socket para publishers 
    int sub_socket;   // socket para subscribers 

    // topic → lista de suscriptores
    map<string, vector<SubscriberInfo>> topic_subscribers;

    // entregas pendientes: clave = "seq_num|ip:puerto"
    map<string, PendingDelivery> pending_deliveries;

    mutex clients_mutex;    // protege topic_subscribers
    mutex pending_mutex;    // protege pending_deliveries

public:
    Broker() : pub_socket(-1), sub_socket(-1) {}

    ~Broker() {
        cleanup();
    }

    bool initialize() {
        // Socket para publishers
        pub_socket = socket(AF_INET, SOCK_DGRAM, 0);
        if (pub_socket < 0) {
            cerr << "[BROKER] Error al crear socket de publishers" << endl;
            return false;
        }

        int opt = 1;
        setsockopt(pub_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in pub_addr{};
        pub_addr.sin_family      = AF_INET;
        pub_addr.sin_addr.s_addr = INADDR_ANY;
        pub_addr.sin_port        = htons(BROKER_PORT_QUIC);

        if (bind(pub_socket, (sockaddr*)&pub_addr, sizeof(pub_addr)) < 0) {
            cerr << "[BROKER] Error al vincular socket de publishers al puerto "
                 << BROKER_PORT_QUIC << endl;
            return false;
        }

        // Socket para suscriptores
        sub_socket = socket(AF_INET, SOCK_DGRAM, 0);
        if (sub_socket < 0) {
            cerr << "[BROKER] Error al crear socket de suscriptores" << endl;
            return false;
        }
        setsockopt(sub_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in sub_addr{};
        sub_addr.sin_family      = AF_INET;
        sub_addr.sin_addr.s_addr = INADDR_ANY;
        sub_addr.sin_port        = htons(BROKER_SUB_PORT_QUIC);

        if (bind(sub_socket, (sockaddr*)&sub_addr, sizeof(sub_addr)) < 0) {
            cerr << "[BROKER] Error al vincular socket de suscriptores al puerto "
                 << BROKER_SUB_PORT_QUIC << endl;
            return false;
        }

        // Timeout de recepción para que los hilos puedan revisar server_running
        struct timeval tv{ 0, 200000 };  // 200 ms
        setsockopt(pub_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sub_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        cout << "[BROKER] Escuchando publishers en puerto  " << BROKER_PORT_QUIC     << endl;
        cout << "[BROKER] Escuchando suscriptores en puerto " << BROKER_SUB_PORT_QUIC << endl;
        return true;
    }

    void run() {
        cout << "[BROKER] Iniciando servidor QUIC-like..." << endl;

        // Hilo para suscriptores (SUBSCRIBE y SUB_ACK)
        thread(&Broker::sub_socket_loop, this).detach();

        // Hilo de retransmisión
        thread(&Broker::retransmit_loop, this).detach();

        // Hilo principal: publishers (PUBLISH)
        pub_socket_loop();
    }

    void cleanup() {
        if (pub_socket >= 0) { close(pub_socket); pub_socket = -1; }
        if (sub_socket >= 0) { close(sub_socket); sub_socket = -1; }
    }

private:
    // Helpers
    string addr_to_str(const sockaddr_in& addr) {
        char buf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf));
        return string(buf) + ":" + to_string(ntohs(addr.sin_port));
    }

    string pending_key(uint32_t seq, const string& addr_str) {
        return to_string(seq) + "|" + addr_str;
    }

    bool send_packet(int sock, const QuicPacket& pkt, const sockaddr_in& addr) {
        char buf[sizeof(QuicHeader) + MAX_PAYLOAD_LEN];
        int len = pkt.serialize(buf, sizeof(buf));
        if (len < 0) return false;
        return sendto(sock, buf, len, 0, (sockaddr*)&addr, sizeof(addr)) > 0;
    }

    // Hilo retransmisión — equivalente al control de flujo de TCP
    void retransmit_loop() {
        while (server_running) {
            this_thread::sleep_for(chrono::milliseconds(100));

            lock_guard<mutex> lock(pending_mutex);
            int64_t now = now_ms();

            for (auto& [key, pd] : pending_deliveries) {
                if (pd.acked) continue;
                if (now - pd.sent_at_ms < ACK_TIMEOUT_MS) continue;

                if (pd.retries >= MAX_RETRIES) {
                    cerr << "[BROKER] ⚠ Sin SUB_ACK para seq="
                         << pd.packet.header.seq_num
                         << " destino=" << pd.subscriber.addr_str
                         << " — se descarta" << endl;
                    pd.acked = true;
                    continue;
                }

                pd.retries++;
                pd.sent_at_ms = now;
                send_packet(sub_socket, pd.packet, pd.subscriber.addr);
                cout << "[BROKER] ↩ Retransmitiendo DELIVER seq="
                     << pd.packet.header.seq_num
                     << " → " << pd.subscriber.addr_str
                     << " (intento " << pd.retries << "/" << MAX_RETRIES << ")" << endl;
            }

            // Limpiar entradas ya confirmadas
            for (auto it = pending_deliveries.begin(); it != pending_deliveries.end(); ) {
                it = it->second.acked ? pending_deliveries.erase(it) : next(it);
            }
        }
    }

    void sub_socket_loop() {
        char buf[sizeof(QuicHeader) + MAX_PAYLOAD_LEN + 16];

        while (server_running) {
            sockaddr_in src{};
            socklen_t   src_len = sizeof(src);

            int n = recvfrom(sub_socket, buf, sizeof(buf), 0,
                             (sockaddr*)&src, &src_len);
            if (n < 0) continue;  // timeout, revisar server_running

            QuicPacket pkt;
            if (!pkt.deserialize(buf, n)) continue;

            string src_str = addr_to_str(src);

            if (pkt.header.type == PacketType::SUBSCRIBE) {
                string topic(pkt.header.topic);

                // El subscriber envía "ip:port" en el payload para que el broker
                // sepa exactamente dónde hacer DELIVER (evita problemas con NAT/VMware)
                sockaddr_in deliver_addr = src;
                string deliver_str = src_str;
                if (pkt.header.payload_len > 0) {
                    string addr_payload(pkt.payload, pkt.header.payload_len);
                    size_t colon = addr_payload.rfind(':');
                    if (colon != string::npos) {
                        string ip_part   = addr_payload.substr(0, colon);
                        int    port_part = stoi(addr_payload.substr(colon + 1));
                        sockaddr_in explicit_addr{};
                        explicit_addr.sin_family = AF_INET;
                        explicit_addr.sin_port   = htons(port_part);
                        if (inet_pton(AF_INET, ip_part.c_str(), &explicit_addr.sin_addr) == 1) {
                            deliver_addr = explicit_addr;
                            deliver_str  = addr_payload;
                        }
                    }
                }

                SubscriberInfo si{ deliver_addr, deliver_str };

                {
                    lock_guard<mutex> lock(clients_mutex);
                    auto& subs = topic_subscribers[topic];
                    bool  found = false;
                    for (auto& s : subs)
                        if (s.addr_str == deliver_str) { found = true; break; }
                    if (!found) subs.push_back(si);
                }

                cout << "[BROKER] Cliente " << deliver_str
                     << " suscrito al tema '" << topic << "'" << endl;

                // Confirmar suscripción 
                QuicPacket ack{};
                set_magic(ack.header);
                ack.header.type        = PacketType::ACK;
                ack.header.ack_num     = pkt.header.seq_num;
                ack.header.payload_len = 0;
                strncpy(ack.header.topic, topic.c_str(), MAX_TOPIC_LEN - 1);
                send_packet(sub_socket, ack, src);

            } else if (pkt.header.type == PacketType::SUB_ACK) {
                // El suscriptor confirmó recepción de un DELIVER 
                string key = pending_key(pkt.header.ack_num, src_str);
                lock_guard<mutex> lock(pending_mutex);
                auto it = pending_deliveries.find(key);
                if (it != pending_deliveries.end()) {
                    it->second.acked = true;
                    cout << "[BROKER] ✔ SUB_ACK de " << src_str
                         << " seq=" << pkt.header.ack_num << endl;
                }
            }
        }
    }


    void pub_socket_loop() {
        char buf[sizeof(QuicHeader) + MAX_PAYLOAD_LEN + 16];

        while (server_running) {
            sockaddr_in src{};
            socklen_t   src_len = sizeof(src);

            int n = recvfrom(pub_socket, buf, sizeof(buf), 0,
                             (sockaddr*)&src, &src_len);
            if (n < 0) continue;

            QuicPacket pkt;
            if (!pkt.deserialize(buf, n)) continue;
            if (pkt.header.type != PacketType::PUBLISH) continue;

            string topic(pkt.header.topic);
            uint32_t seq = pkt.header.seq_num;
            string message(pkt.payload, pkt.header.payload_len);

            cout << "[BROKER] PUBLISH seq=" << seq
                 << " tema='" << topic << "' msg='" << message << "'" << endl;

            // Reenviar a suscriptores
            vector<SubscriberInfo> targets;
            {
                lock_guard<mutex> lock(clients_mutex);
                auto it = topic_subscribers.find(topic);
                if (it != topic_subscribers.end())
                    targets = it->second;
            }

            {
                lock_guard<mutex> lock(pending_mutex);
                for (auto& sub : targets) {
                    QuicPacket deliver{};
                    set_magic(deliver.header);
                    deliver.header.type        = PacketType::DELIVER;
                    deliver.header.seq_num     = seq;
                    deliver.header.payload_len = pkt.header.payload_len;
                    strncpy(deliver.header.topic, topic.c_str(), MAX_TOPIC_LEN - 1);
                    memcpy(deliver.payload, pkt.payload, pkt.header.payload_len);

                    send_packet(sub_socket, deliver, sub.addr);
                    cout << "[BROKER] → DELIVER seq=" << seq
                         << " a " << sub.addr_str << endl;

                    // Registrar como entrega pendiente de SUB_ACK
                    PendingDelivery pd{};
                    pd.packet     = deliver;
                    pd.subscriber = sub;
                    pd.sent_at_ms = now_ms();
                    pd.retries    = 0;
                    pd.acked      = false;
                    pending_deliveries[pending_key(seq, sub.addr_str)] = pd;
                }
            }

            if (targets.empty()) {
                cout << "[BROKER] No hay suscriptores para '" << topic << "'" << endl;
            }

            // ---- ACK al publisher ----
            QuicPacket ack{};
            set_magic(ack.header);
            ack.header.type        = PacketType::ACK;
            ack.header.ack_num     = seq;
            ack.header.payload_len = 0;
            strncpy(ack.header.topic, topic.c_str(), MAX_TOPIC_LEN - 1);
            send_packet(pub_socket, ack, src);
            cout << "[BROKER] ✔ ACK seq=" << seq
                 << " → publisher " << addr_to_str(src) << endl;
        }
    }
};


void signal_handler(int signal) {
    cout << "\n[BROKER] Señal recibida (" << signal << "), cerrando servidor..." << endl;
    server_running = false;
}

int main() {
    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);

    Broker broker;

    if (!broker.initialize()) {
        cerr << "Fallo al inicializar broker" << endl;
        return 1;
    }

    broker.run();

    cout << "[BROKER] Servidor detenido" << endl;
    return 0;
}