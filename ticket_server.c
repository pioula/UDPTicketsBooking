#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <assert.h>
#include "err.h"

#define BUFFER_SIZE 65507
#define COMMAND_LINE_LENGTH 4096
#define WRONG_USAGE "wrong usage"
#define MALLOC_ERROR "malloc error"

#define DEFAULT_PORT 2022

#define TIMEOUT_MAX 86400
#define DEFAULT_TIMEOUT 5

#define FILE_LINE_LENGTH 80

#define INIT_NUMBER_OF_RESERVATIONS 32

#define EMPTY_COMMAND 0
#define GET_EVENTS 1
#define EVENTS 2
#define GET_RESERVATION 3
#define RESERVATION 4
#define GET_TICKETS 5
#define TICKETS 6
#define BAD_REQUEST 255

char shared_buffer[BUFFER_SIZE];
char input_buffer[BUFFER_SIZE];

void* my_malloc(size_t m) {
    void* memblock = malloc(m);
    if (!memblock) fatal(MALLOC_ERROR);
    return memblock;
}

void my_realloc(void* memblock, size_t m) {
    memblock = realloc(memblock, m);
    if (!memblock) fatal(MALLOC_ERROR);
}

typedef struct reservation {
    uint16_t tickets;
    uint32_t event_id;
    char cookie[48];
} reservation_t;

struct reservation_container_s {
    uint32_t* free_reservations;
    reservation_t* reservations;
    size_t reservations_size;
    uint32_t free_reservations_n;
} reservation_container;

void init_reservations() {
    reservation_container.free_reservations =
            my_malloc(sizeof(uint32_t) * INIT_NUMBER_OF_RESERVATIONS);
    reservation_container.reservations =
            my_malloc(sizeof(reservation_t) * INIT_NUMBER_OF_RESERVATIONS);
    reservation_container.reservations_size = INIT_NUMBER_OF_RESERVATIONS;
    reservation_container.free_reservations_n = INIT_NUMBER_OF_RESERVATIONS;
    for (size_t i = 0; i < INIT_NUMBER_OF_RESERVATIONS; i++) {
        reservation_container.free_reservations[i] = i;
    }
}

void free_reservation_container() {
    free(reservation_container.free_reservations);
    free(reservation_container.reservations);
}

uint32_t get_new_reservation_id() {
    if (reservation_container.free_reservations_n == 0) {
        size_t prev_size = reservation_container.reservations_size;
        reservation_container.reservations_size <<= 1;
        my_realloc(reservation_container.free_reservations,
                   reservation_container.reservations_size);
        my_realloc(reservation_container.reservations,
                   reservation_container.reservations_size);
        for (size_t i = prev_size;
            i < reservation_container.reservations_size; i++) {
            reservation_container.free_reservations[i - prev_size] = i;
        }
        reservation_container.free_reservations_n = prev_size;
    }

    return reservation_container
        .free_reservations[--reservation_container.free_reservations_n];
}

void push_reservation_id(uint32_t id) {
    reservation_container
        .free_reservations[reservation_container.free_reservations_n++] = id;
}

uint32_t number_of_free_reservations() {
    return reservation_container.free_reservations_n;
}

typedef struct event {
    uint16_t line_length;
    char description[FILE_LINE_LENGTH];
    uint16_t tickets;
} event_t;

typedef struct event_container {
    uint32_t n;
    event_t* list;
} event_container_t;

unsigned long read_number(char *string) {
    errno = 0;
    unsigned long number = strtoul(string, NULL, 10);
    PRINT_ERRNO();
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

FILE* open_file(char *path) {
    FILE* tmp = fopen(path, "r");
    if (!tmp) {
        fclose(tmp);
        fatal("Error while loading file");
    }

    return tmp;
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
    CHECK_ERRNO(bind(socket_fd, (struct sockaddr *) &server_address,
                        (socklen_t) sizeof(server_address)));

    return socket_fd;
}

size_t read_message(int socket_fd, struct sockaddr_in *client_address, char *buffer, size_t max_length) {
    socklen_t address_length = (socklen_t) sizeof(*client_address);
    int flags = 0; // we do not request anything special
    errno = 0;
    ssize_t len = recvfrom(socket_fd, buffer, max_length, flags,
                           (struct sockaddr *) client_address, &address_length);
    if (len < 0) {
        PRINT_ERRNO();
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

int count_lines(char* path) {
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

void load_events(event_container_t events, char* file_path) {
    FILE* f = open_file(file_path);
    for (uint32_t i = 0; i < events.n; i++) {
        unsigned short it = 0;
        memset(events.list[i].description, 0, sizeof(events.list[i].description));
        for (char c = (char)fgetc(f); c != EOF && c != '\n'; c = (char)fgetc(f))
            events.list[i].description[it++] = c;
        events.list[i].line_length = it;

        // File is correct
        fscanf(f, "%hu ", &events.list[i].tickets); // NOLINT(cert-err34-c)
    }

    fclose(f);
}

uint16_t read_message_id(char* string) {
    unsigned long number = read_number(string);
    if (number <= UINT8_MAX) {
        return (uint16_t)number;
    }

    return EMPTY_COMMAND;
}

void send_events(int socket_fd,
                 struct sockaddr_in client_address,
                 event_container_t events) {
    char* new_event = my_malloc(sizeof(char) * 2 * FILE_LINE_LENGTH);
    memset(shared_buffer, 0, sizeof(shared_buffer));
    int number_of_bytes = 0;

    for (uint32_t i = 0; i < events.n; i++) {
        memset(new_event, 0, 2 * FILE_LINE_LENGTH);
        uint16_t new_event_length = sprintf(new_event, "%u", EVENTS);
        new_event_length += sprintf(new_event + new_event_length, " %hu",
                                   events.list[i].tickets);
        new_event_length += sprintf(new_event + new_event_length, " %hu",
                                   events.list[i].line_length);
        new_event_length += sprintf(new_event + new_event_length, " %s\n",
                                   events.list[i].description);

        if (new_event_length + number_of_bytes > BUFFER_SIZE)
            break;

        printf("%s", new_event);

        memcpy(&shared_buffer[number_of_bytes], new_event, new_event_length);
        number_of_bytes += new_event_length;
    }

    send_message(socket_fd, &client_address,
                 shared_buffer, strlen(shared_buffer));
    free(new_event);
}

void make_reservation(int socket_fd, struct sockaddr_in client_address,
        event_container_t events, FILE* stream) {
    uint32_t event_id;
    int16_t tickets_count;
    fscanf(stream, "%u%hu", &event_id, &tickets_count);

    if (event_id >= events.n || events.list[event_id].tickets < tickets_count ||
        tickets_count == 0) {
        // TODO BAD REQUEST
    }



}


void handle_next_request(int socket_fd, event_container_t events,
                         uint32_t number_of_events) {
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
            send_events(socket_fd, client_address, events);
            break;
        case GET_RESERVATION:
            make_reservation(socket_fd, client_address,
                             events, number_of_events, client_message_stream);
            break;
        case GET_TICKETS:
            break;
    }

    fclose(client_message_stream);
}

int main(int argc, char *argv[]) {
    if (argc < 2 || argc > 6) {
        fatal("usage: %s -f <file> -p <port> -t <timeout>", argv[0]);
    }

    check_correctness(argv, argc);
    int file_position = get_file(argv, argc);
    uint16_t port = get_port(argv, argc);
    uint32_t timeout = get_timeout(argv, argc);

    uint32_t number_of_events = count_lines(argv[file_position]) >> 1;

    event_t* events_list = my_malloc(sizeof(event_t) * number_of_events);

    event_container_t events;
    events.list = events_list;
    events.n = number_of_events;
    load_events(events, argv[file_position]);

    init_reservations();

    memset(shared_buffer, 0, sizeof(shared_buffer));

    int socket_fd = bind_socket(port);

    do {
        handle_next_request(socket_fd, events, number_of_events);
    } while (true);
    printf("finished exchange\n");

    CHECK_ERRNO(close(socket_fd));

    free_reservation_container();
    free(events.list);
    return 0;
}
