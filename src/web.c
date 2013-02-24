#include "web.h"
#include "disk_mgr.h"
#include "util.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <string.h>

#include <ev.h>
#include "../libebb/ebb.h"

#define CONNECTION_BUF_SIZE 128*1024
static int c = 0;

struct web {
	ebb_server server;
};
static struct web web;

struct url {
	const char *path;
	void (*cb)(ebb_connection *connection, ebb_request *request, const char *path, const char *uri, const char *query);
};

struct request_data {
	ebb_connection *connection;
	char path[256];
	char uri[256];
	char query_string[256];
};

static inline void serve_var(ebb_connection *connection, const char *content)
{
	ebb_connection_write(connection, content, strlen(content), ebb_connection_schedule_close);
}

#define SERVE_VAR(name) static void serve_##name(ebb_connection *connection, ebb_request *request, const char *path, const char *uri, const char *query) \
{ \
	serve_var(connection, name); \
}

SERVE_VAR(app_js)
SERVE_VAR(app_css)
SERVE_VAR(index_html)

static char *connection_data(ebb_connection *connection)
{
	if (!connection->data) {
		connection->data = malloc(CONNECTION_BUF_SIZE);
		assert(connection->data);
	}

	return connection->data;
}

static void api_disk_list(ebb_connection *connection, ebb_request *request, const char *path, const char *uri, const char *query)
{
	char *buf = connection_data(connection);
	int hdr_len = snprintf(buf, CONNECTION_BUF_SIZE, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %-10d\r\n\r\n", 0);
	int written = disk_manager_disk_list_json(buf + hdr_len, CONNECTION_BUF_SIZE - hdr_len);
	if (written >= 0) {
		char hdr[hdr_len+1];
		snprintf(hdr, hdr_len+1, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %-10d\r\n\r\n", written-1);
		memcpy(buf, hdr, hdr_len);
		ebb_connection_write(connection, buf, hdr_len + written, ebb_connection_schedule_close);
	} else {
		printf("ERROR: space insufficient\n");
		static const char msg[] = "HTTP/1.1 500 ERROR\r\n";
		ebb_connection_write(connection, msg, strlen(msg), ebb_connection_schedule_close);
	}
}

static void rescan_disks(ebb_connection *connection, ebb_request *request, const char *path, const char *uri, const char *query)
{
	static const char msg[] = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 10\r\n\r\nrescanned\n";
	ebb_connection_write(connection, msg, strlen(msg), ebb_connection_schedule_close);
	disk_manager_rescan();
}

void on_close(ebb_connection *connection)
{
  free(connection->data);
  free(connection);
}

static void request_path(ebb_request *request, const char *at, size_t len)
{
	struct request_data *data = request->data;
	strncpy(data->path, at, MIN(len, sizeof(data->path)));
	data->path[sizeof(data->path)-1] = 0;
}

static void request_uri(ebb_request *request, const char *at, size_t len)
{
	struct request_data *data = request->data;
	strncpy(data->uri, at, MIN(len, sizeof(data->uri)));
	data->uri[sizeof(data->uri)-1] = 0;
}

static void request_query_string(ebb_request *request, const char *at, size_t len)
{
	struct request_data *data = request->data;
	strncpy(data->query_string, at, MIN(len, sizeof(data->query_string)));
	data->query_string[sizeof(data->query_string)-1] = 0;
}

static struct url urls[] = {
	{"/", serve_index_html},
	{"/app.js", serve_app_js},
	{"/app.css", serve_app_css},
	{"/rescan", rescan_disks},
	{"/api/disks", api_disk_list},
};
static void request_complete(ebb_request *request)
{
	struct request_data *data = request->data;
	ebb_connection *connection = data->connection;
	int i;
	bool handled = false;

	printf("Method: %d\nPath: %s\nURI: %s\nQuery: %s\n", request->method, data->path, data->uri, data->query_string);

	for (i = 0; i < ARRAY_SIZE(urls); i++) {
		if (strcasecmp(urls[i].path, data->path) == 0) {
			urls[i].cb(connection, request, data->path, data->uri, data->query_string);
			printf("Request succeeded\n");
			handled = true;
			break;
		}
	}
	if (!handled) {
		static const char msg[] = "HTTP/1.1 404 NOT FOUND\r\nContent-Type: text/plain\r\nContent-Length: 10\r\n\r\nnot found\n";
		ebb_connection_write(connection, msg, strlen(msg), ebb_connection_schedule_close);
		printf("Request not found\n");
	}

	free(request->data);
	free(request);
}

static ebb_request* new_request(ebb_connection *connection)
{
	struct request_data *data = calloc(sizeof(struct request_data), 1);
	data->connection = connection;

	ebb_request *request = malloc(sizeof(ebb_request));
	ebb_request_init(request);
	request->data = data;
	request->on_complete = request_complete;
	request->on_path = request_path;
	request->on_uri = request_uri;
	request->on_query_string = request_query_string;
	return request;
}

ebb_connection* new_connection(ebb_server *server, struct sockaddr_in *addr)
{
  ebb_connection *connection = malloc(sizeof(ebb_connection));
  if(connection == NULL) {
    return NULL;
  }

  ebb_connection_init(connection);
  connection->new_request = new_request;
  connection->on_close = on_close;
  
  printf("connection: %d\n", c++);
  return connection;
}

void web_init(struct ev_loop *loop, int port)
{
	memset(&web, 0, sizeof(web));
	ebb_server_init(&web.server, loop); 
	web.server.new_connection = new_connection;

	printf("listening on port %d\n", port);
	ebb_server_listen_on_port(&web.server, port);
}
