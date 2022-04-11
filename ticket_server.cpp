#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <string>
#include <cstring>
#include <stack>
#include <vector>
#include <queue>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cerrno>
#include <fstream>

// Evaluate `x`: if false, print an error message and exit with an error.
#define ENSURE(x)                                                         \
    do {                                                                  \
        bool result = (x);                                                \
        if (!result) {                                                    \
            fprintf(stderr, "Error: %s was false in %s at %s:%d\n",       \
                #x, __func__, __FILE__, __LINE__);                        \
            exit(EXIT_FAILURE);                                           \
        }                                                                 \
    } while (0)

// Check if errno is non-zero, and if so,
// print an error message and exit with an error.
#define PRINT_ERRNO()                                                  \
    do {                                                               \
        if ((*__errno_location()) != 0) {                              \
            fprintf(stderr, "Error: errno %d in %s at %s:%d\n%s\n",    \
              (*__errno_location()), __func__, __FILE__, __LINE__,     \
              strerror((*__errno_location())));   \
            exit(EXIT_FAILURE);                                        \
        }                                                              \
    } while (0)

// Print an error message and exit with an error.
void fatal(const char *fmt, ...) {
    va_list fmt_args;

    fprintf(stderr, "Error: ");
    va_start(fmt_args, fmt);
    vfprintf(stderr, fmt, fmt_args);
    va_end(fmt_args);
    fprintf(stderr, "\n");
    exit(EXIT_FAILURE);
}

using std::stack;
using std::vector;
using std::queue;
using std::string;
using std::pair;
using std::make_pair;
using std::to_string;
using std::swap;
using std::exception;

using id_t = uint32_t;
using timeout_t = uint64_t;
using tickets_t = uint16_t;

constexpr uint8_t MESSAGE_ID_B = 1;
constexpr uint8_t DESCRIPTION_LEN_B = 1;
constexpr uint8_t TICKET_COUNT_B = 2;
constexpr uint8_t ID_B = 4;
constexpr uint8_t EXPIRATION_TIME_B = 8;
constexpr uint8_t TICKET_B = 7;
constexpr uint8_t COOKIE_B = 48;

constexpr uint16_t DEFAULT_PORT = 2022;
constexpr timeout_t TIMEOUT_MIN = 1;
constexpr timeout_t TIMEOUT_MAX = 86400;
constexpr timeout_t DEFAULT_TIMEOUT = 5;

constexpr uint16_t BUFFER_SIZE = 65507;

constexpr size_t GET_EVENTS_SIZE = MESSAGE_ID_B;
constexpr size_t EVENTS_SIZE = MESSAGE_ID_B + ID_B + TICKET_COUNT_B
                               + DESCRIPTION_LEN_B;
constexpr size_t GET_RESERVATION_SIZE = MESSAGE_ID_B + ID_B
                                        + TICKET_COUNT_B;
constexpr size_t GET_TICKETS_SIZE = MESSAGE_ID_B + ID_B + COOKIE_B;
constexpr size_t TICKETS_SIZE = MESSAGE_ID_B + ID_B
                                + TICKET_COUNT_B;

constexpr uint8_t GET_EVENTS = 1;
constexpr uint8_t EVENTS = 2;
constexpr uint8_t GET_RESERVATION = 3;
constexpr uint8_t RESERVATION = 4;
constexpr uint8_t GET_TICKETS = 5;
constexpr uint8_t TICKETS = 6;
constexpr uint8_t BAD_REQUEST = 255;

class WrongRequest: public exception {};

// A class that handles sending and receiving messages from clients
class ServerHandler {
private:
    using sockaddr_t = struct sockaddr_in;
    using port_t = uint16_t;
    using socket_t = int;
    socket_t socket_fd;
    port_t port;
    sockaddr_t server_address;
    sockaddr_t client_address;
    socklen_t client_address_length;

    void bind_socket() {
        socket_fd = socket(AF_INET, SOCK_DGRAM, 0); // creating IPv4 UDP socket
        ENSURE(socket_fd > 0);

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
        ssize_t len = recvfrom(socket_fd, buffer, BUFFER_SIZE, flags,
                               (struct sockaddr *) &client_address,
                               &client_address_length);
        if (len < 0) {
            PRINT_ERRNO();
        }
        return (size_t) len;
    }

    void send_message(const char *message, const size_t length) const {
        int flags = 0;
        ssize_t sent_length = sendto(socket_fd, message, length, flags,
                                     (struct sockaddr *) &client_address,
                                     client_address_length);
        ENSURE(sent_length == (ssize_t)length);
    }
};

// A class that handles communicates sent by clients and prepares new to be sent
class CommunicatHandler {
private:
    char communicat[BUFFER_SIZE];
    size_t length; // length of read communicat
    size_t read_com_ptr;
    size_t write_com_ptr;
    ServerHandler server_handler;

    void reverse_substring(char* str, const size_t bytes) {
        for (size_t i = 0; i < (bytes >> 1); i++)
            swap(str[i], str[bytes - i - 1]);
    }

    void copy_bytes(void* dest, const size_t bytes) {
        memcpy(dest, communicat + read_com_ptr, bytes);
        read_com_ptr += bytes;
    }

    void overwrite_bytes(const void* src, const size_t bytes) {
        memcpy(communicat + write_com_ptr, src, bytes);
        write_com_ptr += bytes;
    }

public:
    CommunicatHandler(ServerHandler &_server_handler):
        server_handler(_server_handler) {}

    void load_communicat() {
        read_com_ptr = 0;
        write_com_ptr = 0;
        length = server_handler.read_message(communicat);
    }

    size_t get_length() const {
        return length;
    }

    void read_bytes(char* dest, const size_t bytes) {
        copy_bytes(dest, bytes);
    }

    void read_bytes(void* dest, const size_t bytes) {
        // We have to change endian for numbers
        reverse_substring(communicat + read_com_ptr, bytes);
        copy_bytes(dest, bytes);
    }

    CommunicatHandler* write_bytes(const char* src, const size_t bytes) {
        overwrite_bytes(src, bytes);
        return this;
    }

    CommunicatHandler* write_bytes(const void* src, const size_t bytes) {
        overwrite_bytes(src, bytes);
        // We have to change endian
        reverse_substring(communicat + write_com_ptr - bytes, bytes);
        return this;
    }

    size_t get_written_buffer_length() const {
        return write_com_ptr;
    }

    void send_communicat() const {
        server_handler.send_message(communicat, write_com_ptr);
    }
};

// A class that handles reading from files
class FileReader {
private:
    const char* path;
    FILE* file;

    static FILE* open_file(const char *p) {
        FILE* tmp = fopen(p, "r");
        ENSURE(tmp);
        return tmp;
    }
public:
    FileReader(const char* path_): path(path_) {
        file = fopen(path, "r");
        ENSURE(file);
    }

    ~FileReader() {
        fclose(file);
    }

    // Returns true if there are more lines to read
    bool read_line(string &s) {
        s = "";
        char c;
        for (c = (char)fgetc(file);
            c != EOF && c != '\n'; c = (char)fgetc(file))
            s += c;
        return c != EOF;
    }

    int read_short(uint16_t &s) {
        return fscanf(file, "%hu ", &s);
    }
};

// A class that generates unique ticket codes
class TicketPrinter {
private:
    static constexpr uint8_t TICKET_LENGTH = 7;

    char fields[TICKET_LENGTH];

    // creates new ticket code
    void increase_position(uint8_t pos) {
        if (++fields[pos] > 'Z') {
            fields[pos++] = '0';
            increase_position(pos);
        }
        else if (fields[pos] > '9' && fields[pos] < 'A') {
            fields[pos] = 'A';
        }
    }
public:
    TicketPrinter() {
        memset(fields, '0', TICKET_LENGTH);
    }

    string get_unique_ticket() {
        string res;
        for (char field : fields) {
            res += field;
        }
        increase_position(0);
        return res;
    }

    void fill_tickets(vector<string> &ticket_container, const tickets_t count) {
        for (tickets_t ticket = 0; ticket < count; ++ticket) {
            ticket_container.push_back(get_unique_ticket());
        }
    }
};

using event_t = struct event_t {
    string description;
    tickets_t tickets;
};

// A class that stores events
class Events {
private:
    vector<event_t> list;
public:
    Events(char *path) {
        FileReader f = FileReader(path);
        event_t new_event;
        while(f.read_line(new_event.description)) {
            f.read_short(new_event.tickets);
            list.push_back(new_event);
        }
    }

    size_t size() const {
        return list.size();
    }

    event_t* get_event(const id_t event_id) {
        return &(list[event_id]);
    }
};

using reservation_t = struct Reservation {
    id_t event_id;
    string cookie;
    timeout_t expiration_time;
    vector<string> tickets;
    bool is_read;
};

// A class that stores information about all events and reservations
class ReservationCentre {
private:
    static constexpr id_t MIN_RESERVATION_ID = 1000000;
    using deadline_t = struct deadline {
        timeout_t expiration_time;
        id_t reservation_id;
    };

    timeout_t timeout;
    Events events;
    vector<reservation_t> reservations;
    id_t next_id;
    queue<deadline_t> deadlines;
    TicketPrinter ticket_printer;

    // removes from queue all reservations that are expired
    void update_reservations() {
        time_t current_time = time(nullptr);
        while (!deadlines.empty()) {
            if ((time_t)deadlines.front().expiration_time < current_time) {
                deadline d = deadlines.front();
                deadlines.pop();
                if (!reservations[d.reservation_id].is_read) {
                    events.get_event(reservations[d.reservation_id].event_id)
                        ->tickets +=
                                reservations[d.reservation_id].tickets.size();
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

    static string generate_cookie() {
        string s;
        for (int i = 0; i < COOKIE_B; i++) {
            s += (char)(rand() % 94 + 33);
        }
        return s;
    }
public:
    ReservationCentre(timeout_t _timeout, Events &_events):
        timeout(_timeout), events(_events), next_id(0),
        ticket_printer() {}

    Events get_events() const {
        return events;
    }

    // On success returns reservation_id and reservation data
    // otherwise throws WrongRequest
    pair<id_t, Reservation> book_event(id_t event_id, tickets_t ticket_count) {
        update_reservations();
        // Checks if request is valid
        if (event_id >= events.size() || ticket_count == 0 ||
            events.get_event(event_id)->tickets < ticket_count ||
            ticket_count * TICKET_B + TICKETS_SIZE > BUFFER_SIZE) {
            throw WrongRequest();
        }

        // Creates a new reservation
        id_t reservation_id = get_new_reservation_id();
        reservation_t reservation;
        ticket_printer.fill_tickets(reservation.tickets, ticket_count);
        reservation.event_id = event_id;
        reservation.cookie = generate_cookie();
        reservation.expiration_time = time(nullptr) + timeout;
        reservation.is_read = false;
        // Creates a deadline for this reservation
        deadline_t deadline;
        deadline.expiration_time = reservation.expiration_time;
        deadline.reservation_id = reservation_id;

        deadlines.push(deadline);
        reservations.push_back(reservation);

        events.get_event(event_id)->tickets -= ticket_count;

        return make_pair(reservation_id + MIN_RESERVATION_ID, reservation);
    }

    vector<string> get_tickets(id_t reservation_id, string &cookie) {
        reservation_id -= MIN_RESERVATION_ID;
        update_reservations();

        if (reservation_id >= reservations.size() ||
            reservations[reservation_id].cookie != cookie) {
            throw WrongRequest();
        }

        reservations[reservation_id].is_read = true;
        return reservations[reservation_id].tickets;
    }
};

void send_bad_request(id_t id, CommunicatHandler& handler) {
    uint8_t message_id = BAD_REQUEST;
    handler.write_bytes(&message_id, MESSAGE_ID_B)
            ->write_bytes(&id, ID_B)
            ->send_communicat();
}

// sends events to client on GET_EVENTS request
void send_events(CommunicatHandler &handler, Events events) {
    if (handler.get_length() != GET_EVENTS_SIZE) return;

    uint8_t message_id = EVENTS;
    size_t number_of_bytes = handler.write_bytes(&message_id,
                                                 MESSAGE_ID_B)
                                ->get_written_buffer_length();

    for (id_t event_id = 0; event_id < events.size(); event_id++) {
        event_t* event = events.get_event(event_id);
        if (number_of_bytes + EVENTS_SIZE +
            event->description.size() > BUFFER_SIZE) {
            continue;
        }

        uint8_t description_length = event->description.size();
        number_of_bytes = handler.write_bytes(&event_id, ID_B)
            ->write_bytes(&(event->tickets), TICKET_COUNT_B)
            ->write_bytes(&description_length, DESCRIPTION_LEN_B)
            ->write_bytes(event->description.c_str(), description_length)
            ->get_written_buffer_length();
    }

    handler.send_communicat();
}
// books an event on GET_RESERVATION request
void make_reservation(CommunicatHandler &handler,
                      ReservationCentre &reservations) {
    if (handler.get_length() != GET_RESERVATION_SIZE) return;

    id_t event_id;
    tickets_t tickets_count;
    handler.read_bytes(&event_id, ID_B);
    handler.read_bytes(&tickets_count, TICKET_COUNT_B);

    try {
        auto res = reservations.book_event(event_id, tickets_count);
        uint8_t message_id = RESERVATION;
        tickets_t ticketCount = res.second.tickets.size();
        handler.write_bytes(&message_id, MESSAGE_ID_B)
                ->write_bytes(&(res.first), ID_B)
                ->write_bytes(&(res.second.event_id), ID_B)
                ->write_bytes(&ticketCount, TICKET_COUNT_B)
                ->write_bytes(res.second.cookie.c_str(), COOKIE_B)
                ->write_bytes(&(res.second.expiration_time), EXPIRATION_TIME_B)
                ->send_communicat();
    }
    catch (exception &ex) {
        send_bad_request(event_id, handler);
    }
}

string create_string_from_cstring(char *str) {
    string s;
    for (int i = 0; i < COOKIE_B; i++) s += str[i];
    return s;
}

void send_tickets(CommunicatHandler &handler, ReservationCentre &reservations) {
    if (handler.get_length() != GET_TICKETS_SIZE) return;

    id_t reservation_id;
    char cookie[COOKIE_B];
    handler.read_bytes(&reservation_id, ID_B);
    handler.read_bytes(cookie, COOKIE_B);
    string cookie_s = create_string_from_cstring(cookie);

    try {
        auto tickets = reservations.get_tickets(reservation_id, cookie_s);
        uint8_t message_id = TICKETS;
        size_t tickets_count = tickets.size();
        handler.write_bytes(&message_id, MESSAGE_ID_B)
                ->write_bytes(&reservation_id, ID_B)
                ->write_bytes(&tickets_count, TICKET_COUNT_B);
        for (auto & tickets_id : tickets) {
            handler.write_bytes(tickets_id.c_str(), TICKET_B);
        }
        handler.send_communicat();
    }
    catch (exception &ex) {
        send_bad_request(reservation_id, handler);
    }
}

void handle_next_request(CommunicatHandler &handler,
                         ReservationCentre &reservations) {
    handler.load_communicat();
    uint8_t message_id;
    handler.read_bytes(&message_id, sizeof(message_id));

    switch (message_id) {
        case GET_EVENTS:
            send_events(handler, reservations.get_events());
            break;
        case GET_RESERVATION:
            make_reservation(handler, reservations);
            break;
        case GET_TICKETS:
            send_tickets(handler, reservations);
            break;
    }
}

unsigned long read_number(char *string) {
    errno = 0;
    char* ptr;
    unsigned long number = strtoul(string, &ptr, 10);
    PRINT_ERRNO();
    ENSURE(*ptr == '\0');
    return number;
}

uint16_t read_port(char *string) {
    unsigned long port = read_number(string);
    ENSURE(port <= UINT16_MAX);
    return (uint16_t)port;
}

uint32_t read_timeout(char *string) {
    uint32_t timeout = read_number(string);
    ENSURE(timeout <= TIMEOUT_MAX && timeout >= TIMEOUT_MIN);
    return timeout;
}

bool is_flag(char* string) {
    return strcmp(string, "-f") == 0 || strcmp(string, "-p") == 0
           || strcmp(string, "-t") == 0;
}

void read_command(char *argv[], int argc, int &file_position,
                  uint16_t &port, timeout_t &timeout) {
    ENSURE(!((argc - 1) & 1)); // There is even number of args
    port = DEFAULT_PORT;
    timeout = DEFAULT_TIMEOUT;
    for (int i = 1; i < argc; i += 2) {
        ENSURE(is_flag(argv[i]));
        // flags are placed on odd positions
        if (strcmp(argv[i], "-f") == 0) {
            ENSURE(!is_flag(argv[i + 1]));
            file_position = i + 1;
        }
        else if (strcmp(argv[i], "-p") == 0) {
            port = read_port(argv[i + 1]);
        }
        else {
            timeout = read_timeout(argv[i + 1]);
        }
    }
}

int main(int argc, char *argv[]) {
    srand(time(nullptr));
    if (argc <= 2) {
        fatal("usage: %s -f <file> -p <port> -t <expiration_time>", argv[0]);
    }

    int file_position;
    uint16_t port;
    timeout_t timeout;
    read_command(argv, argc, file_position, port, timeout);

    ServerHandler server_handler(port);
    Events events(argv[file_position]);
    ReservationCentre reservations(timeout, events);
    CommunicatHandler handler(server_handler);

    while (true)
        handle_next_request(handler, reservations);
    return 0;
}