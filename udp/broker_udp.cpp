#include <iostream>
#include <cstring>
#include <vector>
#include <map>
#include <algorithm>
#include <thread>
#include <mutex>
#include <atomic>
#include <csignal>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

using namespace std;

//Puerto - buffer
const int BROKER_PORT = 5000;
const int BUFFER_SIZE = 1024;


atomic<bool> server_running(true);

// Direcciones de suscriptores
struct SubscriberAddr {
    struct sockaddr_in addr;
    string addr_str;
};

//Implementacion broker
class Broker {
private:
    int server_socket;
    map<string, vector<SubscriberAddr>> topic_subscribers;
    mutex topics_mutex;

public:
    Broker() {
        server_socket = -1;
    }

    ~Broker() {
        cleanup();
    }

    void cleanup() {
        if (server_socket >= 0) {
            close(server_socket);
            server_socket = -1;
        }
    }

    bool initialize() {
        // Crear socket
        server_socket = socket(AF_INET, SOCK_DGRAM, 0);
        if (server_socket < 0) {
            cerr << "[BROKER] Error al crear socket" << endl;
            return false;
        }

        int opt = 1;
        if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
            cerr << "[BROKER] Error al configurar opciones de socket" << endl;
            return false;
        }

        // Timeout para que recvfrom() no bloquee, ya que no hay confirmacion.
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 200000; // 200ms
        if (setsockopt(server_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
            cerr << "[BROKER] Error al configurar timeout de socket" << endl;
            return false;
        }

        struct sockaddr_in server_addr;
        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_addr.s_addr = INADDR_ANY;
        server_addr.sin_port = htons(BROKER_PORT);

        if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
            cerr << "[BROKER] Error al vincular socket al puerto " << BROKER_PORT << endl;
            return false;
        }

        // NO listen() ni accpet()
        cout << "[BROKER] Escuchando en puerto " << BROKER_PORT << " (UDP)" << endl;
        return true;
    }

    void run() {
        cout << "[BROKER] Iniciando servidor..." << endl;

        char buffer[BUFFER_SIZE];
        struct sockaddr_in sender_addr;
        socklen_t sender_addr_len = sizeof(sender_addr);

        while (server_running) {
            memset(buffer, 0, BUFFER_SIZE);
            sender_addr_len = sizeof(sender_addr);

            // Recibir todos los datagramas
            ssize_t bytes_read = recvfrom(server_socket, buffer, BUFFER_SIZE - 1, 0,
                                          (struct sockaddr*)&sender_addr, &sender_addr_len);

            if (bytes_read < 0) {
                // No bloqueo si hay timeout o error
                continue;
            }

            buffer[bytes_read] = '\0';

            // Identificar origen
            char sender_ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &(sender_addr.sin_addr), sender_ip, INET_ADDRSTRLEN);
            int sender_port = ntohs(sender_addr.sin_port);

            string message(buffer);
            // elimina espacios en blanco
            size_t end = message.find_last_not_of(" \n\r\t");
            if (end != string::npos) {
                message = message.substr(0, end + 1);
            }

            // Formato mensaje (TCP)
            if (message.find("PUBLISH:") == 0) {
                size_t first_colon = message.find(':', 8);
                if (first_colon != string::npos) {
                    string topic = message.substr(8, first_colon - 8);
                    string msg_content = message.substr(first_colon + 1);
                    publish_message(topic, msg_content);
                }
            } else if (message.find("SUBSCRIBE:") == 0) {
                string topic = message.substr(10);
                subscribe_client(sender_addr, topic);
            } else {
                cout << "[BROKER] Mensaje desconocido de " << sender_ip << ":" << sender_port
                     << ": " << message << endl;
            }
        }
    }

    void publish_message(const string& topic, const string& message) {
        lock_guard<mutex> lock(topics_mutex);

        if (topic_subscribers.find(topic) == topic_subscribers.end()) {
            cout << "[BROKER] No hay suscriptores para el tema '" << topic << "'" << endl;
            return;
        }

        string formatted_msg = "[" + topic + "] " + message + "\n";

        for (const auto& subscriber : topic_subscribers[topic]) {
            ssize_t sent = sendto(server_socket, formatted_msg.c_str(), formatted_msg.length(), 0,
                                  (struct sockaddr*)&subscriber.addr, sizeof(subscriber.addr));
            if (sent < 0) {
                cerr << "[BROKER] Error enviando mensaje al suscriptor " << subscriber.addr_str << endl;
            }
        }

        cout << "[BROKER] Publicado en tema '" << topic << "': " << message << endl;
    }

private:
    void subscribe_client(const struct sockaddr_in& client_addr, const string& topic) {
        lock_guard<mutex> lock(topics_mutex);

        //identificador único
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(client_addr.sin_addr), client_ip, INET_ADDRSTRLEN);
        int client_port = ntohs(client_addr.sin_port);
        string addr_str = string(client_ip) + ":" + to_string(client_port);

        // Revisar dumplicacion de suscripcion
        for (const auto& sub : topic_subscribers[topic]) {
            if (sub.addr_str == addr_str) {
                cout << "[BROKER] Suscriptor " << addr_str << " ya está suscrito a '" << topic << "'" << endl;
                return;
            }
        }

        SubscriberAddr new_sub;
        new_sub.addr = client_addr;
        new_sub.addr_str = addr_str;
        topic_subscribers[topic].push_back(new_sub);

        cout << "[BROKER] Suscriptor " << addr_str << " suscrito al tema '" << topic << "'" << endl;

        // confirmación al suscriptor
        string confirmation = "Suscrito a " + topic + "\n";
        sendto(server_socket, confirmation.c_str(), confirmation.length(), 0,
               (struct sockaddr*)&client_addr, sizeof(client_addr));
    }
};

// manejador de señales para cierre
void signal_handler(int signal) {
    cout << "\n[BROKER] Señal recibida (" << signal << "), cerrando servidor..." << endl;
    server_running = false;
}

int main() {
    //manejadores de señales
    signal(SIGINT, signal_handler);
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
