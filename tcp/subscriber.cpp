#include <iostream>
#include <cstring>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <thread>
#include <vector>

using namespace std;

const int BROKER_PORT = 5000;
const char* BROKER_IP = "192.168.77.148";
const int BUFFER_SIZE = 1024;

class Subscriber {
private:
    int socket_fd;
    string subscriber_name;
    vector<string> subscriptions;

public:
    Subscriber(const string& name) : subscriber_name(name), socket_fd(-1) {}

    bool connect_to_broker() {
        socket_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (socket_fd < 0) {
            cerr << "[SUBSCRIBER " << subscriber_name << "] Error al crear socket" << endl;
            return false;
        }

        struct sockaddr_in broker_addr;
        memset(&broker_addr, 0, sizeof(broker_addr));
        broker_addr.sin_family = AF_INET;
        broker_addr.sin_port = htons(BROKER_PORT);

        if (inet_pton(AF_INET, BROKER_IP, &broker_addr.sin_addr) <= 0) {
            cerr << "[SUBSCRIBER " << subscriber_name << "] IP del broker inválida" << endl;
            return false;
        }

        if (connect(socket_fd, (struct sockaddr*)&broker_addr, sizeof(broker_addr)) < 0) {
            cerr << "[SUBSCRIBER " << subscriber_name << "] Error al conectar con broker" << endl;
            return false;
        }

        cout << "[SUBSCRIBER " << subscriber_name << "] Conectado al broker en " << BROKER_IP << ":" << BROKER_PORT << endl;
        return true;
    }

    void subscribe_to_match(const string& match_name) {
        string message = "SUBSCRIBE:" + match_name;

        if (send(socket_fd, message.c_str(), message.length(), 0) < 0) {
            cerr << "[SUBSCRIBER " << subscriber_name << "] Error al enviar suscripción" << endl;
            return;
        }

        subscriptions.push_back(match_name);
        cout << "[SUBSCRIBER " << subscriber_name << "] Suscrito a: " << match_name << endl;
    }

    void receive_messages() {
        cout << "[SUBSCRIBER " << subscriber_name << "] Esperando actualizaciones..." << endl;

        char buffer[BUFFER_SIZE];

        while (true) {
            memset(buffer, 0, BUFFER_SIZE);
            int bytes_read = recv(socket_fd, buffer, BUFFER_SIZE - 1, 0);

            if (bytes_read <= 0) {
                cout << "[SUBSCRIBER " << subscriber_name << "] Desconectado del broker" << endl;
                break;
            }

            string message(buffer);
            message.erase(message.find_last_not_of("\n\r") + 1);

            cout << "[SUBSCRIBER " << subscriber_name << "] ACTUALIZACIÓN: " << message << endl;
        }
    }

    void start_listening() {
        // Iniciar recepción de mensajes en un hilo separado
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

int main(int argc, char* argv[]) {
    if (argc < 2) {
        cerr << "Uso: " << argv[0] << " <nombre_suscriptor> <partido1> [<partido2> ...]" << endl;
        cerr << "Ejemplo: " << argv[0] << " Fan_Juan Real_Madrid_vs_Barcelona Liverpool_vs_ManCity" << endl;
        return 1;
    }

    string subscriber_name = argv[1];
    Subscriber subscriber(subscriber_name);

    if (!subscriber.connect_to_broker()) {
        cerr << "Fallo al conectar con broker" << endl;
        return 1;
    }

    // Suscribirse a todos los partidos proporcionados
    for (int i = 2; i < argc; i++) {
        subscriber.subscribe_to_match(argv[i]);
    }

    // Iniciar escucha de mensajes
    subscriber.start_listening();

    // Mantener el suscriptor ejecutándose
    cout << "[SUBSCRIBER " << subscriber_name << "] Presiona Ctrl+C para salir" << endl;
    while (true) {
        sleep(1);
    }

    subscriber.close_connection();
    return 0;
}
