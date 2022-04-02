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

#define GET_EVENTS 1
#define EVENTS 2
#define GET_RESERVATION 3
#define RESERVATION 4
#define GET_TICKETS 5
#define TICKETS 6
#define BAD_REQUEST 255

char shared_buffer[BUFFER_SIZE];
char input_buffer[BUFFER_SIZE];


typedef struct Event {
    unsigned short line_length;
    char description[FILE_LINE_LENGTH];
    uint16_t tickets;
} event_t;

unsigned long read_number(char *string) {
    errno = 0;
    unsigned long number = strtoul(string, NULL, 10);
    PRINT_ERRNO();
    return number;
}

uint16_t read_port(char *string) {
    uint16_t port = read_number(string);
    if (port > UINT16_MAX) {
        fatal("%ul is not a valid port number", port);
    }

    return port;
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
    int lines_count = 0;
    while ((ch = (char)fgetc(f)) != EOF) {
        if (ch == '\n')
            lines_count++;
    }

    fclose(f);
    return lines_count;
}

void load_events(event_t *events, int n, char* file_path) {
    FILE* f = open_file(file_path);
    for (int i = 0; i < n; i++) {
        unsigned short it = 0;

        for (char c = (char)fgetc(f); c != EOF && c != '\n'; c = (char)fgetc(f))
            events[i].description[it++] = c;
        events[i].line_length = it;
        // File is correct
        fscanf(f, "%hu", &events[i].tickets); // NOLINT(cert-err34-c)
    }

    fclose(f);
}

uint8_t read_message_id(char* string) {
    unsigned long number = read_number(string);
    if (number >= 0 && number <= UINT8_MAX) {
        return (uint8_t)number;
    }

    return 0;
}

void handle_next_request(int socket_fd) {
    struct sockaddr_in client_address;
    size_t read_length = read_message(socket_fd,
                               &client_address,
                               shared_buffer,
                               sizeof(shared_buffer));
    uint8_t message_id;
    FILE* client_message_stream = fmemopen(shared_buffer,
                                           strlen(shared_buffer), "r");

    fscanf(client_message_stream, "%s", input_buffer);
    message_id = read_message_id(input_buffer);
    switch (message_id) {
        case GET_EVENTS:

            break;
        case GET_RESERVATION:
            break;
        case GET_TICKETS:
            break;
    }

}

int main(int argc, char *argv[]) {
    if (argc < 2 || argc > 6) {
        fatal("usage: %s -f <file> -p <port> -t <timeout>", argv[0]);
    }

    check_correctness(argv, argc);
    int file_position = get_file(argv, argc);
    uint16_t port = get_port(argv, argc);
    uint32_t timeout = get_timeout(argv, argc);

    int number_of_events = count_lines(argv[file_position]) >> 1;

    event_t* events = malloc(sizeof(event_t) * number_of_events);

    load_events(events, number_of_events, argv[file_position]);

    memset(shared_buffer, 0, sizeof(shared_buffer));

    int socket_fd = bind_socket(port);


    do {
        handle_next_request(socket_fd);
    } while (true);
    printf("finished exchange\n");

    CHECK_ERRNO(close(socket_fd));

    free(events);
    return 0;
}
