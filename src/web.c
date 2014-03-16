#include "web.h"
#include "disk_mgr.h"
#include "util.h"

#include "wire.h"
#include "wire_pool.h"
#include "wire_fd.h"
#include "wire_wait.h"
#include "wire.h"
#include "wire_fd.h"
#include "wire_pool.h"
#include "wire_stack.h"
#include "macros.h"
#include "http_parser.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <string.h>
#include <fcntl.h>
#include <memory.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>


#define CONNECTION_BUF_SIZE 8192

struct web {
	wire_pool_t web_pool;
	wire_t accept_wire;
	wire_wait_t close_wait;
};
static struct web web;

struct web_data {
	int fd;
	wire_fd_state_t fd_state;
	int method;
	char path[256];
	char query_string[256];
};

struct url {
	const char *path;
	int (*cb)(http_parser *parser);
};

static int buf_write(wire_fd_state_t *fd_state, const char *buf, int len)
{
	int sent = 0;
	do {
		int ret = write(fd_state->fd, buf + sent, len - sent);
		if (ret == 0)
			return -1;
		else if (ret > 0) {
			sent += ret;
			if (sent == len)
				return 0;
		} else {
			// Error
			if (errno != EINTR && errno != EAGAIN)
				return -1;
		}

		wire_fd_mode_write(fd_state);
		wire_fd_wait(fd_state);
		// TODO: Need to handle timeouts here as well
	} while (1);
}

static int response_write(http_parser *parser, int code, const char *title, const char *content_type, const char *body, unsigned body_len)
{
	char hdr[512];
	int hdr_len;
	hdr_len = snprintf(hdr, sizeof(hdr), "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n%s\r\n",
			code, title,
			content_type,
			body_len,
			!http_should_keep_alive(parser) ? "Connection: close\r\n" : "");

	struct web_data *d = parser->data;
	buf_write(&d->fd_state, hdr, hdr_len);
	buf_write(&d->fd_state, body, body_len);

	return 0;
}

#define SERVE_VAR(name, content_type) static int serve_##name(http_parser *parser) \
{ \
	return response_write(parser, 200, "OK", content_type, name, strlen(name)); \
}

SERVE_VAR(app_js, "text/javascript")
SERVE_VAR(app_css, "text/css")
SERVE_VAR(index_html, "text/html")

static int api_disk_list(http_parser *parser)
{
	char buf[CONNECTION_BUF_SIZE];
	int written = disk_manager_disk_list_json(buf, CONNECTION_BUF_SIZE);
	if (written >= 0) {
		return response_write(parser, 200, "OK", "application/json", buf, written-1);
	} else {
		printf("ERROR: space insufficient\n");
		static const char *msg = "Insufficient buffer space";
		response_write(parser, 500, msg, "text/plain", msg, strlen(msg));
		return -1;
	}
}

static int rescan_disks(http_parser *parser)
{
	static const char *msg = "rescanned\n";
	int ret = response_write(parser, 200, "OK", "text/plain", msg, strlen(msg));
	disk_manager_rescan();
	return ret;
}

static struct url urls[] = {
	{"/", serve_index_html},
	{"/app.js", serve_app_js},
	{"/app.css", serve_app_css},
	{"/rescan", rescan_disks},
	{"/api/disks", api_disk_list},
};

static void set_nonblock(int fd)
{
	int ret = fcntl(fd, F_GETFL);
	if (ret < 0)
		return;

	fcntl(fd, F_SETFL, ret | O_NONBLOCK);
}

static void set_reuse(int fd)
{
	int so_reuseaddr = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &so_reuseaddr, sizeof(so_reuseaddr));
}

static int socket_setup(unsigned short port)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 1) {
		perror("Failed to create socket");
		return -1;
	}

	set_nonblock(fd);
	set_reuse(fd);

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);

	int ret = bind(fd, (struct sockaddr*)&addr, sizeof(addr));
	if (ret < 0) {
		perror("Failed to bind to socket");
		close(fd);
		return -1;
	}

	ret = listen(fd, 100);
	if (ret < 0) {
		perror("failed to listen to port");
		close(fd);
		return -1;
	}

	return fd;
}

static int on_message_begin(http_parser *parser)
{
	printf("Message begin\n");

	struct web_data *d = parser->data;

	d->path[0] = 0;
	d->query_string[0] = 0;

	return 0;
}

static int on_headers_complete(http_parser *parser)
{
	struct web_data *d = parser->data;
	d->method = parser->method;
	printf("Headers complete: HTTP/%d.%d %s\n", parser->http_major, parser->http_minor, http_method_str(parser->method));
	return 0;
}

static int on_message_complete(http_parser *parser)
{
	printf("message complete\n");
	struct web_data *d = parser->data;

	int i;

	printf("Method: %d\nPath: %s\nQuery: %s\n", d->method, d->path, d->query_string);

	for (i = 0; i < ARRAY_SIZE(urls); i++) {
		if (strcasecmp(urls[i].path, d->path) == 0) {
			urls[i].cb(parser);
			printf("Request succeeded\n");
			return 0;
		}
	}

	static const char *msg = "Not Found";
	return response_write(parser, 404, msg, "text/plain", msg, strlen(msg));
}

static int on_url(http_parser *parser, const char *at, size_t length)
{
	printf("URL: %.*s\n", (int)length, at);

	struct web_data *d = parser->data;
	struct http_parser_url url;
	int ret = http_parser_parse_url(at, length, 0, &url);
	if (ret != 0) {
		printf("URL parsing failed\n");
		return -1;
	}

	if (url.field_set & (1<<UF_PATH)) {
		memcpy(d->path, at + url.field_data[UF_PATH].off, url.field_data[UF_PATH].len);
		d->path[url.field_data[UF_PATH].len] = 0;
	} else {
		printf("Missing path in the url, baffled and dazed!\n");
		return -1;
	}

	if (url.field_set & (1<<UF_QUERY)) {
		memcpy(d->query_string, at + url.field_data[UF_QUERY].off, url.field_data[UF_QUERY].len);
		d->query_string[url.field_data[UF_QUERY].len] = 0;
	}

	return 0;
}

static int on_status(http_parser *parser, const char *at, size_t length)
{
	UNUSED(parser);
	printf("STATUS: %.*s\n", (int)length, at);
	return 0;
}

static int on_header_field(http_parser *parser, const char *at, size_t length)
{
	UNUSED(parser);
	printf("HEADER FIELD: %.*s\n", (int)length, at);
	return 0;
}

static int on_header_value(http_parser *parser, const char *at, size_t length)
{
	UNUSED(parser);
	printf("HEADER VALUE: %.*s\n", (int)length, at);
	return 0;
}

static int on_body(http_parser *parser, const char *at, size_t length)
{
	UNUSED(parser);
	printf("BODY: %.*s\n", (int)length, at);
	return 0;
}

static const struct http_parser_settings parser_settings = {
	.on_message_begin = on_message_begin,
	.on_headers_complete = on_headers_complete,
	.on_message_complete = on_message_complete,

	.on_url = on_url,
	.on_status = on_status,
	.on_header_field = on_header_field,
	.on_header_value = on_header_value,
	.on_body = on_body,
};

static void web_run(void *arg)
{
	struct web_data d = {
		.fd = (long int)arg,
	};
	http_parser parser;

	wire_fd_mode_init(&d.fd_state, d.fd);
	wire_fd_mode_read(&d.fd_state);

	set_nonblock(d.fd);

	http_parser_init(&parser, HTTP_REQUEST);
	parser.data = &d;

	char buf[4096];
	do {
		buf[0] = 0;
		int received = read(d.fd, buf, sizeof(buf));
		printf("Received: %d %d\n", received, errno);
		if (received == 0) {
			/* Fall-through, tell parser about EOF */
			printf("Received EOF\n");
		} else if (received < 0) {
			printf("Error\n");
			if (errno == EINTR || errno == EAGAIN) {
				printf("Waiting\n");
				/* Nothing received yet, wait for it */
				// TODO: employ a timeout here to exit if no input is received
				wire_fd_wait(&d.fd_state);
				printf("Done waiting\n");
				continue;
			} else {
				printf("breaking out\n");
				break;
			}
		}

		printf("Processing %d\n", (int)received);
		size_t processed = http_parser_execute(&parser, &parser_settings, buf, received);
		if (parser.upgrade) {
			/* Upgrade not supported yet */
			printf("Upgrade not supported, bailing out\n");
			break;
		} else if (received == 0) {
			// At EOF, exit now
			printf("Received EOF\n");
			break;
		} else if (processed != (size_t)received) {
			// Error in parsing
			printf("Not everything was parsed, error is likely, bailing out.\n");
			break;
		}
	} while (1);

	wire_fd_mode_none(&d.fd_state);
	close(d.fd);
}

// ---

static void web_accept(void *arg)
{
        int port = (long int)arg;
        int fd = socket_setup(port);
        if (fd < 0)
                return;

		unsigned web_id = 0;
        wire_fd_state_t fd_state;
        wire_fd_mode_init(&fd_state, fd);
        wire_fd_mode_read(&fd_state);

		wire_wait_list_t wait_list;
		wire_wait_list_init(&wait_list);
		wire_wait_init(&web.close_wait);
		wire_wait_chain(&wait_list, &web.close_wait);
		wire_fd_wait_list_chain(&wait_list, &fd_state);

		printf("listening on port %d\n", port);

        while (1) {
                wire_list_wait(&wait_list);

				if (web.close_wait.triggered) {
					break;
				}

                int new_fd = accept(fd, NULL, NULL);
                if (new_fd >= 0) {
                        printf("New connection: %u %d\n", web_id, new_fd);
                        char name[32];
                        snprintf(name, sizeof(name), "web %u %d", web_id++, new_fd);
                        wire_t *task = wire_pool_alloc(&web.web_pool, name, web_run, (void*)(long int)new_fd);
                        if (!task) {
                                printf("Web server is busy, sorry\n");
								// TODO: Send something into the connection
                                close(new_fd);
                        }
                } else {
                        if (errno != EINTR && errno != EAGAIN) {
                                perror("Error accepting from listening socket");
                                break;
                        }
                }
        }

		wire_fd_mode_none(&fd_state);
		close(fd);
		printf("Web interface shutdown\n");
}

void web_init(int port)
{
	memset(&web, 0, sizeof(web));

	wire_pool_init(&web.web_pool, NULL, 16, CONNECTION_BUF_SIZE*2);
	wire_init(&web.accept_wire, "web accept", web_accept, (void*)(long int)port, WIRE_STACK_ALLOC(4096));
}

void web_stop(void)
{
	printf("Shutting down the web interface\n");
	wire_wait_resume(&web.close_wait);
}
