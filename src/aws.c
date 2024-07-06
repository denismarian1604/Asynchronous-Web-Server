// SPDX-License-Identifier: BSD-3-Clause

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/sendfile.h>
#include <sys/eventfd.h>
#include <libaio.h>
#include <errno.h>

#include "aws.h"
#include "utils/util.h"
#include "utils/debug.h"
#include "utils/sock_util.h"
#include "utils/w_epoll.h"

/* server socket file descriptor */
static int listenfd;

/* epoll file descriptor */
static int epollfd;

static io_context_t ctx;

int connection_send_data(struct connection *conn);

static int aws_on_path_cb(http_parser *p, const char *buf, size_t len)
{
	struct connection *conn = (struct connection *)p->data;

	memcpy(conn->request_path, buf, len);
	conn->request_path[len] = '\0';
	conn->have_path = 1;

	return 0;
}

static void connection_prepare_send_reply_header(struct connection *conn)
{
	snprintf(conn->send_buffer, BUFSIZ, "HTTP/1.1 200 OK\r\n"
							"Content-Type: text/html; charset=UTF-8\r\n"
							"Content-Length: %lu\r\n"
							"Proxy-Connection: close\r\n"
							"\r\n", conn->file_size);

	// strncpy(conn->send_buffer, header, BUFSIZ);
	conn->send_buffer[BUFSIZ - 1] = '\0';
	conn->send_len = strlen(conn->send_buffer);
}

static void connection_prepare_send_404(struct connection *conn)
{
	const char *header = "HTTP/1.1 404 Not Found\r\n"
							"Content-Type: text/html; charset=UTF-8\r\n"
							"Content-Length: 9\r\n"
							"Proxy-Connection: close\r\n"
							"\r\n"
							"Not Found";

	memcpy(conn->send_buffer, header, BUFSIZ);
	conn->send_buffer[BUFSIZ - 1] = '\0';
	conn->send_len = strlen(conn->send_buffer);
}

static enum resource_type connection_get_resource_type(struct connection *conn)
{
	// check if the file exists
	int fd = open(conn->request_path + 1, O_RDONLY);

	// if not, return error
	if (fd < 0)
		return RESOURCE_TYPE_NONE;

	// set the conn fields
	conn->fd = fd;
	conn->file_size = lseek(fd, 0L, SEEK_END);
	lseek(fd, 0L, SEEK_SET);
	conn->send_len = conn->file_size;

	// check the resource type
	if (strstr(conn->request_path, "/static/") == conn->request_path) {
		memcpy(conn->filename, conn->request_path + strlen("/static/"), strlen(conn->request_path) - strlen("/static/"));
		return RESOURCE_TYPE_STATIC;
	} else if (strstr(conn->request_path, "/dynamic/") == conn->request_path) {
		memcpy(conn->filename, conn->request_path + strlen("/dynamic/"), strlen(conn->request_path) - strlen("/dynamic/"));
		conn->ctx = ctx;

		int efd = eventfd(0, EFD_NONBLOCK);

		DIE(efd < 0, "eventfd");
		conn->eventfd = efd;

		// create epoll event
		struct epoll_event epoll_event;

		epoll_event.events = EPOLLOUT;
		epoll_event.data.fd = conn->eventfd;
		epoll_event.data.ptr = conn;

		// add eventfd to epoll
		epoll_ctl(epollfd, EPOLL_CTL_ADD, conn->eventfd, &epoll_event);

		return RESOURCE_TYPE_DYNAMIC;
	}

	return RESOURCE_TYPE_NONE;
}

struct connection *connection_create(int sockfd)
{
	// allocate connection
	struct connection *conn = calloc(1, sizeof(*conn));

	DIE(conn == NULL, "malloc");

	// initialize fields
	conn->sockfd = sockfd;
	memset(conn->recv_buffer, 0, BUFSIZ);
	memset(conn->send_buffer, 0, BUFSIZ);
	conn->state = STATE_INITIAL;

	return conn;
}

void connection_start_async_io(struct connection *conn)
{
	// prepare io operation
	io_prep_pread(&conn->iocb, conn->fd, conn->send_buffer, BUFSIZ, conn->file_pos);
	conn->piocb[0] = &conn->iocb;
	conn->send_len = (conn->file_size - conn->file_pos) > BUFSIZ ? BUFSIZ : (conn->file_size - conn->file_pos);
	// dlog(LOG_INFO, "send_len = %ld bytes\n", conn->send_len);

	// submit io request
	if (io_submit(conn->ctx, 1, conn->piocb) < 0) {
		dlog(LOG_INFO, "io_submit error\n");
		io_destroy(conn->ctx);
		close(conn->fd);
		return;
	}
}

void connection_remove(struct connection *conn)
{
	// remove connection handler
	close(conn->sockfd);
	conn->state = STATE_CONNECTION_CLOSED;
	w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
}

void handle_new_connection(void)
{
	static int sockfd;
	socklen_t addrlen = sizeof(struct sockaddr_in);
	struct sockaddr_in addr;
	struct connection *conn;
	int rc;

	// accept connection
	sockfd = accept(listenfd, (struct sockaddr *)&addr, &addrlen);
	DIE(sockfd < 0, "accept");

	// set socket to non-blocking
	rc = fcntl(sockfd, F_SETFL, fcntl(sockfd, F_GETFL, 0) | O_NONBLOCK);
	DIE(rc < 0, "fcntl");

	// create connection
	conn = connection_create(sockfd);

	// add connection to epoll
	rc = w_epoll_add_ptr_in(epollfd, sockfd, conn);
	DIE(rc < 0, "w_epoll_add_ptr");

	// initialize parser
	http_parser_init(&conn->request_parser, HTTP_REQUEST);
	conn->request_parser.data = conn;
}

void receive_data(struct connection *conn)
{
	// declare buffer
	char buffer[BUFSIZ];

	// receive data in the buffer
	while (1) {
		int recv_len = recv(conn->sockfd, buffer, BUFSIZ, 0);

		if (recv_len <= 0) {
			dlog(LOG_INFO, "finished receiving on port %d\n", AWS_LISTEN_PORT);
			break;
		}

		// store data
		strncat(conn->recv_buffer, buffer, strlen(buffer) + 1);

		conn->recv_len += recv_len;

		dlog(LOG_INFO, "received %s\n", conn->recv_buffer);

		conn->state = STATE_RECEIVING_DATA;
	}
}

int connection_open_file(struct connection *conn)
{
	/* TODO: Open file and update connection fields. */

	return -1;
}

void connection_complete_async_io(struct connection *conn)
{
	struct io_event events[1];

	int rc = io_getevents(conn->ctx, 1, 1, events, NULL);

	if (rc < 0) {
		perror("io_getevents");
		connection_remove(conn);
	}

	conn->file_pos += conn->send_len;

	// notify io completion
	uint64_t val = 1;

	rc = write(conn->eventfd, &val, sizeof(uint64_t));
	DIE(rc < 0, "write fail");

	connection_send_data(conn);
}

int parse_header(struct connection *conn)
{
	http_parser_settings settings_on_path = {
		.on_message_begin = NULL,
		.on_header_field = NULL,
		.on_header_value = NULL,
		.on_path = aws_on_path_cb,
		.on_url = NULL,
		.on_fragment = NULL,
		.on_query_string = NULL,
		.on_body = NULL,
		.on_headers_complete = NULL,
		.on_message_complete = NULL
	};

	http_parser_execute(&conn->request_parser, &settings_on_path, conn->recv_buffer, conn->recv_len);

	// check for message type
	if (conn->request_parser.type != HTTP_REQUEST) {
		ERR("Not a HTTP request\n");
		return -1;
	} else if (conn->have_path == 0) {
		return 0;
	} else if (conn->have_path == 1) { // received data
		dlog(LOG_INFO, "path: %s\n", conn->request_path);

		// check for file type
		enum resource_type type = connection_get_resource_type(conn);

		if (type == RESOURCE_TYPE_NONE) {
			dlog(LOG_INFO, "Unknown resource type\n");
			return -1;
		} else if (type == RESOURCE_TYPE_STATIC) {
			dlog(LOG_INFO, "static resource\n");
			conn->res_type = RESOURCE_TYPE_STATIC;
		} else if (type == RESOURCE_TYPE_DYNAMIC) {
			dlog(LOG_INFO, "dynamic resource\n");
			conn->res_type = RESOURCE_TYPE_DYNAMIC;
		}
	}

	return 0;
}

enum connection_state connection_send_static(struct connection *conn)
{
	dlog(LOG_INFO, "sending %ld bytes\n", conn->file_size);
	conn->send_len = conn->file_size;
	// send data
	while (1) {
		int size_to_send = conn->send_len > BUFSIZ ? BUFSIZ : conn->send_len;

		dlog(LOG_INFO, "sending %d bytes\n", size_to_send);

		int rc = sendfile(conn->sockfd, conn->fd, 0, size_to_send);

		conn->send_len -= rc;

		if (conn->send_len == 0)
			break;
	}

	return STATE_NO_STATE;
}

int connection_send_data(struct connection *conn)
{
	int sent = 0;

	// send data
	while (conn->send_len > 0) {
		dlog(LOG_INFO, "sending %ld bytes\n", conn->send_len);
		// send as much data as possible
		int rc = send(conn->sockfd, conn->send_buffer + sent, conn->send_len, 0);

		if (rc < 0) {
			dlog(LOG_INFO, "send err");
			conn->state = STATE_NO_STATE;

			return -1;
		}

		conn->send_len -= rc;
		sent += rc;
	}

	return 0;
}

int connection_send_dynamic(struct connection *conn)
{
	// start reading the file
	connection_start_async_io(conn);
	// finish reading the file
	connection_complete_async_io(conn);
	return 0;
}

void handle_input(struct connection *conn)
{
	char addr_buffer[64];
	int rc;

	rc = get_peer_address(conn->sockfd, addr_buffer, 64);
	if (rc < 0) {
		ERR("get_peer_address");
		connection_remove(conn);
		return;
	}

	// get the information
	receive_data(conn);
	dlog(LOG_INFO, "received %ld bytes from %s\n", conn->recv_len, addr_buffer);
	conn->state = STATE_REQUEST_RECEIVED;

	// process data
	rc = parse_header(conn);
	if (rc < 0) {
		dlog(LOG_INFO, "404 bro\n");
		conn->state = STATE_SENDING_404;
	} else {
		conn->state = STATE_SENDING_HEADER;
	}

	switch (conn->state) {
	case STATE_SENDING_HEADER:
		dlog(LOG_INFO, "sending header\n");
		connection_prepare_send_reply_header(conn);
		conn->state = STATE_SENDING_DATA;
		break;

	case STATE_SENDING_404:
		dlog(LOG_INFO, "sending 404\n");
		connection_prepare_send_404(conn);
		break;

	default:
		printf("shouldn't get here %d\n", conn->state);
	}

	// send data
	int sent = 0;

	while (conn->send_len > 0) {
		dlog(LOG_INFO, "sending %ld bytes\n", conn->send_len);
		rc = send(conn->sockfd, conn->send_buffer + sent, conn->send_len, 0);
		if (rc < 0) {
			dlog(LOG_INFO, "send err");
			conn->state = STATE_NO_STATE;

			return;
		}

		conn->send_len -= rc;
		sent += rc;

		if (!conn->send_len && conn->state == STATE_SENDING_404) {
			dlog(LOG_INFO, "finished sending\n");
			return;
		}
	}

	memset(conn->send_buffer, 0, BUFSIZ);

	// depending on the resource type, send data accrodingly
	if (conn->state == STATE_SENDING_DATA) {
		switch (conn->res_type) {
		case RESOURCE_TYPE_STATIC:
			conn->state = STATE_SENDING_DATA;
			dlog(LOG_INFO, "sending static\n");
			connection_send_static(conn);
			conn->state = STATE_NO_STATE;
			break;
		case RESOURCE_TYPE_DYNAMIC:
			conn->state = STATE_SENDING_DATA;
			dlog(LOG_INFO, "sending dynamic\n");
			connection_send_dynamic(conn);
			conn->state = STATE_NO_STATE;
			break;
		default:
			ERR("Unknown resource type\n");
			conn->state = STATE_NO_STATE;
			return;
		}
	}
}

void handle_output(struct connection *conn)
{
	// finished an io operation
	dlog(LOG_INFO, "handle output\n");

	// file has finished sending
	if (conn->file_pos == conn->file_size) {
		dlog(LOG_INFO, "finished sending\n");
		conn->state = STATE_NO_STATE;
		w_epoll_remove_fd(epollfd, conn->eventfd);
	} else {
		// if not, continue
		dlog(LOG_INFO, "continuing\n");
		connection_send_dynamic(conn);
		handle_output(conn);
	}

	dlog(LOG_INFO, "finished sending\n");
}

void handle_client(uint32_t event, struct connection *conn)
{
	if (event & EPOLLIN) {
		handle_input(conn);
	} else if (event & EPOLLOUT) {
		handle_output(conn);
		connection_remove(conn);
	} else {
		ERR("Unexpected event\n");
		exit(1);
	}

	// if the resource was static and it was sent, remove the connection
	if (conn->res_type == RESOURCE_TYPE_STATIC && conn->state == STATE_NO_STATE)
		connection_remove(conn);

	// if the request file was not found, remove the connection
	if (conn->state == STATE_SENDING_404)
		connection_remove(conn);
}

int main(void)
{
	int rc;

	// initialize io context
	io_setup(128, &ctx);

	// initialize epoll
	epollfd = w_epoll_create();
	DIE(epollfd < 0, "w_epoll_create");

	dlog(LOG_INFO, "epollfd: %d\n", epollfd);
	// initialize server socket
	listenfd = tcp_create_listener(AWS_LISTEN_PORT, DEFAULT_LISTEN_BACKLOG);
	DIE(listenfd < 0, "tcp_create_listener");

	dlog(LOG_INFO, "listenfd: %d\n", listenfd);

	// add server socket to epoll
	rc = w_epoll_add_fd_in(epollfd, listenfd);
	DIE(rc < 0, "w_epoll_add_fd_in");

	dlog(LOG_INFO, "listenfd added to epoll\n");

	dlog(LOG_INFO, "Server waiting for connections on port %d\n", AWS_LISTEN_PORT);

	// server main loop
	while (1) {
		struct epoll_event events[50];

		// waiting for events
		rc = w_epoll_wait_infinite(epollfd, events);

		// iterate through events
		for (int i = 0; i < rc; i++) {
			struct epoll_event rev = events[i];

			dlog(LOG_INFO, "event on fd %d\n", rev.data.fd);
			if (rev.data.fd == listenfd) {
				dlog(LOG_INFO, "handle new connection\n");
				handle_new_connection();
			} else {
				dlog(LOG_INFO, "handle client\n");
				struct connection *conn = (struct connection *)rev.data.ptr;

				handle_client(rev.events, conn);
			}
		}
	}
	return 0;
}
