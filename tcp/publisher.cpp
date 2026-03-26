#include <iostream>
#include <cstring>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <thread>
#include <chrono>
#include <vector>

using namespace std;

const int BROKER_PORT = 5000;
const char* BROKER_IP = "192.168.77.148";
const int BUFFER_SIZE = 1024;

class Publisher {
private:
    int socket_fd;
    string match_name;

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

public:
    Publisher(const string& match) : match_name(match), socket_fd(-1) {}

    bool connect_to_broker() {
        socket_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (socket_fd < 0) {
            cerr << "[PUBLISHER " << match_name << "] Error al crear socket" << endl;
            return false;
        }

        struct sockaddr_in broker_addr;
        memset(&broker_addr, 0, sizeof(broker_addr));
        broker_addr.sin_family = AF_INET;
        broker_addr.sin_port = htons(BROKER_PORT);

        if (inet_pton(AF_INET, BROKER_IP, &broker_addr.sin_addr) <= 0) {
            cerr << "[PUBLISHER " << match_name << "] IP del broker inválida" << endl;
            return false;
        }

        if (connect(socket_fd, (struct sockaddr*)&broker_addr, sizeof(broker_addr)) < 0) {
            cerr << "[PUBLISHER " << match_name << "] Error al conectar con broker" << endl;
            return false;
        }

        cout << "[PUBLISHER " << match_name << "] Conectado al broker en " << BROKER_IP << ":" << BROKER_PORT << endl;
        return true;
    }

    void publish_events() {
        cout << "[PUBLISHER " << match_name << "] Iniciando publicación de eventos..." << endl;

        for (int i = 0; i < (int)events.size(); i++) {
            string message = "PUBLISH:" + match_name + ":" + events[i];

            if (send(socket_fd, message.c_str(), message.length(), 0) < 0) {
                cerr << "[PUBLISHER " << match_name << "] Error al enviar mensaje" << endl;
                break;
            }

            cout << "[PUBLISHER " << match_name << "] Enviado: " << events[i] << endl;

            // Simular eventos en tiempo real con retrasos variables (1-3 segundos)
            sleep(1 + (rand() % 3));
        }

        cout << "[PUBLISHER " << match_name << "] Finalizada publicación de eventos" << endl;
    }

    void close_connection() {
        if (socket_fd >= 0) {
            close(socket_fd);
            cout << "[PUBLISHER " << match_name << "] Desconectado del broker" << endl;
        }
    }

    ~Publisher() {
        close_connection();
    }
};

int main(int argc, char* argv[]) {
    if (argc < 2) {
        cerr << "Uso: " << argv[0] << " <nombre_partido>" << endl;
        cerr << "Ejemplo: " << argv[0] << " Real_Madrid_vs_Barcelona" << endl;
        return 1;
    }

    string match_name = argv[1];
    Publisher publisher(match_name);

    if (!publisher.connect_to_broker()) {
        cerr << "Fallo al conectar con broker" << endl;
        return 1;
    }

    publisher.publish_events();
    publisher.close_connection();

    return 0;
}
