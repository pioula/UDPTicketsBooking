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

/*
using reservation_response_t = struct reservation_response {
    bool success;
    id_t reservation_id;
    id_t event_id;
    tickets_t ticket_count;
    string cookie;
    timeout_t expiration_time;
};
 */

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
using timeout_t = uint64_t;
using tickets_t = uint16_t;

constexpr uint8_t MESSAGE_ID_B = 1;
constexpr uint8_t DESCRIPTION_LEN_B = 1;
constexpr uint8_t TICKET_COUNT_B = 2;
constexpr uint8_t EVENT_ID_B = 4;
constexpr uint8_t RESERVATION_ID_B = 4;
constexpr uint8_t EXPIRATION_TIME_B = 8;
constexpr uint8_t TICKET_B = 7;
constexpr uint8_t COOKIE_B = 48;

constexpr uint16_t DEFAULT_PORT = 2022;
constexpr timeout_t TIMEOUT_MAX = 86400;
constexpr timeout_t DEFAULT_TIMEOUT = 5;

constexpr uint8_t FILE_LINE_LENGTH = 80;
constexpr uint16_t BUFFER_SIZE = 65507;
constexpr uint16_t COMMAND_LINE_LENGTH = 4096;
constexpr const char* WRONG_USAGE = "wrong usage";

constexpr size_t GET_EVENTS_SIZE = MESSAGE_ID_B;
constexpr size_t EVENTS_SIZE = MESSAGE_ID_B + EVENT_ID_B + TICKET_COUNT_B
        + DESCRIPTION_LEN_B;
constexpr size_t GET_RESERVATION_SIZE = MESSAGE_ID_B + EVENT_ID_B
        + TICKET_COUNT_B;
constexpr size_t RESERVATION_SIZE = MESSAGE_ID_B + RESERVATION_ID_B + EVENT_ID_B
        + TICKET_COUNT_B + COOKIE_B + EXPIRATION_TIME_B;
constexpr size_t GET_TICKETS_SIZE = MESSAGE_ID_B + RESERVATION_ID_B + COOKIE_B;
constexpr size_t TICKETS_SIZE = MESSAGE_ID_B + RESERVATION_ID_B
        + TICKET_COUNT_B;
constexpr size_t BAD_REQUEST_SIZE = MESSAGE_ID_B + EVENT_ID_B;

constexpr uint8_t EMPTY_COMMAND = 0;
constexpr uint8_t GET_EVENTS = 1;
constexpr uint8_t EVENTS = 2;
constexpr uint8_t GET_RESERVATION = 3;
constexpr uint8_t RESERVATION = 4;
constexpr uint8_t GET_TICKETS = 5;
constexpr uint8_t TICKETS = 6;
constexpr uint8_t BAD_REQUEST = 255;

class ServerHandler {
private:
    using sockaddr_t = struct sockaddr_in;
    using port_t = uint16_t;
    using socket_t = int;
    socket_t socket_fd{};
    port_t port;
    sockaddr_t server_address{};
    sockaddr_t client_address{};
    socklen_t client_address_length{};

    void bind_socket() {
        socket_fd = socket(AF_INET, SOCK_DGRAM, 0); // creating IPv4 UDP socket
        ENSURE(socket_fd > 0);
        // after socket() call; we should close(sock) on any execution path;

        server_address.sin_family = AF_INET; // IPv4
        // listening on all interfaces
        server_address.sin_addr.s_addr = htonl(INADDR_ANY);
        server_address.sin_port = htons(port);

        // bind the socket to a concrete address
        errno = 0;
        if (bind(socket_fd, (struct sockaddr *) &server_address,
                 (socklen_t) sizeof(server_address)) != 0) {
            PRINT_ERRNO();
        }
    }
public:
    ServerHandler(port_t _port): port(_port) {
        bind_socket();
    }

    size_t read_message(char *buffer) {
        client_address_length = (socklen_t) sizeof(client_address);
        int flags = 0; // we do not request anything special
        errno = 0;
        ssize_t len = recvfrom(socket_fd, buffer, sizeof(buffer), flags,
                               (struct sockaddr *) &client_address,
                               &client_address_length);
        if (len < 0) {
            PRINT_ERRNO();
        }
        return (size_t) len;
    }

    void send_message(const char *message, size_t length) {
        int flags = 0;
        ssize_t sent_length = sendto(socket_fd, message, length, flags,
                                     (struct sockaddr *) &client_address,
                                     client_address_length);
        ENSURE(sent_length == (ssize_t)length);
    }
};

class CommunicatHandler {
private:
    char communicat[BUFFER_SIZE]{};
    size_t length{};
    size_t read_com_ptr{};
    size_t write_com_ptr{};
    ServerHandler server_handler;
public:
    CommunicatHandler(ServerHandler &_server_handler):
        server_handler(_server_handler) {}

    void load_communicat() {
        read_com_ptr = 0;
        length = server_handler.read_message(communicat);
    }

    size_t get_length() {
        return length;
    }

    void read_bytes(void* dest, size_t bytes) {
        memcpy(dest, communicat + read_com_ptr, bytes);
        read_com_ptr += bytes;
    }

    void start_writing() {
        write_com_ptr = 0;
    }

    CommunicatHandler* write_bytes(const void* src, size_t bytes) {
        memcpy(communicat + write_com_ptr, src, bytes);
        write_com_ptr += bytes;
        return this;
    }

    size_t get_written_buffer_length() const {
        return write_com_ptr;
    }

    void send_communicat() {
        server_handler.send_message(communicat, write_com_ptr);
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

    static int read_short(FILE* f, uint16_t &s) {
        return fscanf(f, "%hu ", &s);
    }
};

class Tickets {
private:
    static constexpr uint8_t N_LETTERS = 25;
    static constexpr uint8_t TICKET_LENGTH = 7;
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

    id_t* get_reservation_id() {
        return &reservation_id;
    }

    vector<string> get_tickets() {
        return tickets;
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

    tickets_t* get_tickets(id_t event_id) {
        return &(list[event_id].tickets);
    }

    string* get_description(id_t event_id) {
        return &(list[event_id].description);
    }
};

class Reservations {
public:
    using reservation_t = struct reservation {
        tickets_t tickets;
        id_t event_id;
        string cookie;
        timeout_t expiration_time;
        bool is_read;
    };
private:
    static constexpr id_t MIN_RESERVATION_ID = 1000000;
    using deadline_t = struct deadline {
        timeout_t timeout;
        id_t reservation_id;
    };

    timeout_t timeout;
    Events events;
    vector<reservation_t> reservations;
    id_t next_id;
    queue<deadline_t> deadlines;


    void update_reservations() {
        time_t current_time = time(nullptr);
        while (!deadlines.empty()) {
            if ((long int)deadlines.front().timeout >= current_time) {
                deadline d = deadlines.front();
                deadlines.pop();
                if (!reservations[d.reservation_id].is_read) {
                    events.add_tickets(reservations[d.reservation_id].event_id,
                                       reservations[d.reservation_id].tickets);
                    reservations[d.reservation_id].cookie = generate_cookie();
                }
            }
            else {
                break;
            }
        }
    }

    id_t get_new_reservation_id() {
        return next_id++;
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
        for (int i = 0; i < COOKIE_B; i++) {
            s += (char)(rand() % 94 + 33);
        }
        return s;
    }
public:
    Reservations(timeout_t _timeout, Events &_events):
        timeout(_timeout), events(_events), next_id(0) {}

    Events get_events() {
        return events;
    }

    optional<pair<id_t, reservation_t>>
            book(id_t event_id, tickets_t ticket_count) {
        update_reservations();
        if (event_id >= events.size() || ticket_count == 0 ||
            *events.get_tickets(event_id) < ticket_count ||
            ticket_count * TICKET_B + TICKETS_SIZE > BUFFER_SIZE) {
            return nullopt;
        }

        id_t reservation_id = get_new_reservation_id();
        reservation_t reservation;
        reservation.tickets = ticket_count;
        reservation.event_id = event_id;
        reservation.cookie = generate_cookie();
        reservation.expiration_time = time(nullptr) + timeout;

        deadline_t deadline;
        deadline.timeout = reservation.expiration_time;
        deadline.reservation_id = reservation_id;

        deadlines.push(deadline);
        put_reservation(reservation, reservation_id);
        events.remove_tickets(event_id, ticket_count);

        return make_pair(reservation_id + MIN_RESERVATION_ID, reservation);
    }

    optional<Tickets> get_tickets(id_t reservation_id, string &cookie) {
        reservation_id -= MIN_RESERVATION_ID;
        update_reservations();
        reservations[reservation_id].is_read = true;
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

    timeout_t get_timeout() const {
        return timeout;
    }
};

unsigned long read_number(char *string) {
    errno = 0;
    unsigned long number = strtoul(string, nullptr, 10);
    if (errno != 0) {
        PRINT_ERRNO();
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

void send_events(CommunicatHandler &handler, Events events) {
    if (handler.get_length() > GET_EVENTS_SIZE) return;

    handler.start_writing();
    uint8_t message_id = EVENTS;
    size_t number_of_bytes = handler.write_bytes(&message_id, MESSAGE_ID_B)
                                ->get_written_buffer_length();

    for (id_t event_id = 0; event_id < events.size(); event_id++) {
        if (number_of_bytes + EVENTS_SIZE +
            events.get_description(event_id)->size() > BUFFER_SIZE) {
            continue;
        }
        uint32_t test = event_id + 1;
        uint8_t description_length = events.get_description(event_id)->size();
        number_of_bytes = handler.write_bytes(&test, EVENT_ID_B)
            ->write_bytes(events.get_tickets(event_id), TICKET_COUNT_B)
            ->write_bytes(&description_length, DESCRIPTION_LEN_B)
            ->write_bytes(events.get_description(event_id), description_length)
            ->get_written_buffer_length();
    }

    handler.send_communicat();
}

void make_reservation(CommunicatHandler &handler,
                      Reservations &reservations) {
    if (handler.get_length() > GET_RESERVATION_SIZE) return;

    id_t event_id;
    tickets_t tickets_count;
    handler.read_bytes(&event_id, EVENT_ID_B);
    handler.read_bytes(&tickets_count, TICKET_COUNT_B);

    handler.start_writing();
    if (auto res = reservations.book(event_id, tickets_count)) {
        uint8_t message_id = RESERVATION;
        handler.write_bytes(&message_id, MESSAGE_ID_B)
            ->write_bytes(&(res->first), RESERVATION_ID_B)
            ->write_bytes(&(res->second.event_id), EVENT_ID_B)
            ->write_bytes(&(res->second.tickets), TICKET_COUNT_B)
            ->write_bytes(res->second.cookie.c_str(), COOKIE_B)
            ->write_bytes(&(res->second.expiration_time), EXPIRATION_TIME_B)
            ->send_communicat();
    }
    else {
        uint8_t message_id = BAD_REQUEST;
        handler.write_bytes(&message_id, MESSAGE_ID_B)
            ->write_bytes(&event_id, EVENT_ID_B)
            ->send_communicat();
    }
}

void send_tickets(CommunicatHandler &handler, Reservations &reservations) {
    if (handler.get_length() > GET_TICKETS_SIZE) return;

    id_t reservation_id;
    char cookie[COOKIE_B];
    handler.read_bytes(&reservation_id, RESERVATION_ID_B);
    handler.read_bytes(cookie, COOKIE_B);
    string cookie_s(cookie);
    handler.start_writing();

    if (auto tickets = reservations.get_tickets(reservation_id, cookie_s)) {
        uint8_t message_id = TICKETS;
        size_t tickets_count = (*tickets).get_tickets().size();
        handler.write_bytes(&message_id, MESSAGE_ID_B)
            ->write_bytes((*tickets).get_reservation_id(), RESERVATION_ID_B)
            ->write_bytes(&tickets_count, TICKET_COUNT_B);
        vector<string> tickets_ids = (*tickets).get_tickets();
        for (auto & tickets_id : tickets_ids) {
            handler.write_bytes(tickets_id.c_str(), TICKET_B);
        }
        handler.send_communicat();
    }
    else {
        uint8_t message_id = BAD_REQUEST;
        handler.write_bytes(&message_id, MESSAGE_ID_B)
            ->write_bytes(&reservation_id, EVENT_ID_B)
            ->send_communicat();
    }
}

void handle_next_request(CommunicatHandler &handler,
                         Reservations &reservations) {
    handler.load_communicat();
    uint8_t message_id;
    handler.read_bytes(&message_id, sizeof(message_id));

    switch (message_id) {
        case GET_EVENTS:
            send_events(handler, reservations.get_events());
            break;
        case GET_RESERVATION:
            make_reservation(handler,
                             reservations);
            break;
        case GET_TICKETS:
            send_tickets(handler, reservations);
            break;
    }
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

    std::cout << "Server: " << port << " " << timeout << std::endl;

    ServerHandler server_handler(port);
    Events events(argv[file_position]);
    Reservations reservations(timeout, events);
    CommunicatHandler handler(server_handler);

    while (true) {
        handle_next_request(handler, reservations);
    }

    printf("finished exchange\n");

    return 0;
}
