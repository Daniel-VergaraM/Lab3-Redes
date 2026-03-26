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

const int BROKER_PORT = 5000;
const int BUFFER_SIZE = 1024;
const int MAX_CLIENTS = 100;

// Global variable for signal handling
atomic<bool> server_running(true);

struct Client {
    int socket_fd;
    int client_id;
    vector<string> subscriptions;
    bool is_active;
};

class Broker {
private:
    int server_socket;
    vector<Client> clients;
    map<string, vector<int>> topic_subscribers;
    mutex clients_mutex;
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
        
        lock_guard<mutex> lock(clients_mutex);
        for (auto& client : clients) {
            if (client.is_active && client.socket_fd >= 0) {
                close(client.socket_fd);
            }
        }
    }

    bool initialize() {
        server_socket = socket(AF_INET, SOCK_STREAM, 0);
        if (server_socket < 0) {
            cerr << "[BROKER] Error al crear socket" << endl;
            return false;
        }

        int opt = 1;
        if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
            cerr << "[BROKER] Error al configurar opciones de socket" << endl;
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

        if (listen(server_socket, MAX_CLIENTS) < 0) {
            cerr << "[BROKER] Error al escuchar en socket" << endl;
            return false;
        }

        cout << "[BROKER] Escuchando en puerto " << BROKER_PORT << endl;
        return true;
    }

    void run() {
        cout << "[BROKER] Iniciando servidor..." << endl;

        while (server_running) {
            struct sockaddr_in client_addr;
            socklen_t client_addr_len = sizeof(client_addr);

            int client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &client_addr_len);
            if (client_socket < 0) {
                if (server_running) {
                    cerr << "[BROKER] Error al aceptar conexión" << endl;
                }
                continue;
            }

            // Use inet_ntop instead of deprecated inet_ntoa
            char client_ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &(client_addr.sin_addr), client_ip, INET_ADDRSTRLEN);
            cout << "[BROKER] Nuevo cliente conectado desde " << client_ip << endl;

            thread(&Broker::handle_client, this, client_socket).detach();
        }
    }

    void publish_message(const string& topic, const string& message, int sender_socket) {
        lock_guard<mutex> lock(topics_mutex);

        if (topic_subscribers.find(topic) == topic_subscribers.end()) {
            return;
        }

        string formatted_msg = "[" + topic + "] " + message + "\n";
        
        for (int subscriber_socket : topic_subscribers[topic]) {
            if (subscriber_socket != sender_socket) {
                ssize_t sent = send(subscriber_socket, formatted_msg.c_str(), formatted_msg.length(), MSG_NOSIGNAL);
                if (sent < 0) {
                    // Socket error, will be handled by the client's thread
                    cerr << "[BROKER] Error enviando mensaje al socket " << subscriber_socket << endl;
                }
            }
        }

        cout << "[BROKER] Publicado en tema '" << topic << "': " << message << endl;
    }

private:
    void handle_client(int client_socket) {
        char buffer[BUFFER_SIZE];
        int client_id;
        int stored_socket_fd;

        {
            lock_guard<mutex> lock(clients_mutex);
            client_id = clients.size();
            Client new_client;
            new_client.socket_fd = client_socket;
            new_client.client_id = client_id;
            new_client.is_active = true;
            clients.push_back(new_client);
            stored_socket_fd = client_socket;
        }

        cout << "[BROKER] Cliente " << client_id << " conectado" << endl;

        while (server_running) {
            memset(buffer, 0, BUFFER_SIZE);
            ssize_t bytes_read = recv(client_socket, buffer, BUFFER_SIZE - 1, 0);

            if (bytes_read <= 0) {
                if (bytes_read < 0) {
                    cerr << "[BROKER] Error de lectura del cliente " << client_id << endl;
                }
                cout << "[BROKER] Cliente " << client_id << " desconectado" << endl;
                close(client_socket);
                unregister_client(client_id, stored_socket_fd);
                break;
            }

            string message(buffer);
            // Remove trailing whitespace
            size_t end = message.find_last_not_of(" \n\r\t");
            if (end != string::npos) {
                message = message.substr(0, end + 1);
            }

            if (message.find("PUBLISH:") == 0) {
                size_t first_colon = message.find(':', 8);
                if (first_colon != string::npos) {
                    string topic = message.substr(8, first_colon - 8);
                    string msg_content = message.substr(first_colon + 1);
                    publish_message(topic, msg_content, client_socket);
                }
            } else if (message.find("SUBSCRIBE:") == 0) {
                string topic = message.substr(10);
                subscribe_client(client_id, client_socket, topic);
            } else {
                cout << "[BROKER] Mensaje desconocido del cliente " << client_id << ": " << message << endl;
            }
        }
    }

    void subscribe_client(int client_id, int client_socket, const string& topic) {
        lock_guard<mutex> lock(topics_mutex);

        topic_subscribers[topic].push_back(client_socket);
        cout << "[BROKER] Cliente " << client_id << " suscrito al tema '" << topic << "'" << endl;

        string confirmation = "Suscrito a " + topic + "\n";
        ssize_t sent = send(client_socket, confirmation.c_str(), confirmation.length(), MSG_NOSIGNAL);
        if (sent < 0) {
            cerr << "[BROKER] Error enviando confirmación al cliente " << client_id << endl;
        }
    }

    void unregister_client(int client_id, int socket_fd) {
        // First remove from topic subscribers
        {
            lock_guard<mutex> lock(topics_mutex);
            for (auto& pair : topic_subscribers) {
                auto& sockets = pair.second;
                sockets.erase(remove(sockets.begin(), sockets.end(), socket_fd), sockets.end());
            }
        }
        
        // Then mark client as inactive
        {
            lock_guard<mutex> clients_lock(clients_mutex);
            if (client_id < (int)clients.size()) {
                clients[client_id].is_active = false;
            }
        }
    }
};

// Signal handler for graceful shutdown
void signal_handler(int signal) {
    cout << "\n[BROKER] Señal recibida (" << signal << "), cerrando servidor..." << endl;
    server_running = false;
}

int main() {
    // Set up signal handlers
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
