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
#include <sys/wait.h>
#include <time.h>
#include <locale.h>

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

#define PREFORK_CHILDREN            100
static pid_t pids[PREFORK_CHILDREN];

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

int     redis_socket_fd;
char    redis_host_ip[32];

/*
 One function that prints the system call and the error details
 and then exits with error code 1. Non-zero meaning things didn't go well.
 */
void fatal_error(const char *syscall)
{
    perror(syscall);
    exit(1);
}

/*
 * Redis uses a long-lived connection. Many commands are sent on and
 * responded to over the same connection. In this function, we connect
 * to the Redis server and store the connected socket in a global variable.
 * */

void connect_to_redis_server() {
    struct sockaddr_in redis_servaddr;
    redis_socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    bzero(&redis_servaddr, sizeof(redis_servaddr));
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
    else
        printf("Connected to Redis server@ %s:%d\n", REDIS_SERVER_HOST, REDIS_SERVER_PORT);
}

/*
 * This is an internal function not to be used directly. It send the command to the server,
 * but reads back the raw server response. Not very useful to be used directly without
 * first processing it to extract the required data.
 * */

int _redis_get_key(const char *key, char *value_buffer, int value_buffer_sz) {
    char *req_buffer;
    /*
     * asprintf() is a useful GNU extension that allocates the string as required.
     * No more guessing the right size for the buffer that holds the string.
     * Don't forget to call free() once you're done.
     * */
    asprintf(&req_buffer, "*2\r\n$3\r\nGET\r\n$%ld\r\n%s\r\n", strlen(key), key);
    write(redis_socket_fd, req_buffer, strlen(req_buffer));
    free(req_buffer);
    read(redis_socket_fd, value_buffer, value_buffer_sz);
    return 0;
}

/*
 * Given the key, fetched the number value associated with it.
 * If we know that a key is indeed a number, use this function. The server sends back
 * numbers are strings, but we convert them and provide them in a number format.
 * */

int redis_get_int_key(const char *key, int *value) {
    /*
     * Example response from server:
     * $3\r\n385\r\n
     * This means that the server is telling us to expect a string
     * of 3 characters.
     */

    /* Shortcut: setting a character array to a null string will fill it up with zeros */
    char redis_response[64] = "";
    _redis_get_key(key, redis_response, sizeof(redis_response));
    char *p = redis_response;
    if (*p != '$')
        return -1;

    /* Loop till we reach the actual number string */
    while(*p++ != '\n');

    /* The following loop converts a string representation of a number to a number */
    int intval = 0;
    while (*p != '\r') {
        intval = (intval * 10) + (*p - '0');
        p++;
    }
    *value = intval;

    return 0;
}

/* Increment given key by incr_by. Key is created by Redis if it doesn't exist. */
int redis_incr_by(char *key, int incr_by) {
    char cmd_buf[1024] = "";
    char incr_by_str[16] = "";
    sprintf(incr_by_str, "%d", incr_by);
    sprintf(cmd_buf, "*3\r\n$6\r\nINCRBY\r\n$%ld\r\n%s\r\n$%ld\r\n%d\r\n", strlen(key), key, strlen(incr_by_str), incr_by);
    write(redis_socket_fd, cmd_buf, strlen(cmd_buf));
    bzero(cmd_buf, sizeof(cmd_buf));
    read(redis_socket_fd, cmd_buf, sizeof(cmd_buf));
    return 0;
}

/* Implies increment by one. Useful addition since this is a common use case. */
int redis_incr(char *key) {
    return redis_incr_by(key, 1);
}

/*
 * Appends an item pointed to by 'value' to the list in redis referred by 'key'.
 * Uses the Redis RPUSH command to get the job done.
 * */

int redis_list_append(char *key, char *value) {
    char cmd_buf[1024] = "";
    sprintf(cmd_buf, "*3\r\n$5\r\nRPUSH\r\n$%ld\r\n%s\r\n$%ld\r\n%s\r\n", strlen(key), key, strlen(value), value);
    write(redis_socket_fd, cmd_buf, strlen(cmd_buf));
    bzero(cmd_buf, sizeof(cmd_buf));
    read(redis_socket_fd, cmd_buf, sizeof(cmd_buf));
    return 0;
}

/*
 * Get the range of items in a list from 'start' to 'end'.
 * This function allocates memory. An array of pointers and all the strings pointed to by it
 * are dynamically allocated. redis_free_array_results() has to be called to free this
 * allocated memory by the caller.
 * */

int redis_list_get_range(char *key, int start, int end, char ***items, int *items_count) {
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

    char cmd_buf[1024]="", start_str[16], end_str[16];
    sprintf(start_str, "%d", start);
    sprintf(end_str, "%d", end);
    sprintf(cmd_buf, "*4\r\n$6\r\nLRANGE\r\n$%ld\r\n%s\r\n$%ld\r\n%s\r\n$%ld\r\n%s\r\n", strlen(key), key, strlen(start_str), start_str, strlen(end_str), end_str);
    write(redis_socket_fd, cmd_buf, strlen(cmd_buf));

    /* Find out the length of the array */
    char ch;
    read(redis_socket_fd, &ch, 1);
    if (ch != '*')
        return -1;

    int returned_items = 0;
    while (1) {
        read(redis_socket_fd, &ch, 1);
        if (ch == '\r') {
            /* Read the next '\n' character */
            read(redis_socket_fd, &ch, 1);
            break;
        }
        returned_items = (returned_items * 10) + (ch - '0');
    }
    /* Allocate the array that will hold a pointer each for
     * every element in the returned list */
    *items_count = returned_items;
    char **items_holder = malloc(sizeof(char *) * returned_items);
    *items = items_holder;

    /*
     * We now know the length of the array. Let's loop that many iterations
     * and grab those strings, allocating a new chunk of memory for each one.
     * */
    for (int i = 0; i < returned_items; i++) {
        /* read the first '$' */
        read(redis_socket_fd, &ch, 1);
        int str_size = 0;
        while (1) {
            read(redis_socket_fd, &ch, 1);
            if (ch == '\r') {
                /* Read the next '\n' character */
                read(redis_socket_fd, &ch, 1);
                break;
            }
            str_size = (str_size * 10) + (ch - '0');
        }
        char *str = malloc(sizeof(char) * str_size + 1);
        items_holder[i] = str;
        read(redis_socket_fd, str, str_size);
        str[str_size] = '\0';
        /* Read the '\r\n' chars */
        read(redis_socket_fd, &ch, 1);
        read(redis_socket_fd, &ch, 1);
    }
}

/*
 * Utility function to get the whole list.
 * We set start to 0 and end to -1, which is a magic number that means the last element.
 * */

int redis_get_list(char *key, char ***items, int *items_count) {
    return redis_list_get_range(key, 0, -1, items, items_count);
}

/*
 * redis_get_list() and redis_get_list_range() return a dynamically allocated result.
 * This is a utility function to free all strings and also the array of pointers that
 * holds pointers to those strings.
 * */

int redis_free_array_result(char **items, int length) {
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

    sock = socket(PF_INET, SOCK_STREAM, 0);
    if (sock == -1)
        fatal_error("socket()");

    int enable = 1;
    if (setsockopt(sock,
                   SOL_SOCKET, SO_REUSEADDR,
                   &enable, sizeof(int)) < 0)
        fatal_error("setsockopt(SO_REUSEADDR)");

    bzero(&srv_addr, sizeof(srv_addr));
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
 * The guest book template file is a normal HTML file except two special strings:
 * $GUEST_REMARKS$ and $VISITOR_COUNT$
 * In this method, both these special template variables are replaced by content
 * generated by us. And that content is based on stuff we retrieve from Redis.
 * */

int render_guestbook_template(int client_socket) {
    char templ[16384]="";
    char rendering[16384]="";

    /* Read the template file */
    int fd = open(GUESTBOOK_TEMPLATE, O_RDONLY);
    if (fd == -1)
        fatal_error("Template read()");
    read(fd, templ, sizeof(templ));
    close(fd);

    /* Get guestbook entries and render them as HTML */
    int entries_count;
    char **guest_entries;
    char guest_entries_html[16384]="";
    redis_get_list(GUESTBOOK_REDIS_REMARKS_KEY, &guest_entries, &entries_count);
    for (int i = 0; i < entries_count; i++) {
        char guest_entry[1024];
        sprintf(guest_entry, "<p class=\"guest-entry\">%s</p>", guest_entries[i]);
        strcat(guest_entries_html, guest_entry);
    }
    redis_free_array_result(guest_entries, entries_count);

    /* In Redis, increment visitor count and fetch latest value */
    int visitor_count;
    char visitor_count_str[16]="";
    redis_incr(GUESTBOOK_REDIS_VISITOR_KEY);
    redis_get_int_key(GUESTBOOK_REDIS_VISITOR_KEY, &visitor_count);
    /* Setting the locale and using the %' escape sequence lets us format
     * numbers with commas */
    setlocale(LC_NUMERIC, "");
    sprintf(visitor_count_str, "%'d", visitor_count);

    /* Replace guestbook entries */
    char *entries = strstr(templ, GUESTBOOK_TMPL_REMARKS);
    if (entries) {
        memcpy(rendering, templ, entries-templ);
        strcat(rendering, guest_entries_html);
        char *copy_offset = templ + (entries-templ) + strlen(GUESTBOOK_TMPL_REMARKS);
        strcat(rendering, copy_offset);
        strcpy(templ, rendering);
        bzero(rendering, sizeof(rendering));
    }

    /* Replace visitor count */
    char *vcount = strstr(templ, GUESTBOOK_TMPL_VISITOR);
    if (vcount) {
        memcpy(rendering, templ, vcount-templ);
        strcat(rendering, visitor_count_str);
        char *copy_offset = templ + (vcount-templ) + strlen(GUESTBOOK_TMPL_VISITOR);
        strcat(rendering, copy_offset);
        strcpy(templ, rendering);
        bzero(rendering, sizeof(rendering));
    }

    /*
     * We've now rendered the template. Send headers and finally the
     * template over to the client.
     * */
    char send_buffer[1024];
    strcpy(send_buffer, "HTTP/1.0 200 OK\r\n");
    send(client_socket, send_buffer, strlen(send_buffer), 0);
    strcpy(send_buffer, SERVER_STRING);
    send(client_socket, send_buffer, strlen(send_buffer), 0);
    strcpy(send_buffer, "Content-Type: text/html\r\n");
    send(client_socket, send_buffer, strlen(send_buffer), 0);
    sprintf(send_buffer, "content-length: %ld\r\n", strlen(templ));
    send(client_socket, send_buffer, strlen(send_buffer), 0);
    strcpy(send_buffer, "\r\n");
    send(client_socket, send_buffer, strlen(send_buffer), 0);
    send(client_socket, templ, strlen(templ), 0);
    printf("200 GET /guestbook %ld bytes\n", strlen(templ));
}

/*
 * If we are not serving static files and we want to write web apps, this is the place
 * to add more routes. If this function returns METHOD_NOT_HANDLED, the request is
 * considered a regular static file request and continues down that path. This function
 * gets precedence over static file serving.
 * */

int handle_app_get_routes(char *path, int client_socket) {
    if (strcmp(path, GUESTBOOK_ROUTE) == 0) {
        render_guestbook_template(client_socket);
        return METHOD_HANDLED;
    }

    return METHOD_NOT_HANDLED;
}

/*
 * This is the main get method handler. It checks for any app methods, else proceeds to
 * look for static files or index files inside of directories.
 * */

void handle_get_method(char *path, int client_socket)
{
    char final_path[1024];

    if (handle_app_get_routes(path, client_socket) == METHOD_HANDLED)
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
        handle_http_404(client_socket);
    }
    else
    {
        /* Check if this is a normal/regular file and not a directory or something else */
        if (S_ISREG(path_stat.st_mode))
        {
            send_headers(final_path, path_stat.st_size, client_socket);
            transfer_file_contents(final_path, client_socket, path_stat.st_size);
            printf("200 %s %ld bytes\n", final_path, path_stat.st_size);
        }
        else
        {
            handle_http_404(client_socket);
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

void handle_new_guest_remarks(int client_socket) {
    char remarks[1024]="";
    char name[512]="";
    char buffer[4026]="";
    char *c1, *c2;
    read(client_socket, buffer, sizeof(buffer));
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
        write(client_socket, html, strlen(html));
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
    bzero(buffer, sizeof(buffer));
    sprintf(buffer, "%s - %s", decoded_remarks, decoded_name);
    redis_list_append(GUESTBOOK_REDIS_REMARKS_KEY, buffer);
    free(decoded_name);
    free(decoded_remarks);

    /* All good! Show a 'thank you' page. */
    char *html = "HTTP/1.0 200 OK\r\ncontent-type: text/html\r\n\r\n<html><title>Thank you!</title><body><p>Thank you for leaving feedback! We really appreciate that!</p><p><a href=\"/guestbook\">Go back to Guestbook</a></p></body></html>";
    write(client_socket, html, strlen(html));
    printf("200 POST /guestbook\n");
}

/*
 * This is the routing function for POST calls. If newer POST methods need to be handled,
 * you start by extending this function to check for your call slug and calling the
 * appropriate handler to deal with the call.
 * */
int handle_app_post_routes(char *path, int client_socket) {
    if (strcmp(path, GUESTBOOK_ROUTE) == 0) {
        handle_new_guest_remarks(client_socket);
        return METHOD_HANDLED;
    }

    return METHOD_NOT_HANDLED;
}

/*
 * In HTTP, POST is used create objects. Our handle_get_method() function is relatively
 * more complicated since it has to deal with serving static files. This function however,
 * only deals with app-related routes.
 * */

void handle_post_method(char *path, int client_socket) {
    handle_app_post_routes(path, client_socket);
}

/*
 * This function looks at method used and calls the appropriate handler function.
 * Since we only implement GET and POST methods, it calls handle_unimplemented_method()
 * in case both these don't match. This sends an error to the client.
 * */

void handle_http_method(char *method_buffer, int client_socket)
{
    char *method, *path;

    method = strtok(method_buffer, " ");
    strtolower(method);
    path = strtok(NULL, " ");

    if (strcmp(method, "get") == 0)
    {
        handle_get_method(path, client_socket);
    }
    else if (strcmp(method, "post") == 0) {

        handle_post_method(path, client_socket);
    }
    else
    {
        handle_unimplemented_method(client_socket);
    }
}

/*
 * This function is called per client request. It reads the HTTP headers and calls
 * handle_http_method() with the first header sent by the client. All other headers
 * are read and discarded since we do not deal with them.
 * */

void handle_client(int client_socket)
{
    char line_buffer[1024];
    char method_buffer[1024];
    int method_line = 0;

    while (1)
    {
        get_line(client_socket, line_buffer, sizeof(line_buffer));
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

    handle_http_method(method_buffer, client_socket);
}

/*
 * This function is the main server loop. It never returns. In a loop, it accepts client
 * connections and calls handle_client() to serve the request. Once the request is served,
 * it closes the client connection and goes back to waiting for a new client connection,
 * calling accept() again.
 * */

void enter_server_loop(int server_socket) {
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    signal(SIGINT, SIG_IGN);
    connect_to_redis_server();

    while (1)
    {
        int client_socket = accept(
                server_socket,
                (struct sockaddr *)&client_addr,
                &client_addr_len);
        if (client_socket == -1)
            fatal_error("accept()");

        handle_client(client_socket);
        close(client_socket);
    }
}

/*
 * When Ctrl-C is pressed on the terminal, the shell sends our process SIGINT. This is the
 * handler for SIGINT, like we setup in main().
 *
 * We use the getrusage() call to get resource usage in terms of user and system times and
 * we print those. This helps us see how our server is performing.
 * */

void print_stats() {
    double          user, sys;
    struct rusage   myusage, childusage;

    if (getrusage(RUSAGE_SELF, &myusage) < 0)
        fatal_error("getrusage()");
    if (getrusage(RUSAGE_CHILDREN, &childusage) < 0)
        fatal_error("getrusage()");

    user = (double) myusage.ru_utime.tv_sec +
                    myusage.ru_utime.tv_usec/1000000.0;
    user += (double) childusage.ru_utime.tv_sec +
                     childusage.ru_utime.tv_usec/1000000.0;
    sys = (double) myusage.ru_stime.tv_sec +
                   myusage.ru_stime.tv_usec/1000000.0;
    sys += (double) childusage.ru_stime.tv_sec +
                    childusage.ru_stime.tv_usec/1000000.0;

    printf("\nuser time = %g, sys time = %g\n", user, sys);
    exit(0);
}

void sigint_handler(int signo) {
    printf("Signal handler called\n");
    for(int i=0; i < PREFORK_CHILDREN; i++)
        kill(pids[i], SIGTERM);
    while (wait(NULL) > 0);
    print_stats();
}

pid_t create_child(int index, int listening_socket) {
    pid_t pid;

    pid = fork();
    if(pid < 0)
        fatal_error("fork()");
    else if (pid > 0)
        return (pid);

    printf("Server %d(pid: %ld) starting\n", index, (long)getpid());
    enter_server_loop(listening_socket);
}

/*
 * Our main function is fairly simple. I sets up the listening socket, establishes
 * a connection to Redis, which we use to host our Guestbook app, sets up a signal
 * handler for SIGINT and creates PREFORK_CHILDREN number of processes to handle.
 * each of these processes call enter_server_loop(), which accepts and
 * serves client requests.
 * */

int main(int argc, char *argv[])
{
    int server_port;
    signal(SIGINT, sigint_handler);

    if (argc > 1)
      server_port = atoi(argv[1]);
    else
      server_port = DEFAULT_SERVER_PORT;

    if (argc > 2)
        strcpy(redis_host_ip, argv[2]);
    else
        strcpy(redis_host_ip, REDIS_SERVER_HOST);

    int server_socket = setup_listening_socket(server_port);
    printf("ZeroHTTPd server listening on port %d\n", server_port);
    for(int i=0; i < PREFORK_CHILDREN; i++)
        pids[i] = create_child(i, server_socket);

    for(;;)
        pause();
}

