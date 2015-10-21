/* amqp_proxy.c -- socket server for security filtering
 *
 * Copyright (c) 2015 Petr Gotthard <petr.gotthard@centrum.cz>
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <string.h>
#include <errno.h>

#include <arpa/inet.h>
#include <unistd.h>
#include <ctype.h>
#include <getopt.h>
#include <time.h>

#include "amqp_filter.h"

#define SESSION_MAX	10
#define FRAME_MAX	262144 /* RabbitMQ default value is 131072 */
#define LISTEN_QUEUE	5

#define DEFAULT_PROXY_PORT	5672

#define TRUE             1
#define FALSE            0

struct buffer
{
	uint8_t buff[FRAME_MAX];

	uint8_t *ptr; /* pointer for reading/writing */
	int size; /* number of bytes to read/write */
};

struct _session
{
	enum _state {
		DIRECT_CONNECT,
		SOCKS_HEADER,
		SOCKS_REQUEST,
		AMQP_HEADER,
		AMQP_FRAME
	} state;

	struct buffer out2in;
	struct buffer in2out;

	struct timeval start_time; /* to calculate duration of the session */
	struct statistics o2i_stats;
	struct statistics i2o_stats;
};

int arg_socks = 0;
struct sockaddr_in6 arg_addr;

int expect_amqp_header(struct pollfd *in_fd, struct pollfd *out_fd, struct _session *sess);
int send_socks_connect_reply(int code, struct pollfd *in_fd, struct pollfd *out_fd, struct _session *sess);

int start_listen(uint16_t proxy_port)
{
	int listen_fd, rc;
	struct sockaddr_in6 addr;
	const int on = 1;

	listen_fd = socket(AF_INET6, SOCK_STREAM, 0);
	if(listen_fd < 0)
	{
		perror("socket() failed");
		exit(-1);
	}

	rc = setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, (char *)&on, sizeof(on));
	if(rc < 0)
	{
		perror("setsockopt() failed");
		close(listen_fd);
		exit(-1);
	}

	/* Set socket to be nonblocking. All of the sockets for the incoming connections will
	 * also be nonblocking since they will inherit that state from the listening socket. */
	rc = ioctl(listen_fd, FIONBIO, (char *)&on);
	if(rc < 0)
	{
		perror("ioctl() failed");
		close(listen_fd);
		exit(-1);
	}

	memset(&addr, 0, sizeof(addr));
	addr.sin6_family = AF_INET6;
	memcpy(&addr.sin6_addr, &in6addr_any, sizeof(in6addr_any));
	addr.sin6_port = htons(proxy_port);

	rc = bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr));
	if (rc < 0)
	{
		perror("bind() failed");
		close(listen_fd);
		exit(-1);
	}

	rc = listen(listen_fd, LISTEN_QUEUE);
	if (rc < 0)
	{
		perror("listen() failed");
		close(listen_fd);
		exit(-1);
	}

	return listen_fd;
}

int start_connect(int family, struct sockaddr *addr, size_t addr_size)
{
	int connect_fd, rc;
	const int on = 1;

	connect_fd = socket(family, SOCK_STREAM, 0);
	if(connect_fd < 0)
	{
		perror("socket() failed");
		exit(-1);
	}

	/* set socket to be nonblocking */
	rc = ioctl(connect_fd, FIONBIO, (char *)&on);
	if(rc < 0)
	{
		perror("ioctl() failed");
		close(connect_fd);
		exit(-1);
	}

	rc = connect(connect_fd, addr, addr_size);
	if (rc < 0)
	{
		if (errno != EINPROGRESS)
		{
			perror("connect() failed");
			close(connect_fd);
			exit(-1);
		}
	}

	return connect_fd;
}

int session_created(int sesno, struct pollfd *in_fd, struct pollfd *out_fd, struct _session *sess)
{
	msglog(0, "[%i.?] <new session>\n", sesno);

	if(arg_socks)
	{
		sess->state = SOCKS_HEADER;

		in_fd->events = POLLIN; /* waiting for SOCKS header */
		out_fd->events = 0; /* no communication */

		sess->in2out.ptr = sess->in2out.buff;
		sess->in2out.size = 2;
	}
	else
	{
		sess->state = DIRECT_CONNECT;

		/* this is a direct proxy, connect to the given address */
		out_fd->fd = start_connect(AF_INET6, (struct sockaddr *)&arg_addr, sizeof(arg_addr));

		in_fd->events = 0; /* no communication */
		out_fd->events = POLLOUT; /* waiting for outward connection */

		sess->in2out.size = 0;
	}

	/* initialize statistics */
	gettimeofday(&sess->start_time, 0);
	memset(&sess->o2i_stats, 0, sizeof(struct statistics));
	memset(&sess->i2o_stats, 0, sizeof(struct statistics));

	return 0;
}

int stop_session(int sesno, struct pollfd *in_fd, struct pollfd *out_fd, struct _session *sess)
{
	struct timeval now;
	double seconds;

	msglog(0, "[%i.?] <stop session>\n", sesno);

	gettimeofday(&now, 0);
	/* calculate duration of the session */
	if (sess->start_time.tv_usec > now.tv_usec)
	{
		int usec = sess->start_time.tv_usec - now.tv_usec;
		seconds = (now.tv_sec - sess->start_time.tv_sec)-1 + (double)usec/1000000;
	}
	else
	{
		int usec = now.tv_usec - sess->start_time.tv_usec;
		seconds = (now.tv_sec - sess->start_time.tv_sec) + (double)usec/1000000;
	}

	printf("session duration: %f\n", seconds);
	print_statistics(1 /* in */, &sess->o2i_stats);
	print_statistics(0 /* out */, &sess->i2o_stats);

	if(in_fd->fd >= 0)
	{
		close(in_fd->fd);
		in_fd->fd = -1;
	}

	if(out_fd->fd >= 0)
	{
		close(out_fd->fd);
		out_fd->fd = -1;
	}

	return 0;
}

int inner_closed(int sesno, struct pollfd *in_fd, struct pollfd *out_fd, struct _session *sess)
{
	msglog(1, "[%i.?] <session closed>\n", sesno);
	return stop_session(sesno, in_fd, out_fd, sess);
}

int inner_failure(int sesno, struct pollfd *in_fd, struct pollfd *out_fd, struct _session *sess)
{
	msglog(1, "[%i.?] <session failure>\n", sesno);
	return stop_session(sesno, in_fd, out_fd, sess);
}

int outer_connect_ready(int sesno, struct pollfd *in_fd, struct pollfd *out_fd, struct _session *sess)
{
	msglog(1, "[%i.?] <connected>\n", sesno);

	if(sess->state == DIRECT_CONNECT)
		return expect_amqp_header(in_fd, out_fd, sess);

	send_socks_connect_reply(0, in_fd, out_fd, sess); /* OK */
	return 0;
}

int outer_connect_failed(int sesno, struct pollfd *in_fd, struct pollfd *out_fd, struct _session *sess)
{
	msglog(1, "[%i.?] <connect failed>\n", sesno);
	return stop_session(sesno, in_fd, out_fd, sess);
}

int outer_closed(int sesno, struct pollfd *in_fd, struct pollfd *out_fd, struct _session *sess)
{
	msglog(0, "[%i.?] <session closed>\n", sesno);
	return stop_session(sesno, in_fd, out_fd, sess);
}

int outer_failure(int sesno, struct pollfd *in_fd, struct pollfd *out_fd, struct _session *sess)
{
	msglog(0, "[%i.?] <session failure>\n", sesno);
	return stop_session(sesno, in_fd, out_fd, sess);
}

int in2out_received_socks_header(int sesno, struct pollfd *in_fd, struct pollfd *out_fd, struct _session *sess)
{
	int i;
	/* fix the header size */
	if(sess->in2out.size == 2)
		sess->in2out.size = 2 + sess->in2out.buff[1]; /* NMETHODS */

	/* need to continue reading the rest of the header */
	if(sess->in2out.ptr-sess->in2out.buff < sess->in2out.size)
		return 0;

	if(is_verbose)
	{
		msglog(0, "[%i.?] <SOCKS%i header>\n", sesno, sess->in2out.buff[0]);
		printf("\tauth:");
		for(i = 0; i < sess->in2out.buff[1]; i++)
			printf(" %i", sess->in2out.buff[3+i]);
		printf("\n");
	}

	in_fd->events &= ~POLLIN; /* stop reading */
	in_fd->events |= POLLOUT; /* start writing */

	/* write reply */
	sess->out2in.buff[0] = '\x5';
	sess->out2in.buff[1] = '\x0'; /* NO AUTHENTICATION REQUIRED */
	sess->out2in.ptr = sess->out2in.buff;
	sess->out2in.size = 2;

	return 0;
}

int in2out_received_socks_request(int sesno, struct pollfd *in_fd, struct pollfd *out_fd, struct _session *sess)
{
	struct sockaddr_in addr4;
	struct sockaddr_in6 addr6;
	int family;
	char straddr[256];

	/* fix the header size */
	if(sess->in2out.size == 5)
	{
		switch(sess->in2out.buff[3])
		{
		case 1: /* IP V4 address */
			sess->in2out.size = 4 + 4 + 2;
			break;
		case 3: /* DOMAINNAME */
			sess->in2out.size = 4 + (1 + sess->in2out.buff[4]) + 2;
			break;
		case 4: /* IP V6 address */
			sess->in2out.size = 4 + 16 + 2;
			break;
		}
	}

	/* need to continue reading the rest of the header */
	if(sess->in2out.ptr-sess->in2out.buff < sess->in2out.size)
		return 0;

	/* hexdump(sess->in2out.buff, sess->in2out.size); */

	if(sess->in2out.buff[0] != 5 || sess->in2out.buff[1] != 1)
	{
		msglog(0, "[%i.?] SOCKS%i unknown request(%i)\n", sesno,
			sess->in2out.buff[0], sess->in2out.buff[1]);
		send_socks_connect_reply(7, in_fd, out_fd, sess); /* Command not supported */
		return 0;
	}

	switch(sess->in2out.buff[3])
	{
	case 1: /* IP V4 address */
		family = addr4.sin_family = AF_INET;
		memcpy(&addr4.sin_addr, sess->in2out.buff+4, 4);
		memcpy(&addr4.sin_port, sess->in2out.buff+4+4, 2);
		break;
	case 3: /* DOMAINNAME */
		memcpy(straddr, sess->in2out.buff+5, sess->in2out.buff[4]);
		straddr[sess->in2out.buff[4]] = 0;
		/* we do not support domain names, but we do support text IP addresses
		 * in this field; this is used e.g. by PuTTY */
		msglog(0, "[%i.?] SOCKS5 CONNECT (host) to %s\n", sesno, straddr);
		if(isdigit(straddr[0]))
		{
			family = addr4.sin_family = AF_INET;
			inet_pton(AF_INET, straddr, &addr4.sin_addr);
			memcpy(&addr4.sin_port, sess->in2out.buff+4+(1+sess->in2out.buff[4]), 2);
		}
		else
			family = -1;
		break;
	case 4: /* IP V6 address */
		family = addr6.sin6_family = AF_INET6;
		memcpy(&addr6.sin6_addr, sess->in2out.buff+4, 16);
		memcpy(&addr6.sin6_port, sess->in2out.buff+4+16, 2);
		break;
	default:
		family = -1;
		break;
	}

	if(family == AF_INET)
	{
		msglog(0, "[%i.?] SOCKS5 CONNECT (IPv4) to %s:%i\n", sesno,
			inet_ntop(AF_INET, &addr4.sin_addr, straddr, INET_ADDRSTRLEN+1),
			ntohs(addr4.sin_port));
		out_fd->fd = start_connect(AF_INET, (struct sockaddr *)&addr4, sizeof(addr4));
	}
	else if(family == AF_INET6)
	{
		msglog(0, "[%i.?] SOCKS5 CONNECT (IPv6) to [%s]:%i\n", sesno,
			inet_ntop(AF_INET6, &addr6.sin6_addr, straddr, INET6_ADDRSTRLEN+1),
			ntohs(arg_addr.sin6_port));
		out_fd->fd = start_connect(AF_INET6, (struct sockaddr *)&addr6, sizeof(addr6));
	}
	else
	{
		send_socks_connect_reply(8, in_fd, out_fd, sess); /* Address type not supported */
		return 0;
	}

	in_fd->events = 0; /* no communication */
	out_fd->events = POLLOUT; /* waiting for outward connection */

	sess->in2out.size = 0;

	return 0;
}

int send_socks_connect_reply(int code, struct pollfd *in_fd, struct pollfd *out_fd, struct _session *sess)
{
	in_fd->events = POLLOUT; /* write reply */
	out_fd->events = 0; /* no communication */

	sess->out2in.ptr = sess->out2in.buff;
	sess->out2in.size = 4+4+2;

	memset(sess->out2in.buff, 0, sess->out2in.size);

	sess->out2in.buff[0] = 5; /* SOCKS version */
	sess->out2in.buff[1] = code;
	sess->out2in.buff[3] = 1; /* IPv4 */
	/* FIXME: we don't set any address here */

	return 0;
}

int expect_amqp_header(struct pollfd *in_fd, struct pollfd *out_fd, struct _session *sess)
{
	sess->state = AMQP_HEADER;

	in_fd->events = POLLIN; /* expecting client to start the communication */
	out_fd->events = 0; /* no communication */

	sess->in2out.ptr = sess->in2out.buff;
	sess->in2out.size = 8; /* protocol header */

	return 0;
}

int in2out_received_amqp_header(int sesno, struct pollfd *in_fd, struct pollfd *out_fd, struct _session *sess)
{
	msglog(0, "[%i.?] <AMQP header>\n", sesno);
	hexdump(sess->in2out.buff, sess->in2out.size);

	sess->i2o_stats.bytes_total += sess->in2out.size;
	/* FIXME: the full version should be checked here
	 * but the .NET client using 0-9-1 protocol is sending the 0-8 header */
	if(memcmp(&sess->in2out.buff, "AMQP", 4) != 0)
	{
		printf("this is NOT AMQP 0-9-1\n");

		stop_session(sesno, in_fd, out_fd, sess);
		return 0;
	}

	in_fd->events &= ~POLLIN; /* stop reading */
	out_fd->events |= POLLOUT; /* start writing */

	sess->in2out.ptr = sess->in2out.buff;

	return 0;
}

int in2out_received_amqp_frame(int sesno, struct pollfd *in_fd, struct pollfd *out_fd, struct _session *sess)
{
	struct amqp09_frame_header *hdr = (struct amqp09_frame_header *)&sess->in2out.buff;
	/* fix the frame size */
	if(sess->in2out.size == 7)
		sess->in2out.size = 7 + ntohl(hdr->size) + 1;

	if(sess->in2out.ptr-sess->in2out.buff < sess->in2out.size)
	{
		/* need to continue reading the rest of the frame */
		if(sess->in2out.size > FRAME_MAX)
		{
			msglog(0, "[%i.?] too large frame: %i\n", sesno, sess->in2out.size);
			stop_session(sesno, in_fd, out_fd, sess);
		}

		return 0;
	}

	sess->i2o_stats.bytes_total += sess->in2out.size;
	sess->i2o_stats.frames_total++;
	/* received complete AMQP frame */
	switch(filter_frame(0 /* out */, sesno, hdr, sess->in2out.buff + 7, &sess->i2o_stats))
	{
	case 1: /* pass frame */
		in_fd->events &= ~POLLIN; /* stop reading */
		out_fd->events |= POLLOUT; /* start writing */

		sess->in2out.ptr = sess->in2out.buff;

		return 0;

	case 0: /* trash frame */
		in_fd->events |= POLLIN; /* start reading */
		out_fd->events &= ~POLLOUT; /* stop writing */

		sess->in2out.ptr = sess->in2out.buff;
		sess->in2out.size = 7; /* protocol header */

		msglog(0, "[%i.?] frame trashed\n", sesno);
		sess->i2o_stats.frames_trashed++;
		return 0;

	default:
	case -1: /* kill connection */
		stop_session(sesno, in_fd, out_fd, sess);
		return 0;
	}
}

int in2out_received(int sesno, struct pollfd *in_fd, struct pollfd *out_fd, struct _session *sess)
{
	switch(sess->state)
	{
	case SOCKS_HEADER:
		return in2out_received_socks_header(sesno, in_fd, out_fd, sess);
	case SOCKS_REQUEST:
		return in2out_received_socks_request(sesno, in_fd, out_fd, sess);
	case AMQP_HEADER:
		return in2out_received_amqp_header(sesno, in_fd, out_fd, sess);
	case AMQP_FRAME:
	default:
		return in2out_received_amqp_frame(sesno, in_fd, out_fd, sess);
	}
}

int in2out_written_amqp_header(struct pollfd *in_fd, struct pollfd *out_fd, struct _session *sess)
{
	/* start reading AMQP frames */
	sess->state = AMQP_FRAME;

	in_fd->events = POLLIN; /* read in2out */
	sess->in2out.ptr = sess->in2out.buff;
	sess->in2out.size = 7; /* protocol header */

	out_fd->events = POLLIN; /* read out2in */
	sess->out2in.ptr = sess->out2in.buff;
	sess->out2in.size = 7; /* protocol header */

	return 0;
}

int in2out_written_amqp_frame(struct pollfd *in_fd, struct pollfd *out_fd, struct _session *sess)
{
	in_fd->events |= POLLIN; /* start reading */
	out_fd->events &= ~POLLOUT; /* stop writing */

	sess->in2out.ptr = sess->in2out.buff;
	sess->in2out.size = 7; /* protocol header */

	return 0;
}

int in2out_written(struct pollfd *in_fd, struct pollfd *out_fd, struct _session *sess)
{
	switch(sess->state)
	{
	case AMQP_HEADER:
		return in2out_written_amqp_header(in_fd, out_fd, sess);
	case AMQP_FRAME:
		return in2out_written_amqp_frame(in_fd, out_fd, sess);
	default:
		assert(0);
	}

	return 0;
}

int out2in_received(int sesno, struct pollfd *in_fd, struct pollfd *out_fd, struct _session *sess)
{
	struct amqp09_frame_header *hdr = (struct amqp09_frame_header *)&sess->out2in.buff;
	/* fix the frame size */
	if(sess->out2in.size == 7)
		sess->out2in.size = 7 + ntohl(hdr->size) + 1;

	if(sess->out2in.ptr-sess->out2in.buff < sess->out2in.size)
	{
		/* need to continue reading the rest of the frame */
		if(sess->out2in.size > FRAME_MAX)
		{
			msglog(1, "[%i.?] too large frame: %i\n", sesno, sess->out2in.size);
			stop_session(sesno, in_fd, out_fd, sess);
		}

		return 0;
	}

	sess->o2i_stats.bytes_total += sess->out2in.size;
	sess->o2i_stats.frames_total++;
	/* received complete AMQP frame */
	switch(filter_frame(1 /* in */, sesno, hdr, sess->out2in.buff + 7, &sess->o2i_stats))
	{
	case 1: /* pass frame */
		out_fd->events &= ~POLLIN; /* stop reading */
		in_fd->events |= POLLOUT; /* start writing */

		sess->out2in.ptr = sess->out2in.buff;

		return 0;

	case 0: /* trash frame */
		out_fd->events |= POLLIN; /* start reading */
		in_fd->events &= ~POLLOUT; /* stop writing */

		sess->out2in.ptr = sess->out2in.buff;
		sess->out2in.size = 7; /* protocol header */

		msglog(1, "[%i.?] frame trashed\n", sesno);
		sess->o2i_stats.frames_trashed++;
		return 0;

	default:
	case -1: /* kill connection */
		stop_session(sesno, in_fd, out_fd, sess);
		return 0;
	}
}

int out2in_written_socks_header(struct pollfd *in_fd, struct pollfd *out_fd, struct _session *sess)
{
	/* start reading the SOCKS request */
	sess->state = SOCKS_REQUEST;

	in_fd->events &= ~POLLOUT; /* stop writing */
	in_fd->events |= POLLIN; /* start reading */

	sess->in2out.ptr = sess->in2out.buff;
	sess->in2out.size = 5; /* VER, CMD, RSV, ATYP, possible DOMAINNAME length */

	return 0;
}

int out2in_written_socks_reply(struct pollfd *in_fd, struct pollfd *out_fd, struct _session *sess)
{
	expect_amqp_header(in_fd, out_fd, sess);
	return 0;
}

int out2in_written_amqp_frame(struct pollfd *in_fd, struct pollfd *out_fd, struct _session *sess)
{
	out_fd->events |= POLLIN; /* start reading */
	in_fd->events &= ~POLLOUT; /* stop writing */

	sess->out2in.ptr = sess->out2in.buff;
	sess->out2in.size = 7; /* protocol header */

	return 0;
}

int out2in_written(struct pollfd *in_fd, struct pollfd *out_fd, struct _session *sess)
{
	switch(sess->state)
	{
	case SOCKS_HEADER:
		return out2in_written_socks_header(in_fd, out_fd, sess);
	case SOCKS_REQUEST:
		return out2in_written_socks_reply(in_fd, out_fd, sess);
	case AMQP_FRAME:
	default:
		return out2in_written_amqp_frame(in_fd, out_fd, sess);
	}

	return 0;
}

/* one listening socket and two sockets per session
 * 0=listening, (1=inner, 2=outer), (3=inner, 4=outer) */
#define SOCKET_MAX 1+2*SESSION_MAX
struct pollfd fds[SOCKET_MAX];
#define LISTEN_SOCKET 0
#define INNER_SOCKET(session) (2*(session)+1)
#define OUTER_SOCKET(session) (2*(session)+2)

/* two frame buffers per session */
struct _session session[SESSION_MAX];

int is_verbose = 0; /* global config: print more output */

int main(int argc, char *argv[])
{
	int opt, i, rc;
	uint16_t proxy_port = DEFAULT_PROXY_PORT;

	/* ----- parse command line parameters */
	while ((opt = getopt(argc, argv, "l:si:p:vh")) != -1)
	{
		switch (opt)
		{
		case 'l':
			proxy_port = atoi(optarg);
			break;
		case 's':
			arg_socks = 1;
			break;
		case 'i':
			arg_addr.sin6_family = AF_INET6;
			inet_pton(AF_INET6, optarg, &arg_addr.sin6_addr);
			break;
		case 'p':
			arg_addr.sin6_port = htons(atoi(optarg));
			break;
		case 'v':
			is_verbose = 1;
			break;
		case 'h':
			printf("Usage: %s [-l port] -i addr6 [-p port] [-v]\n", argv[0]);
			printf("Examples:\n");
			printf("\t%s -l 1234 -i ::ffff:222.1.41.90 -p 5672\n", argv[0]);
			return 0;
		default: /* '?' */
			fprintf(stderr, "Usage: %s [-l port] -i addr6 [-p port] [-v]\n", argv[0]);
			exit(EXIT_FAILURE);
		}
	}

	/* ----- start listening */
	if(arg_socks)
		printf("Awaiting connections on port %i (SOCKS5 proxy)\n", proxy_port);
	else
	{
		char straddr[INET6_ADDRSTRLEN+1];
		printf("Awaiting connections on port %i (direct proxy to [%s]:%i)\n", proxy_port,
			inet_ntop(AF_INET6, &arg_addr.sin6_addr, straddr, INET6_ADDRSTRLEN+1),
			ntohs(arg_addr.sin6_port));
	}

	/* set listening socket */
	fds[LISTEN_SOCKET].fd = start_listen(proxy_port);
	fds[LISTEN_SOCKET].events = POLLIN;

	/* reset other sockets */
	for(i = 1; i < SOCKET_MAX; i++)
		fds[i].fd = -1; /* sockets < 0 will be ignored by poll */

	while(1)
	{
		/* wait forever */
		rc = poll(fds, SOCKET_MAX, -1);
		if(rc < 0)
		{
			perror("  poll() failed");
			break;
		}

		if(fds[LISTEN_SOCKET].revents == POLLIN)
		{
			int new_sd = accept(fds[LISTEN_SOCKET].fd, NULL, NULL);
			if(new_sd >= 0)
			{
				/* find unused session */
				for(i = 0; i < SESSION_MAX; i++)
				{
					if(fds[INNER_SOCKET(i)].fd < 0)
						break;
				}

				if(i < SESSION_MAX)
				{
					fds[INNER_SOCKET(i)].fd = new_sd;
					session_created(i, fds+INNER_SOCKET(i), fds+OUTER_SOCKET(i), &session[i]);
				}
				else
				{
					printf("  Too many connections\n");
					close(new_sd);
				}
			}
			else
				perror("  accept() failed");
		}

		for(i = 0; i < SESSION_MAX; i++)
		{
			/* this is for debugging of the poll() funtionality only
			if(fds[INNER_SOCKET(i)].revents > 0 || fds[OUTER_SOCKET(i)].revents > 0)
			{
				printf("session %i: active sockets [%i%i][%i%i], [%i%i][%i%i]\n", i,
					fds[INNER_SOCKET(i)].events & POLLIN, fds[INNER_SOCKET(i)].events & POLLOUT,
					fds[INNER_SOCKET(i)].revents & POLLIN, fds[INNER_SOCKET(i)].revents & POLLOUT,
					fds[OUTER_SOCKET(i)].events & POLLIN, fds[OUTER_SOCKET(i)].events & POLLOUT,
					fds[OUTER_SOCKET(i)].revents & POLLIN, fds[OUTER_SOCKET(i)].revents & POLLOUT);
			}
			*/

			/* filtering outwards, i.e. from the more secure to the less secure environment */
			if(fds[INNER_SOCKET(i)].revents & POLLIN)
			{
				/* reading in2out */
				rc = recv(fds[INNER_SOCKET(i)].fd, session[i].in2out.ptr,
					session[i].in2out.size + session[i].in2out.buff - session[i].in2out.ptr, 0);

				if(rc > 0)
				{
					session[i].in2out.ptr += rc;
					
					if(session[i].in2out.ptr-session[i].in2out.buff == session[i].in2out.size)
						in2out_received(i, fds+INNER_SOCKET(i), fds+OUTER_SOCKET(i), &session[i]);
				}
				else if(rc == 0)
				{
					inner_closed(i, fds+INNER_SOCKET(i), fds+OUTER_SOCKET(i), &session[i]);
				}
				else /* rc < 0 */
				{
					if (errno != EWOULDBLOCK)
					{
						perror("  recv() failed");
						inner_failure(i, fds+INNER_SOCKET(i), fds+OUTER_SOCKET(i), &session[i]);
					}
				}
			}
			else if(fds[INNER_SOCKET(i)].revents & POLLERR || fds[INNER_SOCKET(i)].revents & POLLHUP)
			{
				inner_failure(i, fds+INNER_SOCKET(i), fds+OUTER_SOCKET(i), &session[i]);
			}
			else if(fds[OUTER_SOCKET(i)].revents & POLLOUT)
			{
				if(session[i].in2out.size == 0)
				{
					int err;
					socklen_t len = sizeof(err);
					/* get the status */
					getsockopt(fds[OUTER_SOCKET(i)].fd, SOL_SOCKET, SO_ERROR, &err, &len);
					if(err == 0)
						outer_connect_ready(i, fds+INNER_SOCKET(i), fds+OUTER_SOCKET(i), &session[i]);
					else
						outer_connect_failed(i, fds+INNER_SOCKET(i), fds+OUTER_SOCKET(i), &session[i]);
				}
				else
				{
					/* writing in2out */
					rc = send(fds[OUTER_SOCKET(i)].fd, session[i].in2out.ptr,
						session[i].in2out.size + session[i].in2out.buff - session[i].in2out.ptr, 0);
					if(rc > 0)
					{
						session[i].in2out.ptr += rc;

						if(session[i].in2out.ptr-session[i].in2out.buff == session[i].in2out.size)
							in2out_written(fds+INNER_SOCKET(i), fds+OUTER_SOCKET(i), &session[i]);
					}
					else if(rc < 0)
					{
						perror("  send() failed");
						outer_failure(i, fds+INNER_SOCKET(i), fds+OUTER_SOCKET(i), &session[i]);
					}
				}
			}
			else if(fds[OUTER_SOCKET(i)].revents & POLLERR || fds[OUTER_SOCKET(i)].revents & POLLHUP)
			{
				outer_failure(i, fds+INNER_SOCKET(i), fds+OUTER_SOCKET(i), &session[i]);
			}

			/* filtering inwards, i.e. from the less secure to the more secure environment */
			if(fds[OUTER_SOCKET(i)].revents & POLLIN)
			{
				/* reading out2in */
				rc = recv(fds[OUTER_SOCKET(i)].fd, session[i].out2in.ptr,
					session[i].out2in.size + session[i].out2in.buff - session[i].out2in.ptr, 0);

				if(rc > 0)
				{
					session[i].out2in.ptr += rc;
					
					if(session[i].out2in.ptr-session[i].out2in.buff == session[i].out2in.size)
						out2in_received(i, fds+INNER_SOCKET(i), fds+OUTER_SOCKET(i), &session[i]);
				}
				else if(rc == 0)
				{
					outer_closed(i, fds+INNER_SOCKET(i), fds+OUTER_SOCKET(i), &session[i]);
				}
				else /* rc < 0 */
				{
					if (errno != EWOULDBLOCK)
					{
						perror("  recv() failed");
						outer_failure(i, fds+INNER_SOCKET(i), fds+OUTER_SOCKET(i), &session[i]);
					}
				}
			}
			else if(fds[OUTER_SOCKET(i)].revents & POLLERR || fds[OUTER_SOCKET(i)].revents & POLLHUP)
			{
				outer_failure(i, fds+INNER_SOCKET(i), fds+OUTER_SOCKET(i), &session[i]);
			}
			else if(fds[INNER_SOCKET(i)].revents & POLLOUT)
			{
				/* writing out2in */
				rc = send(fds[INNER_SOCKET(i)].fd, session[i].out2in.ptr,
					session[i].out2in.size + session[i].out2in.buff - session[i].out2in.ptr, 0);
				if(rc > 0)
				{
					session[i].out2in.ptr += rc;

					if(session[i].out2in.ptr-session[i].out2in.buff == session[i].out2in.size)
						out2in_written(fds+INNER_SOCKET(i), fds+OUTER_SOCKET(i), &session[i]);
				}
				else if(rc < 0)
				{
					perror("  send() failed");
					inner_failure(i, fds+INNER_SOCKET(i), fds+OUTER_SOCKET(i), &session[i]);
				}
			}
			else if(fds[INNER_SOCKET(i)].revents & POLLERR || fds[INNER_SOCKET(i)].revents & POLLHUP)
			{
				inner_failure(i, fds+INNER_SOCKET(i), fds+OUTER_SOCKET(i), &session[i]);
			}
		}
	}

	return 0;
}

/* end of file */
