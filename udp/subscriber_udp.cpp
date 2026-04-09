#include <iostream>
#include <cstring>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <thread>
#include <vector>
#include <atomic>
#include <csignal>

using namespace std;

const int BROKER_PORT = 5000;
const char* BROKER_IP = "127.0.0.1";
const int BUFFER_SIZE = 1024;


atomic<bool> running(true);

class Subscriber {
private:
    int socket_fd;
    string subscriber_name;
    vector<string> subscriptions;
    struct sockaddr_in broker_addr;

public:
    Subscriber(const string& name) : subscriber_name(name), socket_fd(-1) {}

    bool connect_to_broker() {
        // Crear socket
        socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (socket_fd < 0) {
            cerr << "[SUBSCRIBER " << subscriber_name << "] Error al crear socket" << endl;
            return false;
        }

        // puerto x para recibir
        struct sockaddr_in local_addr;
        memset(&local_addr, 0, sizeof(local_addr));
        local_addr.sin_family = AF_INET;
        local_addr.sin_addr.s_addr = INADDR_ANY;
        local_addr.sin_port = htons(0); // Puerto asignado por el SO

        if (bind(socket_fd, (struct sockaddr*)&local_addr, sizeof(local_addr)) < 0) {
            cerr << "[SUBSCRIBER " << subscriber_name << "] Error al vincular socket" << endl;
            return false;
        }

        // timeout para que recvfrom() no bloquee
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        // direccion broker
        memset(&broker_addr, 0, sizeof(broker_addr));
        broker_addr.sin_family = AF_INET;
        broker_addr.sin_port = htons(BROKER_PORT);

        if (inet_pton(AF_INET, BROKER_IP, &broker_addr.sin_addr) <= 0) {
            cerr << "[SUBSCRIBER " << subscriber_name << "] IP del broker inválida" << endl;
            return false;
        }

        // evio directo
        cout << "[SUBSCRIBER " << subscriber_name << "] Listo para comunicarse con broker en " << BROKER_IP << ":" << BROKER_PORT << " (UDP)" << endl;
        return true;
    }

    void subscribe_to_match(const string& match_name) {
        string message = "SUBSCRIBE:" + match_name;

        //  suscripción al broker - sendto()
        ssize_t sent = sendto(socket_fd, message.c_str(), message.length(), 0,
                              (struct sockaddr*)&broker_addr, sizeof(broker_addr));
        if (sent < 0) {
            cerr << "[SUBSCRIBER " << subscriber_name << "] Error al enviar suscripción" << endl;
            return;
        }

        subscriptions.push_back(match_name);

        // Confirmacion opcional
        char buffer[BUFFER_SIZE];
        memset(buffer, 0, BUFFER_SIZE);
        struct sockaddr_in from_addr;
        socklen_t from_len = sizeof(from_addr);

        ssize_t bytes_read = recvfrom(socket_fd, buffer, BUFFER_SIZE - 1, 0,
                                      (struct sockaddr*)&from_addr, &from_len);
        if (bytes_read > 0) {
            buffer[bytes_read] = '\0';
            cout << "[SUBSCRIBER " << subscriber_name << "] " << buffer;
        } else {
            // Sin problema sin confrimacion cubierto con else
            cout << "[SUBSCRIBER " << subscriber_name << "] Suscrito a: " << match_name << " (sin confirmación)" << endl;
        }
    }

    void receive_messages() {
        cout << "[SUBSCRIBER " << subscriber_name << "] Esperando actualizaciones..." << endl;

        char buffer[BUFFER_SIZE];

        while (running) {
            memset(buffer, 0, BUFFER_SIZE);
            struct sockaddr_in from_addr;
            socklen_t from_len = sizeof(from_addr);

            // recepcion de info usando recvfrom()
            ssize_t bytes_read = recvfrom(socket_fd, buffer, BUFFER_SIZE - 1, 0,
                                          (struct sockaddr*)&from_addr, &from_len);

            if (bytes_read <= 0) {
                // timeout sin problema
                continue;
            }

            buffer[bytes_read] = '\0';
            string message(buffer);
            message.erase(message.find_last_not_of("\n\r") + 1);

            cout << "[SUBSCRIBER " << subscriber_name << "] ACTUALIZACIÓN: " << message << endl;
        }
    }

    void start_listening() {
        // iniciar recepcion
        thread(&Subscriber::receive_messages, this).detach();
    }

    void close_connection() {
        if (socket_fd >= 0) {
            close(socket_fd);
            cout << "[SUBSCRIBER " << subscriber_name << "] Conexión cerrada" << endl;
        }
    }

    ~Subscriber() {
        close_connection();
    }
};

// manejador de señales para cierre
void signal_handler(int signal) {
    cout << "\n[SUBSCRIBER] Señal recibida (" << signal << "), cerrando..." << endl;
    running = false;
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        cerr << "Uso: " << argv[0] << " <nombre_suscriptor> <partido1> [<partido2> ...] [broker_ip]" << endl;
        cerr << "Ejemplo: " << argv[0] << " Fan_Juan Real_Madrid_vs_Barcelona Liverpool_vs_ManCity" << endl;
        cerr << "Ejemplo: " << argv[0] << " Fan_Juan Real_Madrid_vs_Barcelona 192.168.77.148" << endl;
        return 1;
    }

    // manejadores de señales
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    string subscriber_name = argv[1];

    // si el ultimo argumento es una ip para usarla como broker
    // si no, todos los argumentos despues del nombre son temas
    int topic_end = argc;
    string last_arg = argv[argc - 1];
    if (last_arg.find('.') != string::npos && last_arg.find('_') == string::npos) {
        // Parece ser una IP, no un nombre de partido
        BROKER_IP = argv[argc - 1];
        topic_end = argc - 1;
    }

    if (topic_end < 3) {
        cerr << "Debe proporcionar al menos un partido para suscribirse" << endl;
        return 1;
    }

    Subscriber subscriber(subscriber_name);

    if (!subscriber.connect_to_broker()) {
        cerr << "Fallo al conectar con broker" << endl;
        return 1;
    }

    // suscribirse a todo
    for (int i = 2; i < topic_end; i++) {
        subscriber.subscribe_to_match(argv[i]);
    }

    // Iniciar repeccion
    subscriber.start_listening();

    // mantener ejecucion
    cout << "[SUBSCRIBER " << subscriber_name << "] Presiona Ctrl+C para salir" << endl;
    while (running) {
        sleep(1);
    }

    subscriber.close_connection();
    return 0;
}
