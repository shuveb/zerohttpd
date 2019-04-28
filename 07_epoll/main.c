#define _GNU_SOURCE /* For asprintf() */
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <ctype.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/sendfile.h>
#include <time.h>
#include <locale.h>
#include <sys/ioctl.h>
#include <sys/epoll.h>
#include <stdnoreturn.h>
#include <asm/errno.h>
#include <errno.h>
#include "uthash.h"

#define SERVER_STRING               "Server: zerohttpd/0.1\r\n"
#define DEFAULT_SERVER_PORT         8000
#define REDIS_SERVER_HOST           "127.0.0.1"
#define REDIS_SERVER_PORT           6379

#define METHOD_HANDLED              0
#define METHOD_NOT_HANDLED          1
#define GUESTBOOK_ROUTE             "/guestbook"
#define GUESTBOOK_TEMPLATE          "templates/guestbook/index.html"
#define GUESTBOOK_REDIS_VISITOR_KEY "visitor_count"
#define GUESTBOOK_REDIS_REMARKS_KEY "guestbook_remarks"
#define GUESTBOOK_TMPL_VISITOR      "$VISITOR_COUNT$"
#define GUESTBOOK_TMPL_REMARKS      "$GUEST_REMARKS$"

const char *unimplemented_content = \
        "HTTP/1.0 400 Bad Request\r\n"
        "Content-type: text/html\r\n"
        "\r\n"
        "<html>"
        "<head>"
        "<title>ZeroHTTPd: Unimplemented</title>"
        "</head>"
        "<body>"
        "<h1>Bad Request (Unimplemented)</h1>"
        "<p>Your client sent a request ZeroHTTPd did not understand and it is probably not your fault.</p>"
        "</body>"
        "</html>";

const char *http_404_content = \
        "HTTP/1.0 404 Not Found\r\n"
        "Content-type: text/html\r\n"
        "\r\n"
        "<html>"
        "<head>"
        "<title>ZeroHTTPd: Not Found</title>"
        "</head>"
        "<body>"
        "<h1>Not Found (404)</h1>"
        "<p>Your client is asking for an object that was not found on this server.</p>"
        "</body>"
        "</html>";

char    redis_host_ip[32];

/*
 * This whole program runs in a single thread and there are
 * many reads and writes that need to be tracked per client
 * connection. The client_context represents a single client
 * request and it maintains all that is required to fulfill
 * a single request. A new client_context is allocated for
 * every new client request and is freed once the request
 * has been processed.
 * */
typedef struct client_context{
    int client_sock;
    int redis_sock;
    char templ[16384];      /* Template storage */
    char rendering[16384];

    /* Next callback to make to render the page */
    void (*next_callback)(struct client_context *cc);

    /* Used by Redis reader/writer callback functions */
    void (*read_write_callback)(struct client_context *cc);

    int entries_count;
    char **entries;
    char redis_key_buffer[64];
    int intval;

    /* Hash tables handles to look up by Redis or by client socket*/
    UT_hash_handle hh_cs;   /* Lookup by client socket */
    UT_hash_handle hh_rs;   /* Lookup by Redis socket  */
} client_context;

int epoll_fd;
#define MAX_EVENTS 32000
struct epoll_event events[MAX_EVENTS];

/*
 * Hash tables that allow us to look up the client_context
 * given the client socket or the Redis socket respectively.
 * */
client_context *cc_cs_hash = NULL;
client_context *cc_rs_hash = NULL;

/*
 One function that prints the system call and the error details
 and then exits with error code 1. Non-zero meaning things didn't go well.
 */
void fatal_error(const char *syscall)
{
    perror(syscall);
    exit(1);
}

void add_to_epoll_fd_list(int fd, uint32_t ep_events) {
    struct epoll_event event;
    event.data.fd = fd;
    event.events = ep_events;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event))
        fatal_error("add epoll_ctl()");

}

void remove_from_epoll_fd_list(int fd) {
    if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL))
        fatal_error("remove epoll_ctl()");

}

/*
 * Redis uses a long-lived connection. Many commands are sent on and
 * responded to over the same connection. In this function, we connect
 * to the Redis server and return the connected socket.
 *
 * This is the synchronous version of the connect_to_redis() function.
 * */

int connect_to_redis_server_sync() {
    struct sockaddr_in redis_servaddr;
    int redis_socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    memset(&redis_servaddr, 0, sizeof(redis_servaddr));
    redis_servaddr.sin_family = AF_INET;
    redis_servaddr.sin_port = htons(REDIS_SERVER_PORT);

    int pton_ret = inet_pton(AF_INET, redis_host_ip, &redis_servaddr.sin_addr);

    if (pton_ret < 0)
        fatal_error("inet_pton()");
    else if (pton_ret == 0) {
        fprintf(stderr, "Error: Please provide a valid Redis server IP address.\n");
        exit(1);
    }

    int cret = connect(redis_socket_fd,
                        (struct sockaddr *) &redis_servaddr,
                        sizeof(redis_servaddr));
    if (cret == -1)
        fatal_error("redis connect()");

    return redis_socket_fd;
}

/*
 * This callback is called from the main event loop driven by epoll_wait()
 * once a connection to the Redis server is established. See the
 * connect_to_redis_server() function below. In this function, we
 * turn the Redis connected socket back into a blocking socket.
 *
 * For us blocking sockets are what we need when we need to do reads
 * and writes. The epoll_wait() system call ensures that we do not block.
 * The only reason we setup a non-blocking socket in the first place
 * is because we want to avoid connect() from blocking.
 *
 * */


void connect_to_redis_server_callback(client_context *cc) {

    int result;
    socklen_t result_len = sizeof(result);
    if (getsockopt(cc->redis_sock, SOL_SOCKET, SO_ERROR, &result, &result_len) < 0) {
        fatal_error("Redis connect getsockopt()");
    }

    if (result != 0) {
        fatal_error("Redis connect getsockopt() result");
    }

    remove_from_epoll_fd_list(cc->redis_sock);

    /* Make the redis socket blocking */
    fcntl(cc->redis_sock, F_SETFL,
          fcntl(cc->redis_sock, F_GETFL) &
          ~O_NONBLOCK);

    add_to_epoll_fd_list(cc->redis_sock, EPOLLIN);
    cc->next_callback(cc);
}

/*
 * This is the async version of the connect_to_redis_sync() function
 * implemented earlier. Here, we first setup a non-blocking socket and
 * use connect() to connect to the Redis server. It is important to
 * understand that connect() can also take a long time and during
 * that time, as we block, everything comes to a halt. But not if we
 * use a non-blocking socket like we're using here. We call connect()
 * but that does not block, it returns -1, but sets sets errno to
 * EINPROGRESS. We then add this connecting socket to the descriptors
 * that epoll_wait() is monitoring.
 *
 * When the connection is made, epoll_wait() wakes up and calls the callback
 * we've carefully saved in the client_context, which is the
 * connect_to_redis_server_callback() implemented above.
 * */


void connect_to_redis_server(client_context *cc) {
    struct sockaddr_in redis_servaddr;
    /* Create a non-blocking socket */
    int redis_socket_fd = socket(AF_INET, SOCK_STREAM|SOCK_NONBLOCK, 0);
    memset(&redis_servaddr, 0, sizeof(redis_servaddr));
    redis_servaddr.sin_family = AF_INET;
    redis_servaddr.sin_port = htons(REDIS_SERVER_PORT);

    int pton_ret = inet_pton(AF_INET, redis_host_ip, &redis_servaddr.sin_addr);

    if (pton_ret < 0)
        fatal_error("inet_pton()");
    else if (pton_ret == 0) {
        fprintf(stderr, "Error: Please provide a valid Redis server IP address.\n");
        exit(1);
    }

    int cret = connect(redis_socket_fd,
                       (struct sockaddr *) &redis_servaddr,
                       sizeof(redis_servaddr));
    if (cret == -1 && errno != EINPROGRESS) {
        fatal_error("redis connect()");
    } else {
        cc->redis_sock = redis_socket_fd;
        cc->read_write_callback = connect_to_redis_server_callback;
        add_to_epoll_fd_list(redis_socket_fd, EPOLLOUT);
    }
}
/*
 * One note about writes: we never check if we will block. We simply
 * write. We mainly worry about the time we might spend blocked on
 * connect() and read(). Both these, we track with epoll_wait()
 * */
ssize_t redis_write(int fd, char *buf, ssize_t length) {
    ssize_t n_written = write(fd, buf, length);
    if (n_written < 0)
        fatal_error("Redis write()");

    return n_written;
}

/*
 * We are expecting to read and fetch an integer from Redis. This callback
 * is made when we can read without blocking on the Redis socket.
 * */

void redis_get_int_key_callback(client_context *cc) {
    /*
     * Example response from server:
     * $3\r\n385\r\n
     * This means that the server is telling us to expect a string
     * of 3 characters.
     */

    /* memset needed only if we're printing the read buffer */
    //memset(cc->redis_key_buffer, 0, sizeof(cc->redis_key_buffer));

    int ret = read(cc->redis_sock, cc->redis_key_buffer, sizeof(cc->redis_key_buffer));
    if (ret < 0) fatal_error("Redis read()");
    char *p = cc->redis_key_buffer;
    if (*p != '$') {
        printf("Redis protocol error. Wrong number format.\n");
        return;
    }

    /* Loop till we reach the actual number string */
    while(*p++ != '\n');

    /* The following loop converts a string representation of a number to a number */
    int intval = 0;
    while (*p != '\r') {
        intval = (intval * 10) + (*p - '0');
        p++;
    }
    cc->intval = intval;
    cc->next_callback(cc);
}

/*
 * Initiate reading a key with an int value from Redis. See the
 * redis_get_int_key_callback() function above. That gets called from
 * the event loop once we can read the Redis socket without blocking.
 * */

void redis_get_int_key(const char *key, client_context *cc) {
    char *req_buffer;
    asprintf(&req_buffer, "*2\r\n$3\r\nGET\r\n$%ld\r\n%s\r\n", strlen(key), key);
    redis_write(cc->redis_sock, req_buffer, strlen(req_buffer));
    free(req_buffer);

    cc->read_write_callback = redis_get_int_key_callback;
}

void redis_incr_by_callback(client_context *cc) {
    /*
     * Redis returns the new increased count. If this is not read here,
     * it interferes with future reads.
     * */
    char cmd_buf[1024] = "";
    ssize_t r_ret = read(cc->redis_sock, cmd_buf, sizeof(cmd_buf));
    if (r_ret < 0)
        fatal_error("Redis incr_cb read()");
    cc->next_callback(cc);
}

/* Increment given key by incr_by. Key is created by Redis if it doesn't exist. */
void redis_incr_by(char *key, int incr_by, client_context *cc) {
    char cmd_buf[1024] = "";
    char incr_by_str[16] = "";
    sprintf(incr_by_str, "%d", incr_by);
    sprintf(cmd_buf, "*3\r\n$6\r\nINCRBY\r\n$%ld\r\n%s\r\n$%ld\r\n%d\r\n", strlen(key), key, strlen(incr_by_str), incr_by);
    redis_write(cc->redis_sock, cmd_buf, strlen(cmd_buf));
    cc->read_write_callback = redis_incr_by_callback;
}

/* Implies increment by one. Useful addition since this is a common use case. */
void redis_incr(char *key, client_context *cc) {
    redis_incr_by(key, 1, cc);
}

/*
 * Appends an item pointed to by 'value' to the list in redis referred by 'key'.
 * Uses the Redis RPUSH command to get the job done.
 *
 * This call is made rarely and so we read synchronously.
 * */

int redis_list_append(char *key, char *value, int redis_socket_fd) {
    char cmd_buf[1024] = "";
    sprintf(cmd_buf, "*3\r\n$5\r\nRPUSH\r\n$%ld\r\n%s\r\n$%ld\r\n%s\r\n", strlen(key), key, strlen(value), value);
    redis_write(redis_socket_fd, cmd_buf, strlen(cmd_buf));
    memset(cmd_buf, 0, sizeof(cmd_buf));
    read(redis_socket_fd, cmd_buf, sizeof(cmd_buf));
    return 0;
}

void redis_list_get_range_callback(client_context *cc) {
    /*
     *  The Redis protocol is elegantly simple. The following is a response for an array
     *  that has 3 elements (strings):
     *  Example response:
     *  *3\r\n$5\r\nHello\r\n$6\r\nLovely\r\n\$5\r\nWorld\r\n
     *
     *  What it means:
     *  *3      -> Array with 3 items
     *  $5      -> string with 5 characters
     *  Hello   -> actual string
     *  $6      -> string with 6 characters
     *  Lovely  -> actual string
     *  $5      -> string with 5 characters
     *  World   -> actual string
     *
     *  A '\r\n' (carriage return + line feed) sequence is used as the delimiter.
     *  Now, you should be able to understand why we're doing what we're doing in this function
     * */

    /* Find out the length of the array */
    char ch;
    read(cc->redis_sock, &ch, 1);
    if (ch != '*')
        return;

    int returned_items = 0;
    while (1) {
        read(cc->redis_sock, &ch, 1);
        if (ch == '\r') {
            /* Read the next '\n' character */
            read(cc->redis_sock, &ch, 1);
            break;
        }
        returned_items = (returned_items * 10) + (ch - '0');
    }
    /* Allocate the array that will hold a pointer each for
     * every element in the returned list */
    cc->entries_count = returned_items;
    char **items_holder = malloc(sizeof(char *) * returned_items);
    cc->entries = items_holder;

    /*
     * We now know the length of the array. Let's loop that many iterations
     * and grab those strings, allocating a new chunk of memory for each one.
     * */
    for (int i = 0; i < returned_items; i++) {
        /* read the first '$' */
        read(cc->redis_sock, &ch, 1);
        int str_size = 0;
        while (1) {
            read(cc->redis_sock, &ch, 1);
            if (ch == '\r') {
                /* Read the next '\n' character */
                read(cc->redis_sock, &ch, 1);
                break;
            }
            str_size = (str_size * 10) + (ch - '0');
        }
        char *str = malloc(sizeof(char) * str_size + 1);
        items_holder[i] = str;
        read(cc->redis_sock, str, str_size);
        str[str_size] = '\0';
        /* Read the '\r\n' chars */
        read(cc->redis_sock, &ch, 1);
        read(cc->redis_sock, &ch, 1);
    }

    cc->next_callback(cc);
}

/*
 * Get the range of items in a list from 'start' to 'end'.
 * This function allocates memory. An array of pointers and all the strings pointed to by it
 * are dynamically allocated. redis_free_array_results() has to be called to free this
 * allocated memory by the caller.
 * */

void redis_list_get_range(char *key, int start, int end, client_context *cc) {
    /* Shortcut: setting a character array to a null string will fill it up with zeros */
    char cmd_buf[1024]="", start_str[16], end_str[16];
    sprintf(start_str, "%d", start);
    sprintf(end_str, "%d", end);
    sprintf(cmd_buf, "*4\r\n$6\r\nLRANGE\r\n$%ld\r\n%s\r\n$%ld\r\n%s\r\n$%ld\r\n%s\r\n", strlen(key), key, strlen(start_str), start_str, strlen(end_str), end_str);
    redis_write(cc->redis_sock, cmd_buf, strlen(cmd_buf));
    cc->read_write_callback = redis_list_get_range_callback;
}

/*
 * Utility function to get the whole list.
 * We set start to 0 and end to -1, which is a magic number that means the last element.
 * */

void redis_get_list(char *key, client_context *cc) {
    redis_list_get_range(key, 0, -1, cc);
}

/*
 * redis_get_list() and redis_get_list_range() return a dynamically allocated result.
 * This is a utility function to free all strings and also the array of pointers that
 * holds pointers to those strings.
 * */

void redis_free_array_result(char **items, int length) {
    for (int i = 0; i < length; i++) {
        free(items[i]);
    }
    free(items);
}

/*
 * This function is responsible for setting up the main listening socket used by the
 * web server.
 * */

int setup_listening_socket(int port) {
    int sock;
    struct sockaddr_in srv_addr;

    /* Create a non-blocking socket */
    sock = socket(PF_INET, SOCK_STREAM|SOCK_NONBLOCK, 0);
    if (sock == -1)
        fatal_error("socket()");

    int enable = 1;
    if (setsockopt(sock,
                   SOL_SOCKET, SO_REUSEADDR,
                   &enable, sizeof(int)) < 0)
        fatal_error("setsockopt(SO_REUSEADDR)");


    memset(&srv_addr, 0, sizeof(srv_addr));
    srv_addr.sin_family = AF_INET;
    srv_addr.sin_port = htons(port);
    srv_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    /* We bind to a port and turn this socket into a listening
     * socket.
     * */
    if (bind(sock,
             (const struct sockaddr *)&srv_addr,
             sizeof(srv_addr)) < 0)
        fatal_error("bind()");

    if (listen(sock, 10) < 0)
        fatal_error("listen()");

    return (sock);
}

/*
 * Reads the client socket character-by-character and once a line is read, the string
 * is NULL-terminated and the function returns the characters read.
 * This is not a very efficient way of doing things since we are making multiple calls
 * to read(), but it keeps things simple.
 * */

int get_line(int sock, char *buf, int size) {
    int i = 0;
    char c = '\0';
    ssize_t n;

    while ((i < size - 1) && (c != '\n'))
    {
        n = recv(sock, &c, 1, 0);
        if (n > 0)
        {
            if (c == '\r')
            {
                n = recv(sock, &c, 1, MSG_PEEK);
                if ((n > 0) && (c == '\n'))
                    recv(sock, &c, 1, 0);
            }
            else
            {
                buf[i] = c;
                i++;
            }
        }
        else
            return 0;
    }
    buf[i] = '\0';

    return (i);
}

/*
 * Utility function to convert a string to lower case.
 * */

void strtolower(char *str) {
    for (; *str; ++str)
        *str = (char)tolower(*str);
}

/*
 * When ZeroHTTPd encounters any other HTTP method other than GET or POST, this function
 * is used to inform the client.
 * */

void handle_unimplemented_method(int client_socket) {
    send(client_socket, unimplemented_content, strlen(unimplemented_content), 0);
}

/*
 * This function is used to send a "HTTP Not Found" code and message to the client in
 * case the file requested is not found.
 * */

void handle_http_404(int client_socket) {
    send(client_socket, http_404_content, strlen(http_404_content), 0);
}

/*
 * Once a static file is identified to be served, this function is used to read the file
 * and write it over the client socket using Linux's sendfile() system call. This saves us
 * the hassle of transferring file buffers from kernel to user space and back.
 *
 * Special note on asychronous file access: under Linux, poll()/epoll claim regular
 * files are ready for reads or writes at all times. There is no point in adding them to the
 * descriptors monitored by epoll_wait(). Libraries like libuv emulate regular file readiness
 * monitoring by accesssing them over a separate "I/O" thread. So, we just read.
 * */

void transfer_file_contents(char *file_path, int client_socket, off_t file_size) {
    int fd;

    fd = open(file_path, O_RDONLY);
    sendfile(client_socket, fd, NULL, file_size);
    close(fd);
}

/*
 * Simple function to get the file extension of the file that we are about to serve.
 * */

const char *get_filename_ext(const char *filename) {
    const char *dot = strrchr(filename, '.');
    if (!dot || dot == filename)
        return "";
    return dot + 1;
}

/* Converts a hex character to its integer value */
char from_hex(char ch) {
    return isdigit(ch) ? ch - '0' : tolower(ch) - 'a' + 10;
}

/*
 * HTML URLs and other data like data sent over the POST method encoded using a simple
 * scheme. This function takes the encoded data and returns a regular, decoded string.
 * e.g:
 * Encoded: Nothing+is+better+than+bread+%26+butter%21
 * Decoded: Nothing is better than bread & butter!
 * */
char *urlencoding_decode(char *str) {
    char *pstr = str, *buf = malloc(strlen(str) + 1), *pbuf = buf;
    while (*pstr) {
        if (*pstr == '%') {
            if (pstr[1] && pstr[2]) {
                *pbuf++ = from_hex(pstr[1]) << 4 | from_hex(pstr[2]);
                pstr += 2;
            }
        } else if (*pstr == '+') {
            *pbuf++ = ' ';
        } else {
            *pbuf++ = *pstr;
        }
        pstr++;
    }
    *pbuf = '\0';
    return buf;
}

/*
 * Sends the HTTP 200 OK header, the server string, for a few types of files, it can also
 * send the content type based on the file extension. It also sends the content length
 * header. Finally it send a '\r\n' in a line by itself signalling the end of headers
 * and the beginning of any content.
 * */

void send_headers(const char *path, off_t len, int client_socket) {
    char small_case_path[1024];
    char send_buffer[1024];
    strcpy(small_case_path, path);
    strtolower(small_case_path);

    strcpy(send_buffer, "HTTP/1.0 200 OK\r\n");
    send(client_socket, send_buffer, strlen(send_buffer), 0);
    strcpy(send_buffer, SERVER_STRING);
    send(client_socket, send_buffer, strlen(send_buffer), 0);

    /*
     * Check the file extension for certain common types of files
     * on web pages and send the appropriate content-type header.
     * Since extensions can be mixed case like JPG, jpg or Jpg,
     * we turn the extension into lower case before checking.
     * */
    const char *file_ext = get_filename_ext(small_case_path);
    if (strcmp("jpg", file_ext) == 0)
        strcpy(send_buffer, "Content-Type: image/jpeg\r\n");
    if (strcmp("jpeg", file_ext) == 0)
        strcpy(send_buffer, "Content-Type: image/jpeg\r\n");
    if (strcmp("png", file_ext) == 0)
        strcpy(send_buffer, "Content-Type: image/png\r\n");
    if (strcmp("gif", file_ext) == 0)
        strcpy(send_buffer, "Content-Type: image/gif\r\n");
    if (strcmp("htm", file_ext) == 0)
        strcpy(send_buffer, "Content-Type: text/html\r\n");
    if (strcmp("html", file_ext) == 0)
        strcpy(send_buffer, "Content-Type: text/html\r\n");
    if (strcmp("js", file_ext) == 0)
        strcpy(send_buffer, "Content-Type: application/javascript\r\n");
    if (strcmp("css", file_ext) == 0)
        strcpy(send_buffer, "Content-Type: text/css\r\n");
    if (strcmp("txt", file_ext) == 0)
        strcpy(send_buffer, "Content-Type: text/plain\r\n");
    send(client_socket, send_buffer, strlen(send_buffer), 0);

    /* Send the content-length header, which is the file size in this case. */
    sprintf(send_buffer, "content-length: %ld\r\n", len);
    send(client_socket, send_buffer, strlen(send_buffer), 0);

    /*
     * When the browser sees a '\r\n' sequence in a line on its own,
     * it understands there are no more headers. Content may follow.
     * */
    strcpy(send_buffer, "\r\n");
    send(client_socket, send_buffer, strlen(send_buffer), 0);
}

/*
 * This is the last function called by the call chain and this is called when
 * we're done dealing with a client request. We remove the client and Redis
 * sockets from the list that epoll_wait() deals with and we also remove the
 * client_context entries from both the hash tables we're using. We close both
 * sockets and finally free the memory we allocated for the client_context.
 * */

void client_fini(client_context *cc) {
    client_context *check_cc;
    HASH_FIND(hh_cs, cc_cs_hash, &cc->client_sock, sizeof(cc->client_sock), check_cc);
    if (check_cc) {
        HASH_DELETE(hh_cs, cc_cs_hash, check_cc);
        HASH_DELETE(hh_rs, cc_rs_hash, check_cc);
        remove_from_epoll_fd_list(cc->client_sock);
        remove_from_epoll_fd_list(cc->redis_sock);
        close(cc->client_sock);
        close(cc->redis_sock);
        free(check_cc);
    }
    else {
        printf("Could not retrieve client context back from hash!\n");
    }
}

void write_template(client_context *cc) {
    /*
     * We've now rendered the template. Send headers and finally the
     * template over to the client.
     * */
    char send_buffer[1024];
    strcpy(send_buffer, "HTTP/1.0 200 OK\r\n");
    ssize_t s_ret = send(cc->client_sock, send_buffer, strlen(send_buffer), 0);
    if (s_ret < 0)
        fatal_error("send()");
    strcpy(send_buffer, SERVER_STRING);
    s_ret = send(cc->client_sock, send_buffer, strlen(send_buffer), 0);
    if (s_ret < 0)
        fatal_error("send()");
    strcpy(send_buffer, "Content-Type: text/html\r\n");
    s_ret = send(cc->client_sock, send_buffer, strlen(send_buffer), 0);
    if (s_ret < 0)
        fatal_error("send()");
    sprintf(send_buffer, "content-length: %ld\r\n", strlen(cc->templ));
    s_ret = send(cc->client_sock, send_buffer, strlen(send_buffer), 0);
    if (s_ret < 0)
        fatal_error("send()");
    strcpy(send_buffer, "\r\n");
    s_ret = send(cc->client_sock, send_buffer, strlen(send_buffer), 0);
    if (s_ret < 0)
        fatal_error("send()");
    s_ret = send(cc->client_sock, cc->templ, strlen(cc->templ), 0);
    if (s_ret < 0)
        fatal_error("send()");
    printf("200 GET /guestbook %ld bytes\n", strlen(cc->templ));

    client_fini(cc);
}

void render_visitor_count(client_context *cc) {
    char visitor_count_str[16]="";

    sprintf(visitor_count_str, "%'d", cc->intval);

    /* Replace visitor count */
    char *vcount = strstr(cc->templ, GUESTBOOK_TMPL_VISITOR);
    if (vcount) {
        memcpy(cc->rendering, cc->templ, vcount-cc->templ);
        strcat(cc->rendering, visitor_count_str);
        char *copy_offset = cc->templ + (vcount-cc->templ) + strlen(GUESTBOOK_TMPL_VISITOR);
        strcat(cc->rendering, copy_offset);
        strcpy(cc->templ, cc->rendering);
        memset(cc->rendering, 0, sizeof(cc->rendering));
    }

    write_template(cc);
}

void init_fetch_visitor_count(client_context *cc) {
    cc->next_callback = render_visitor_count;
    redis_get_int_key(GUESTBOOK_REDIS_VISITOR_KEY, cc);
}

void bump_visitor_count_callback(client_context *cc) {
    init_fetch_visitor_count(cc);
}

void bump_visitor_count(client_context *cc) {
    cc->next_callback = bump_visitor_count_callback;
    redis_incr(GUESTBOOK_REDIS_VISITOR_KEY, cc);
}

void render_guestbook_entries(client_context *cc) {
    char guest_entries_html[16384]="";

    for (int i = 0; i < cc->entries_count; i++) {
        char guest_entry[1024];
        sprintf(guest_entry, "<p class=\"guest-entry\">%s</p>", cc->entries[i]);
        strcat(guest_entries_html, guest_entry);
    }
    redis_free_array_result(cc->entries, cc->entries_count);

    /* Replace guestbook entries */
    char *entries = strstr(cc->templ, GUESTBOOK_TMPL_REMARKS);
    if (entries) {
        memcpy(cc->rendering, cc->templ, entries-cc->templ);
        strcat(cc->rendering, guest_entries_html);
        char *copy_offset = cc->templ + (entries-cc->templ) + strlen(GUESTBOOK_TMPL_REMARKS);
        strcat(cc->rendering, copy_offset);
        strcpy(cc->templ, cc->rendering);
        memset(cc->rendering, 0, sizeof(cc->rendering));
    }

    bump_visitor_count(cc);
}

/*
 * This function is called from the connect_to_redis_server_callback()
 * function once epoll_wait() detects that the new connection to Redis has
 * succeeded.
 * */

void render_guestbook_post_redis_connect(client_context *cc) {
    add_to_epoll_fd_list(cc->client_sock, EPOLLIN);
    memset(cc->rendering, 0, sizeof(cc->rendering));
    memset(cc->templ, 0, sizeof(cc->rendering));

    /* Read the template file */
    int fd = open(GUESTBOOK_TEMPLATE, O_RDONLY);
    if (fd == -1)
        fatal_error("Template read()");
    read(fd, cc->templ, sizeof(cc->templ));
    close(fd);

    /* Get guestbook entries and render them as HTML */
    cc->next_callback = render_guestbook_entries;
    redis_get_list( GUESTBOOK_REDIS_REMARKS_KEY, cc);
}

/*
 * The guest book template file is a normal HTML file except two special strings:
 * $GUEST_REMARKS$ and $VISITOR_COUNT$
 * In this method, both these special template variables are replaced by content
 * generated by us. And that content is based on stuff we retrieve from Redis.
 *
 * In this async version, we setup the next_callback in the client context object
 * and call connect_to_redis_server(). Here we also add both the client socket and
 * the Redis socket to hash tables so that we can retrieve the client context given
 * either the Redis socket or the client socket in the main epoll_wait() event loop.
 * */

int init_render_guestbook_template(int client_sock) {
    /* This is a new connection. Allocate a client context. */
    client_context *cc = malloc(sizeof(client_context));
    if (!cc)
        fatal_error("malloc()");
    cc->client_sock = client_sock;
    HASH_ADD(hh_cs, cc_cs_hash, client_sock, sizeof(int), cc);

    cc->next_callback = render_guestbook_post_redis_connect;
    connect_to_redis_server(cc);
    HASH_ADD(hh_rs, cc_rs_hash, redis_sock, sizeof(int), cc);
}

/*
 * If we are not serving static files and we want to write web apps, this is the place
 * to add more routes. If this function returns METHOD_NOT_HANDLED, the request is
 * considered a regular static file request and continues down that path. This function
 * gets precedence over static file serving.
 * */

int handle_app_get_routes(char *path, int client_sock) {
    if (strcmp(path, GUESTBOOK_ROUTE) == 0) {
        init_render_guestbook_template(client_sock);
        return METHOD_HANDLED;
    }

    return METHOD_NOT_HANDLED;
}

/*
 * This is the main get method handler. It checks for any app methods, else proceeds to
 * look for static files or index files inside of directories.
 * */

void handle_get_method(char *path, int client_sock)
{
    char final_path[1024];

    if (handle_app_get_routes(path, client_sock) == METHOD_HANDLED)
        return;

    /*
     If a path ends in a trailing slash, the client probably wants the index
     file inside of that directory.
     */
    if (path[strlen(path) - 1] == '/')
    {
        strcpy(final_path, "public");
        strcat(final_path, path);
        strcat(final_path, "index.html");
    }
    else
    {
        strcpy(final_path, "public");
        strcat(final_path, path);
    }

    /* The stat() system call will give you information about the file
     * like type (regular file, directory, etc), size, etc. */
    struct stat path_stat;
    if (stat(final_path, &path_stat) == -1)
    {
        printf("404 Not Found: %s\n", final_path);
        handle_http_404(client_sock);
    }
    else
    {
        /* Check if this is a normal/regular file and not a directory or something else */
        if (S_ISREG(path_stat.st_mode))
        {
            send_headers(final_path, path_stat.st_size, client_sock);
            transfer_file_contents(final_path, client_sock, path_stat.st_size);
            printf("200 %s %ld bytes\n", final_path, path_stat.st_size);
        }
        else
        {
            handle_http_404(client_sock);
            printf("404 Not Found: %s\n", final_path);
        }
    }
}

/*
 * A guest submitted their name and remarks via the form on the page. That data is
 * available to us as post x-www-form-urlencoded data. Fortunately, it is easy to
 * decode and get what we need. Please see example data below. We also have a utility
 * function to decode the data and get plain text.
 *
 * We use a simple, nested go-while loop construct and strtok() to split
 * variables-value pairs and then variables and values themselves. If all goes well,
 * we end up with the guest's name and remarks in our declared string arrays.
 *
 * Another point to note is that we've already read the headers. After that, what we
 * should find on the socket is the post data, which forms the body of the request.
 * */

void handle_new_guest_remarks(int client_sock, int redis_sock) {
    char remarks[1024]="";
    char name[512]="";
    char buffer[4026]="";
    char *c1, *c2;
    read(client_sock, buffer, sizeof(buffer));
    /*
     * Sample data format:
     * guest-remarks=Relatively+great+service&guest-name=Albert+Einstein
     * */
    char *assignment = strtok_r(buffer, "&", &c1);
    do {
        char *subassignment = strtok_r(assignment, "=", &c2);
        if (!subassignment)
            break;
        do {
            if (strcmp(subassignment, "guest-name") == 0) {
                subassignment = strtok_r(NULL, "=", &c2);
                if (!subassignment) {
                    name[0] = '\0';
                    break;
                }
                else
                    strcpy(name, subassignment);
            }
            if (strcmp(subassignment, "guest-remarks") == 0) {
                subassignment = strtok_r(NULL, "=", &c2);
                if (!subassignment) {
                    remarks[0] = '\0';
                    break;
                }
                else
                    strcpy(remarks, subassignment);
            }
            subassignment = strtok_r(NULL, "=", &c2);
            if (!subassignment)
                break;
        } while (1);

        assignment = strtok_r(NULL, "&", &c1);
        if (!assignment)
            break;
    } while (1);

    /* Validate name and remark lengths and show an error page is required. */
    if (strlen(name) == 0 || strlen(remarks) == 0) {
        char *html = "HTTP/1.0 400 Bad Request\r\ncontent-type: text/html\r\n\r\n<html><title>Error</title><body><p>Error: Do not leave name or remarks empty.</p><p><a href=\"/guestbook\">Go back to Guestbook</a></p></body></html>";
        write(client_sock, html, strlen(html));
        printf("400 POST /guestbook\n");
        return;
    }

    /*
     * POST uses form URL encoding. Decode the strings and append them to the Redis
     * list that holds all remarks. We combine the remarks and guest name into one
     * string, which we them push into the list.
     * */
    char *decoded_name = urlencoding_decode(name);
    char *decoded_remarks = urlencoding_decode(remarks);
    memset(buffer, 0, sizeof(buffer));
    sprintf(buffer, "%s - %s", decoded_remarks, decoded_name);
    redis_list_append(GUESTBOOK_REDIS_REMARKS_KEY, buffer, redis_sock);
    free(decoded_name);
    free(decoded_remarks);

    /* All good! Show a 'thank you' page. */
    char *html = "HTTP/1.0 200 OK\r\ncontent-type: text/html\r\n\r\n<html><title>Thank you!</title><body><p>Thank you for leaving feedback! We really appreciate that!</p><p><a href=\"/guestbook\">Go back to Guestbook</a></p></body></html>";
    write(client_sock, html, strlen(html));
    printf("200 POST /guestbook\n");
}

/*
 * This is the routing function for POST calls. If newer POST methods need to be handled,
 * you start by extending this function to check for your call slug and calling the
 * appropriate handler to deal with the call.
 * */
int handle_app_post_routes(char *path, int client_sock, int redis_sock) {
    if (strcmp(path, GUESTBOOK_ROUTE) == 0) {
        handle_new_guest_remarks(client_sock, redis_sock);
        return METHOD_HANDLED;
    }

    return METHOD_NOT_HANDLED;
}

/*
 * In HTTP, POST is used create objects. Our handle_get_method() function is relatively
 * more complicated since it has to deal with serving static files. This function however,
 * only deals with app-related routes.
 * */

void handle_post_method(char *path, int client_sock) {
    int redis_sock = connect_to_redis_server_sync();
    handle_app_post_routes(path, client_sock, redis_sock);
    close(redis_sock);
    close(client_sock);
}

/*
 * This function looks at method used and calls the appropriate handler function.
 * Since we only implement GET and POST methods, it calls handle_unimplemented_method()
 * in case both these don't match. This sends an error to the client.
 * */

void handle_http_method(char *method_buffer, int client_sock)
{
    char *method, *path;

    method = strtok(method_buffer, " ");
    strtolower(method);
    path = strtok(NULL, " ");

    if (strcmp(method, "get") == 0)
    {
        handle_get_method(path, client_sock);
    }
    else if (strcmp(method, "post") == 0) {

        handle_post_method(path, client_sock);
    }
    else
    {
        handle_unimplemented_method(client_sock);
    }
}

/*
 * This function is called per client request. It reads the HTTP headers and calls
 * handle_http_method() with the first header sent by the client. All other headers
 * are read and discarded since we do not deal with them.
 * */

void handle_client(int client_sock)
{
    char line_buffer[1024];
    char method_buffer[1024];
    int method_line = 0;

    while (1)
    {
        get_line(client_sock, line_buffer, sizeof(line_buffer));
        method_line++;

        unsigned long len = strlen(line_buffer);

        /*
         The first line has the HTTP method/verb. It's the only
         thing we care about. We read the rest of the lines and
         throw them away.
         */
        if (method_line == 1)
        {
            if (len == 0)
                return;

            strcpy(method_buffer, line_buffer);
        }
        else
        {
            if (len == 0)
                break;
        }
    }

    handle_http_method(method_buffer, client_sock);
}

/*
 * This is the beating heart of this program - our epoll_wait()-based event loop.
 *
 * When epoll_wait() returns normally, there are 3 possibilities:
 *
 * #1. we got a new connection on the server socket. Remember that we've
 * setup our server socket to be non-blocking, meaning that accept() in this
 * loop won't block. When epoll_wait() tells us that there is activity in the server
 * socket, it might be that there are several queued new client connections.
 * Since accept() is non-blocking, we call it multiple times. If it returns
 * a non-zero value, it means a client connection was accepted. We then call
 * handle_client() that starts the function call chain that ends in client_fini()
 * #2. We can read one of the client sockets without blocking
 * #3. We can read one of the Redis sockets without blocking
 *
 * In either of these cases, we find out the corresponding client_context
 * structure and call the next callback stored in the function pointer
 * read_write_callback, passing in the client_context. This can be tricky to
 * understand. Please read the accompanying article series on unixism.net.
 *
 * */

_Noreturn void enter_server_loop(int server_socket)
{
    int epoll_ret;
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    while (1) {
        epoll_ret = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (epoll_ret < 0)
            fatal_error("epoll_wait()");

        for (int i = 0; i < epoll_ret; i++) {

            if (events[i].events == 0)
                continue;

            int client_socket = accept(
                    server_socket,
                    (struct sockaddr *)&client_addr,
                    &client_addr_len);

            if (client_socket > 0 ) {
                handle_client(client_socket);
            }

            if (events[i].data.fd != server_socket) {
                if (!((events[i].events & EPOLLIN)||(events[i].events & EPOLLOUT))) {
                    printf("ERROR: Error condition in socket %d!\n", events[i].data.fd);
                    exit(1);
                    continue;
                }
                /* A descriptor handling one of the clients is ready */
                client_context *check_cc;
                HASH_FIND(hh_cs, cc_cs_hash, &events[i].data.fd, sizeof(int), check_cc);
                if (!check_cc) {
                    HASH_FIND(hh_rs, cc_rs_hash, &events[i].data.fd, sizeof(int), check_cc);
                    if (!check_cc) {
                        printf("Unable to find client context associated with socket %d.\n", events[i].data.fd);
	            		exit(1);
                    }

                }
                check_cc->read_write_callback(check_cc);
            }
        }
    }
}

typedef struct {
    unsigned long size,resident,share,text,lib,data,dt;
} statm_t;

void read_memory_stats(statm_t *result)
{
    const char* statm_path = "/proc/self/statm";

    FILE *f = fopen(statm_path,"r");
    if(!f){
        perror(statm_path);
        abort();
    }
    if(7 != fscanf(f,"%ld %ld %ld %ld %ld %ld %ld",
                   &result->size, &result->resident,
                   &result->share,&result->text,
                   &result->lib,&result->data,&result->dt))
    {
        perror(statm_path);
        abort();
    }
    fclose(f);
}

/*
 * When Ctrl-C is pressed on the terminal, the shell sends our process SIGINT. This is the
 * handler for SIGINT, like we setup in main().
 *
 * We use the getrusage() call to get resource usage in terms of user and system times and
 * we print those. This helps us see how our server is performing.
 * */

void print_stats(int signo) {
    double          user, sys;
    struct rusage   myusage;

    if (getrusage(RUSAGE_SELF, &myusage) < 0)
        fatal_error("getrusage()");

    user = (double) myusage.ru_utime.tv_sec +
                    myusage.ru_utime.tv_usec/1000000.0;
    sys = (double) myusage.ru_stime.tv_sec +
                   myusage.ru_stime.tv_usec/1000000.0;

    printf("\nuser time = %g, sys time = %g\n", user, sys);
    exit(0);
}


/*
 * Our main function is fairly simple. I sets up the listening socket, sets up 
 * a signal handler for SIGINT, creates a epoll descriptor and finally calls 
 * enter_server_loop(), which starts our main event loop based on epoll_wait()
 * */

int main(int argc, char *argv[])
{
    int server_port;

    if (argc > 1)
        server_port = atoi(argv[1]);
    else
        server_port = DEFAULT_SERVER_PORT;

    if (argc > 2)
        strcpy(redis_host_ip, argv[2]);
    else
        strcpy(redis_host_ip, REDIS_SERVER_HOST);

    printf("Configured to connect to Redis @%s:%d\n", redis_host_ip, REDIS_SERVER_PORT);
    int server_socket = setup_listening_socket(server_port);

    /* Setting the locale and using the %' escape sequence lets us format
     * large numbers with commas */
    setlocale(LC_NUMERIC, "");

    printf("ZeroHTTPd server listening on port %d\n", server_port);
    signal(SIGINT, print_stats);

    epoll_fd = epoll_create1(0);
    if (epoll_fd < 0)
        fatal_error("epoll_create1()");

    add_to_epoll_fd_list(server_socket, EPOLLIN);
    enter_server_loop(server_socket);

    /* Never reaches this point */
    return (0);
}
