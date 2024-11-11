#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include <getopt.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <ctype.h>

#define MAX_BUFFER 2048
#define DEFAULT_PORT 80

// stats to keep track of the server
int request_count = 0;
int bytes_received = 0;
int bytes_sent = 0;
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

// Delclare all of the functions
void serve_static(int client_fd, const char *filepath);
void serve_stats(int client_fd);
void serve_calc(int client_fd, const char *query);
void *handle_client(void *arg);
int setup_server(int port);
void send_response(int client_fd, const char *header, const char *content_type, const char *body);
void parse_request(const char *request, char *method, char *url);
void url_decode(char *dst, const char *src);

int main(int argc, char **argv)
{
    int option, port = DEFAULT_PORT;
    while ((option = getopt(argc, argv, "p:")) != -1)
    {
        switch (option)
        {
        case 'p':
            port = atoi(optarg);
            break;
        default:
            fprintf(stderr, "Usage: %s [-p port]\n", argv[0]);
            exit(EXIT_FAILURE);
        }
    }

    int server_fd = setup_server(port);
    printf("Server listening on port %d\n", port);

    while (1)
    {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0)
        {
            perror("accept");
            continue;
        }

        pthread_t thread_id;
        if (pthread_create(&thread_id, NULL, handle_client, (void *)(intptr_t)client_fd) != 0)
        {
            perror("pthread_create");
            close(client_fd);
        }
        else
        {
            pthread_detach(thread_id);
        }
    }

    close(server_fd);
    return 0;
}

void *handle_client(void *arg)
{
    int client_fd = (intptr_t)arg;
    char buffer[MAX_BUFFER], method[10], url[1024];
    int read_len = read(client_fd, buffer, MAX_BUFFER - 1);

    if (read_len > 0)
    {
        buffer[read_len] = '\0';

        pthread_mutex_lock(&lock);
        bytes_received += strlen(buffer);
        request_count++;
        pthread_mutex_unlock(&lock);

        parse_request(buffer, method, url);

        if (strncmp(url, "/static/", 8) == 0)
        {
            serve_static(client_fd, url + 7);
        }
        else if (strcmp(url, "/stats") == 0)
        {
            serve_stats(client_fd);
        }
        else if (strncmp(url, "/calc", 5) == 0)
        {
            serve_calc(client_fd, url);
        }
        else
        {
            send_response(client_fd, "HTTP/1.1 404 Not Found", "text/html", "<html><body><h1>404 Not Found</h1></body></html>");
        }
    }

    close(client_fd);
    return NULL;
}

void parse_request(const char *request, char *method, char *url)
{
    sscanf(request, "%s %s", method, url);
}

void serve_static(int client_fd, const char *filepath)
{
    char full_path[1024];
    snprintf(full_path, sizeof(full_path), ".%s", filepath);
    int file_fd = open(full_path, O_RDONLY);
    struct stat stat_buf;

    if (file_fd < 0 || fstat(file_fd, &stat_buf) < 0)
    {
        send_response(client_fd, "HTTP/1.1 404 Not Found", "text/html", "<html><body><h1>File Not Found</h1></body></html>");
    }
    else
    {
        char *file_buffer = malloc(stat_buf.st_size);
        read(file_fd, file_buffer, stat_buf.st_size);
        char header[1024];
        sprintf(header, "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: %ld\r\n\r\n", stat_buf.st_size);
        write(client_fd, header, strlen(header));
        write(client_fd, file_buffer, stat_buf.st_size);

        pthread_mutex_lock(&lock);
        bytes_sent += strlen(header) + stat_buf.st_size;
        pthread_mutex_unlock(&lock);

        free(file_buffer);
    }
    close(file_fd);
}

void serve_stats(int client_fd)
{
    char body[1024];
    snprintf(body, sizeof(body), "<html><body><h1>Server Stats</h1><p>Requests: %d</p><p>Bytes Received: %d</p><p>Bytes Sent: %d</p></body></html>", request_count, bytes_received, bytes_sent);
    send_response(client_fd, "HTTP/1.1 200 OK", "text/html", body);
}

void serve_calc(int client_fd, const char *query)
{
    char *param = strchr(query, '?');
    int a = 0, b = 0;

    if (param)
    {
        sscanf(param, "?a=%d&b=%d", &a, &b);
    }

    int result = a + b;
    char body[1024];
    snprintf(body, sizeof(body), "<html><body><h1>Calculation Result</h1><p>%d + %d = %d</p></body></html>", a, b, result);
    send_response(client_fd, "HTTP/1.1 200 OK", "text/html", body);
}

void send_response(int client_fd, const char *header, const char *content_type, const char *body)
{
    char response[MAX_BUFFER];
    int response_length = snprintf(response, MAX_BUFFER, "%s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n\r\n%s", header, content_type, strlen(body), body);
    write(client_fd, response, response_length);

    pthread_mutex_lock(&lock);
    bytes_sent += response_length;
    pthread_mutex_unlock(&lock);
}

int setup_server(int port)
{
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1)
    {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        perror("bind");
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 10) < 0)
    {
        perror("listen");
        exit(EXIT_FAILURE);
    }

    return server_fd;
}

void url_decode(char *dst, const char *src)
{
    char a, b;
    while (*src)
    {
        if ((*src == '%') &&
            ((a = src[1]) && (b = src[2])) &&
            (isxdigit(a) && isxdigit(b)))
        {
            if (a >= 'a')
                a -= 'a' - 'A';
            if (a >= 'A')
                a -= ('A' - 10);
            else
                a -= '0';
            if (b >= 'a')
                b -= 'a' - 'A';
            if (b >= 'A')
                b -= ('A' - 10);
            else
                b -= '0';
            *dst++ = 16 * a + b;
            src += 3;
        }
        else if (*src == '+')
        {
            *dst++ = ' ';
            src++;
        }
        else
        {
            *dst++ = *src++;
        }
    }
    *dst++ = '\0';
}
