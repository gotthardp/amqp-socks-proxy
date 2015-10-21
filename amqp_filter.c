/* filter.c -- message filter for the AMQP 0-9-1
 *
 * Copyright (C) 2015 Petr Gotthard <petr.gotthard@centrum.cz>
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <sys/time.h>
#include <arpa/inet.h>

#include "amqp_filter.h"

void hexdump(void *ptr, int len)
{
	uint8_t *buf = (uint8_t *)ptr;
	int i, j;
	for (i=0; i<len; i+=16)
	{
		printf("%06x: ", i);
		for (j=0; j<16; j++)
			if (i+j < len)
				printf("%02x ", buf[i+j]);
			else
				printf("   ");
		printf(" ");
		for (j=0; j<16; j++)
			if (i+j < len)
				printf("%c", isprint(buf[i+j]) ? buf[i+j] : '.');
		printf("\n");
	}
}

void msglog(int is_inbound, const char *format, ...)
{
	char outstr[20];
	struct timeval tv;
	va_list args;

	gettimeofday(&tv, 0);
	strftime(outstr, sizeof(outstr), "%H:%M:%S", localtime(&tv.tv_sec));
	printf("%s.%03lu %s ", outstr, tv.tv_usec/1000, is_inbound ? "<--" : "-->");

	va_start(args, format);
	vprintf(format, args);
	va_end(args);
}

void print_statistics(int is_inbound, struct statistics *stats)
{
	printf("%s bytes: %llu, frames: %u (%u bad), messages: %u, payload: %llu\n",
		is_inbound ? "<--" : "-->", stats->bytes_total, stats->frames_total, stats->frames_trashed,
		stats->message_count, stats->payload_size);
}

typedef int (*frame_filter_t)(uint8_t *payload, int size);

uint8_t decode_uint8(uint8_t **payload)
{
	uint8_t res = *(uint8_t*)(*payload);
	(*payload) += sizeof(uint8_t);
	return res;
}

uint16_t decode_uint16(uint8_t **payload)
{
	uint16_t res = *(uint16_t*)(*payload);
	(*payload) += sizeof(uint16_t);
	return ntohs(res);
}

void decode_str(uint8_t **payload, char *str)
{
	int len = decode_uint8(payload);
	memcpy(str, (*payload), len);
	str[len] = 0;
	(*payload) += len;
}

#define MAX_NAME_LENGTH 256

int kill_connection(uint8_t *payload, int size)
{
	return -1; /* kill connection */
}

int trash_frame(uint8_t *payload, int size)
{
	return 0; /* trash frame */
}

int filter_exchange_declare(uint8_t *payload, int size)
{
	char exchange[MAX_NAME_LENGTH], type[MAX_NAME_LENGTH];
	uint8_t *ptr = payload;

	decode_uint16(&ptr); /* reserved */
	decode_str(&ptr, exchange);
	decode_str(&ptr, type);

	if(is_verbose)
		printf("\texchange='%s' OF type='%s'\n", exchange, type);

	if(memcmp(exchange, "HACK", 4) == 0)
		return -1; /* kill connection */

	return 1;
}

int filter_queue_declare(uint8_t *payload, int size)
{
	char queue[MAX_NAME_LENGTH];
	uint8_t *ptr = payload;

	decode_uint16(&ptr); /* reserved */
	decode_str(&ptr, queue);

	if(is_verbose)
		printf("\tqueue='%s'\n", queue);

	if(memcmp(queue, "HACK", 4) == 0)
		return -1; /* kill connection */

	return 1;
}

int filter_queue_bind(uint8_t *payload, int size)
{
	char queue[MAX_NAME_LENGTH], exchange[MAX_NAME_LENGTH], routing_key[MAX_NAME_LENGTH];
	uint8_t *ptr = payload;

	decode_uint16(&ptr); /* reserved */
	decode_str(&ptr, queue);
	decode_str(&ptr, exchange);
	decode_str(&ptr, routing_key);

	if(is_verbose)
		printf("\tqueue='%s' TO exchange='%s' WHERE routing-key='%s'\n", queue, exchange, routing_key);
	return 1;
}

struct _method_filters
{
	uint16_t class;
	uint16_t method;
	char *descr;
	frame_filter_t func_in;
	frame_filter_t func_out;
} method_filters[] = {
	{10,10,"Connection.Start", NULL, NULL},
	{10,11,"Connection.Start-Ok", NULL, NULL},
	{10,20,"Connection.Secure", NULL, NULL},
	{10,21,"Connection.Secure-Ok", NULL, NULL},
	{10,30,"Connection.Tune", NULL, NULL},
	{10,31,"Connection.Tune-Ok", NULL, NULL},
	{10,40,"Connection.Open", NULL, NULL},
	{10,41,"Connection.Open-Ok", NULL, NULL},
	{10,50,"Connection.Close", NULL, NULL},
	{10,51,"Connection.Close-Ok", NULL, NULL},
	{20,10,"Channel.Open", NULL, NULL},
	{20,11,"Channel.Open-Ok", NULL, NULL},
	{20,20,"Channel.Flow", NULL, NULL},
	{20,21,"Channel.Flow-Ok", NULL, NULL},
	{20,40,"Channel.Close", NULL, NULL},
	{20,41,"Channel.Close-Ok", NULL, NULL},
	{40,10,"Exchange.Declare", filter_exchange_declare, NULL},
	{40,11,"Exchange.Declare-Ok", NULL, NULL},
	{40,20,"Exchange.Delete", NULL, NULL},
	{40,21,"Exchange.Delete-Ok", NULL, NULL},
	{50,10,"Queue.Declare", filter_queue_declare, NULL},
	{50,11,"Queue.Declare-Ok", NULL, NULL},
	{50,20,"Queue.Bind", filter_queue_bind, NULL},
	{50,21,"Queue.Bind-Ok", NULL, NULL},
	{50,50,"Queue.Unbind", NULL, NULL},
	{50,51,"Queue.Unbind-Ok", NULL, NULL},
	{50,30,"Queue.Purge", NULL, NULL},
	{50,31,"Queue.Purge-Ok", NULL, NULL},
	{50,40,"Queue.Delete", NULL, NULL},
	{50,41,"Queue.Delete-Ok", NULL, NULL},
	{60,10,"Basic.Qos", NULL, NULL},
	{60,11,"Basic.Qos-Ok", NULL, NULL},
	{60,20,"Basic.Consume", NULL, NULL},
	{60,21,"Basic.Consume-Ok", NULL, NULL},
	{60,30,"Basic.Cancel", NULL, NULL},
	{60,31,"Basic.Cancel-Ok", NULL, NULL},
	{60,40,"Basic.Publish", NULL, NULL},
	{60,50,"Basic.Return", NULL, NULL},
	{60,60,"Basic.Deliver", NULL, NULL},
	{60,70,"Basic.Get", NULL, NULL},
	{60,71,"Basic.Get-Ok", NULL, NULL},
	{60,72,"Basic.Get-Empty", NULL, NULL},
	{60,80,"Basic.Ack", NULL, NULL},
	{60,90,"Basic.Reject", NULL, NULL},
	{60,100,"Basic.Recover-Async", NULL, NULL},
	{60,110,"Basic.Recover", NULL, NULL},
	{60,111,"Basic.Recover-Ok", NULL, NULL},
	{85,10,"Confirm.Select", NULL, NULL}, /* RabbitMQ extension */
	{85,11,"Confirm.Select-Ok", NULL, NULL}, /* RabbitMQ extension */
	{90,10,"Tx.Select", NULL, NULL},
	{90,11,"Tx.Select-Ok", NULL, NULL},
	{90,20,"Tx.Commit", NULL, NULL},
	{90,21,"Tx.Commit-Ok", NULL, NULL},
	{90,30,"Tx.Rollback", NULL, NULL},
	{90,31,"Tx.Rollback-Ok", NULL, NULL},
	{0,0,NULL, NULL, NULL}
};

int filter_method_frame(int is_inbound, int sesno, struct amqp09_frame_header *hdr, uint8_t *payload, struct statistics *stats)
{
	struct amqp09_method_header *mtd = (struct amqp09_method_header *)payload;
	struct _method_filters *filter;

	for(filter = method_filters; filter->descr != NULL; filter++)
	{
		if(filter->class == ntohs(mtd->class) &&
			filter->method == ntohs(mtd->method))
		{
			if(is_verbose)
				msglog(is_inbound, "[%i.%i] METHOD %s\n", sesno, ntohs(hdr->channel), filter->descr);

			if(is_inbound)
			{
				if(filter->func_in)
					return filter->func_in(payload+4, ntohl(hdr->size)-4);
				else
					return 1; /* pass */
			}
			else /* !is_inbound */
			{
				if(filter->func_out)
					return filter->func_out(payload+4, ntohl(hdr->size)-4);
				else
					return 1; /* pass */
			}
		}
	}

	msglog(is_inbound, "[%i.%i] unknown method(%i.%i)\n", sesno, ntohs(hdr->channel),
		ntohs(mtd->class), ntohs(mtd->method));
	return 0; /* trash frame */
}

int filter_header_frame(int is_inbound, int sesno, struct amqp09_frame_header *hdr, uint8_t *payload, struct statistics *stats)
{
	if(is_verbose)
		msglog(is_inbound, "[%i.%i] HEADER\n", sesno, ntohs(hdr->channel));

	/* Messages always have a content header and zero or more content body frames. */
	stats->message_count++;

	return 1; /* pass */
}

int filter_body_frame(int is_inbound, int sesno, struct amqp09_frame_header *hdr, uint8_t *payload, struct statistics *stats)
{
	if(is_verbose)
	{
		msglog(is_inbound, "[%i.%i] BODY\n", sesno, ntohs(hdr->channel));
		hexdump(payload, ntohl(hdr->size) < 64 ? ntohl(hdr->size) : 64);
	}

	stats->payload_size += ntohl(hdr->size);

	if(memcmp(payload, "HACK", 4) == 0)
		return -1; /* kill connection */

	return 1; /* pass */
}

int filter_heartbeat_frame(int is_inbound, int sesno, struct amqp09_frame_header *hdr)
{
	/* heartbeats destroy log readability */
	/* msglog(is_inbound, "[%i.%i] HEARTBEAT (%li bytes)\n", sesno, ntohs(hdr->channel), ntohl(hdr->size)); */
	return 1; /* pass */
}

int filter_frame(int is_inbound, int sesno, struct amqp09_frame_header *hdr, uint8_t *payload, struct statistics *stats)
{
	switch(hdr->type)
	{
	case 1:
		return filter_method_frame(is_inbound, sesno, hdr, payload, stats);
	case 2:
		return filter_header_frame(is_inbound, sesno, hdr, payload, stats);
	case 3:
		return filter_body_frame(is_inbound, sesno, hdr, payload, stats);
	case 8:
		return filter_heartbeat_frame(is_inbound, sesno, hdr); /* no payload in heartbeat */
	default:
		msglog(is_inbound, "[%i.%i] unknown frame type (%i)\n", sesno, ntohs(hdr->channel), hdr->type);
		return 0; /* trash frame */
	}
}

/* end of file */
