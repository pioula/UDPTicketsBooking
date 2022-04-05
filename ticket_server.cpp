#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <iostream>
#include <sys/types.h>
#include <string>
#include <cstring>
#include <unistd.h>
#include <cassert>
#include <stack>
#include <vector>
#include <queue>
#include <optional>
#include "err.h"

using std::stack;
using std::vector;
using std::queue;
using std::string;
using std::pair;
using std::make_pair;
using std::optional;
using std::nullopt;
using std::to_string;

using id_t = uint32_t;
using timeout_t = uint32_t;
using tickets_t = uint16_t;

constexpr uint8_t COOKIE_LENGTH = 48;
constexpr uint16_t DEFAULT_PORT = 2022;
constexpr timeout_t TIMEOUT_MAX = 86400;
constexpr timeout_t DEFAULT_TIMEOUT = 5;

constexpr uint8_t FILE_LINE_LENGTH = 80;
constexpr uint16_t BUFFER_SIZE = 65507;
constexpr uint16_t COMMAND_LINE_LENGTH = 4096;
constexpr const char* WRONG_USAGE = "wrong usage";

constexpr uint8_t EMPTY_COMMAND = 0;
constexpr uint8_t GET_EVENTS = 1;
constexpr uint8_t EVENTS = 2;
constexpr uint8_t GET_RESERVATION = 3;
constexpr uint8_t RESERVATION = 4;
constexpr uint8_t GET_TICKETS = 5;
constexpr uint8_t TICKETS = 6;
constexpr uint8_t BAD_REQUEST = 255;


char shared_buffer[BUFFER_SIZE];
char input_buffer[BUFFER_SIZE];

using reservation_response_t = struct reservation_response {
    bool success;
    id_t reservation_id;
    id_t event_id;
    tickets_t ticket_count;
    string cookie;
    timeout_t expiration_time;
};

class Tickets {
private:
    const uint8_t N_LETTERS = 25;
    const uint8_t TICKET_LENGTH = 7;
    vector<string> tickets;
    id_t reservation_id;
    void generate_new_ticket() {
        string ticket;
        for (uint8_t i = 0; i < TICKET_LENGTH; i++) {
            if (random() % 2) {
                ticket += (char)(random() % 'A' + N_LETTERS + 1);
            }
            else {
                ticket += (char)(random() % 'a' + N_LETTERS + 1);
            }
        }
        tickets.push_back(ticket);
    }
public:
    Tickets(id_t _reservation_id, tickets_t ticket_count):
        reservation_id(_reservation_id){
        if (ticket_count != 0) {
            for (tickets_t i = 0; i < ticket_count; i++) {
                generate_new_ticket();
            }
        }
    }
};

class FileReader {
public:
    static FILE* open_file(char *path) {
        FILE* tmp = fopen(path, "r");
        if (!tmp) {
            fclose(tmp);
            fatal("Error while loading file");
        }

        return tmp;
    }

    static void close_file(FILE* f) {
        fclose(f);
    }

    static uint32_t count_lines(char *path) {
        FILE* f = open_file(path);
        char ch;
        int lines_count = 1;
        while ((ch = (char)fgetc(f)) != EOF) {
            if (ch == '\n')
                lines_count++;
        }

        fclose(f);
        return lines_count;
    }

    static void read_line(FILE* f, string &s) {
        for (char c = (char)fgetc(f); c != EOF && c != '\n'; c = (char)fgetc(f))
            s += c;
    }

    static void read_short(FILE* f, uint16_t &s) {
        fscanf(f, "%hu ", &s); // NOLINT(cert-err34-c)
    }
};

class Events {
private:
    using event_t = struct event {
        string description;
        uint16_t tickets;
    };

    size_t n;
    vector<event_t> list;
public:
    Events(char *path) {
        FILE* f = FileReader::open_file(path);
        n = FileReader::count_lines(path) >> 1;
        for (size_t i = 0; i < n; i++) {
            event_t new_event;
            FileReader::read_line(f, new_event.description);
            FileReader::read_short(f, new_event.tickets);
            list.push_back(new_event);
        }
        FileReader::close_file(f);
    }

    size_t size() {
        return list.size();
    }

    void add_tickets(id_t event_id, tickets_t tickets) {
        list[event_id].tickets += tickets;
    }

    void remove_tickets(id_t event_id, tickets_t tickets) {
        list[event_id].tickets -= tickets;
    }

    tickets_t get_tickets(id_t event_id) {
        return list[event_id].tickets;
    }

    string get_description(id_t event_id) {
        return list[event_id].description;
    }
};

class Reservations {
public:
    using reservation_t = struct reservation {
        tickets_t tickets;
        id_t event_id;
        string cookie;
    };
private:
    using deadline_t = struct deadline {
        timeout_t timeout;
        id_t reservation_id;
        string cookie;
    };

    timeout_t timeout;
    vector<reservation_t> reservations;
    stack<id_t> free_ids;
    queue<deadline_t> deadlines;
    Events events;

    void update_reservations() {
        time_t current_time = time(nullptr);
        while (!deadlines.empty()) {
            if (deadlines.front().timeout >= current_time) {
                deadline d = deadlines.front();
                deadlines.pop();
                if (d.cookie == reservations[d.reservation_id].cookie) {
                    events.add_tickets(reservations[d.reservation_id].event_id,
                                       reservations[d.reservation_id].tickets);
                    free_ids.push(d.reservation_id);
                }
            }
            else {
                break;
            }
        }
    }

    id_t get_new_reservation_id() {
        if (free_ids.empty()) {
            free_ids.push(reservations.size());
        }

        id_t id = free_ids.top();
        free_ids.pop();
        return id;
    }

    void put_reservation(reservation_t &reservation, id_t id) {
        assert(id <= reservations.size());
        if (id == reservations.size()) {
            reservations.push_back(reservation);
        }
        else {
            reservations[id] = reservation;
        }
    }

    static string generate_cookie() {
        string s;
        for (int i = 0; i < COOKIE_LENGTH; i++) {
            s += (char)(rand() % 94 + 33);
        }
        return s;
    }
public:
    Reservations(timeout_t _timeout, Events &_events):
        timeout(_timeout), events(_events) {}

    Events get_events() {
        return events;
    }

    pair<id_t, reservation_t>
            book(id_t event_id, tickets_t ticket_count) {
        update_reservations();
        if (event_id >= events.size() || ticket_count == 0 ||
            events.get_tickets(event_id) < ticket_count) {
            //TODO BAD REQUEST
        }

        id_t reservation_id = get_new_reservation_id();
        reservation_t reservation;
        reservation.tickets = ticket_count;
        reservation.event_id = event_id;
        reservation.cookie = generate_cookie();

        deadline_t deadline;
        deadline.timeout = time(nullptr) + timeout;
        deadline.reservation_id = reservation_id;
        deadline.cookie = reservation.cookie;

        deadlines.push(deadline);
        put_reservation(reservation, reservation_id);
        events.remove_tickets(event_id, ticket_count);

        return make_pair(reservation_id, reservation);
    }

    optional<Tickets> get_tickets(id_t reservation_id, string &cookie) {
        update_reservations();
        if (reservation_id >= reservations.size() ||
            reservations[reservation_id].cookie == cookie) {
            return nullopt;
        }

        if (reservations[reservation_id].cookie == cookie) {
            reservations[reservation_id].cookie = "";
            Tickets tickets(reservation_id,
                            reservations[reservation_id].tickets);
            return Tickets(reservation_id,
                           reservations[reservation_id].tickets);
        }

        return nullopt;
    }
};

unsigned long read_number(char *string) {
    errno = 0;
    unsigned long number = strtoul(string, nullptr, 10);
    if (errno != 0) {
        puts("TODO ZABEZPIECZENIE");
    }
    return number;
}

uint16_t read_port(char *string) {
    unsigned long port = read_number(string);
    if (port > UINT16_MAX) {
        fatal("%ul is not a valid port number", port);
    }

    return (uint16_t)port;
}

uint32_t read_timeout(char *string) {
    uint32_t timeout = read_number(string);
    if (timeout > TIMEOUT_MAX) {
        fatal("%ul is not a valid timout", timeout);
    }

    return timeout;
}

int find_flag(const char* flag, char *argv[], int argc, int *flag_counter) {
    int flag_position = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(flag, argv[i]) == 0) {
            flag_position = i;
            (*flag_counter)++;
        }
    }

    return flag_position;
}

int get_file(char *argv[], int argc) {
    int count_f = 0;
    int flag_position = find_flag("-f", argv, argc, &count_f);

    if (count_f == 0 || count_f > 1 || flag_position + 1 == argc) {
        fatal(WRONG_USAGE);
    }

    return flag_position + 1;
}

uint16_t get_port(char *argv[], int argc) {
    int count_p = 0;
    int flag_position = find_flag("-p", argv, argc, &count_p);

    if (count_p == 0)
        return DEFAULT_PORT;

    if (count_p > 1 || flag_position + 1 == argc)
        fatal(WRONG_USAGE);

    return read_port(argv[flag_position + 1]);
}

uint32_t get_timeout(char *argv[], int argc) {
    int count_t = 0;
    int flag_position = find_flag("-t", argv, argc, &count_t);
    if (count_t > 1 || flag_position + 1 == argc)
        fatal(WRONG_USAGE);

    if (count_t == 0)
        return DEFAULT_TIMEOUT;

    return read_timeout(argv[flag_position + 1]);
}

bool is_flag(char* string) {
    return strcmp(string, "-f") == 0 || strcmp(string, "-p") == 0
           || strcmp(string, "-t") == 0;
}

void check_correctness(char *argv[], int argc) {
    if ((argc - 1) & 1) {
        fatal(WRONG_USAGE);
    }

    for (int i = 1; i < argc; i += 2) {
        if (!is_flag(argv[i])) {
            fatal(WRONG_USAGE);
        }
    }
}

int bind_socket(uint16_t port) {
    int socket_fd = socket(AF_INET, SOCK_DGRAM, 0); // creating IPv4 UDP socket
    ENSURE(socket_fd > 0);
    // after socket() call; we should close(sock) on any execution path;

    struct sockaddr_in server_address;
    server_address.sin_family = AF_INET; // IPv4
    server_address.sin_addr.s_addr = htonl(INADDR_ANY); // listening on all interfaces
    server_address.sin_port = htons(port);

    // bind the socket to a concrete address
    errno = 0;
    if (bind(socket_fd, (struct sockaddr *) &server_address,
                     (socklen_t) sizeof(server_address)) != 0) {
        puts("TODO bind_socket");
    }

    return socket_fd;
}

size_t read_message(int socket_fd, struct sockaddr_in *client_address, char *buffer, size_t max_length) {
    socklen_t address_length = (socklen_t) sizeof(*client_address);
    int flags = 0; // we do not request anything special
    errno = 0;
    ssize_t len = recvfrom(socket_fd, buffer, max_length, flags,
                           (struct sockaddr *) client_address, &address_length);
    if (len < 0) {
        puts("TODO read_message");
    }
    return (size_t) len;
}

void send_message(int socket_fd, const struct sockaddr_in *client_address, const char *message, size_t length) {
    socklen_t address_length = (socklen_t) sizeof(*client_address);
    int flags = 0;
    ssize_t sent_length = sendto(socket_fd, message, length, flags,
                                 (struct sockaddr *) client_address, address_length);
    ENSURE(sent_length == (ssize_t) length);
}

uint16_t read_message_id(char* string) {
    unsigned long number = read_number(string);
    if (number <= UINT8_MAX) {
        return (uint16_t)number;
    }

    return EMPTY_COMMAND;
}

void send_events(int socket_fd,
                 struct sockaddr_in &client_address,
                 Events events) {
    string new_event;
    size_t number_of_bytes = 0;
    string new_message;

    for (size_t i = 0; i < events.size(); i++) {
        new_event = to_string(EVENTS)
            + " " + to_string(events.get_tickets(i))
            + " " + to_string(events.get_description(i).size())
            + " " + events.get_description(i) + "\n";

        if (new_event.size() + number_of_bytes > BUFFER_SIZE)
            continue;

        std::cout << new_event << "\n";

        new_message += new_event;
        number_of_bytes += new_event.size();
    }

    send_message(socket_fd, &client_address,
                 new_message.c_str(), new_message.size());
}

void make_reservation(int socket_fd, struct sockaddr_in &client_address,
                      Reservations &reservations, FILE* stream) {
    uint32_t event_id;
    int16_t tickets_count;
    fscanf(stream, "%u%hu", &event_id, &tickets_count);

    pair<id_t, Reservations::reservation> res =
            reservations.book(event_id, tickets_count);
}


void handle_next_request(int socket_fd, Reservations &reservations) {
    struct sockaddr_in client_address;
    read_message(socket_fd,
                 &client_address,
                 shared_buffer,
                 sizeof(shared_buffer));
    uint16_t message_id;
    FILE* client_message_stream = fmemopen(shared_buffer,
                                           strlen(shared_buffer), "r");

    fscanf(client_message_stream, "%s", input_buffer);
    message_id = read_message_id(input_buffer);
    switch (message_id) {
        case GET_EVENTS:
            send_events(socket_fd, client_address, reservations.get_events());
            break;
        case GET_RESERVATION:
            make_reservation(socket_fd, client_address,
                             reservations, client_message_stream);
            break;
        case GET_TICKETS:
            break;
    }

    fclose(client_message_stream);
}

int main(int argc, char *argv[]) {
    srand(time(nullptr));
    if (argc < 2 || argc > 6) {
        fatal("usage: %s -f <file> -p <port> -t <timeout>", argv[0]);
    }

    check_correctness(argv, argc);
    int file_position = get_file(argv, argc);
    uint16_t port = get_port(argv, argc);
    uint32_t timeout = get_timeout(argv, argc);

    Events events(argv[file_position]);
    Reservations reservations(timeout, events);

    memset(shared_buffer, 0, sizeof(shared_buffer));

    int socket_fd = bind_socket(port);

    do {
        handle_next_request(socket_fd, reservations);
    } while (true);
    printf("finished exchange\n");

    return 0;
}
