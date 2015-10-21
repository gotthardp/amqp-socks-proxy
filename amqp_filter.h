/* filter.h -- message filter for the AMQP 0-9-1
 *
 * Copyright (C) 2015 Petr Gotthard <petr.gotthard@centrum.cz>
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdint.h>

extern int is_verbose; /* global config: print more output */

struct amqp09_frame_header
{
	uint8_t type;
	uint16_t channel;
	uint32_t size;
} __attribute__ ((__packed__));

struct amqp09_method_header
{
	uint16_t class;
	uint16_t method;
} __attribute__ ((__packed__));

struct statistics
{
	uint64_t bytes_total; /* size of all AMQP frames transmitted */
	size_t frames_total; /* total number of frames transmitted */
	size_t frames_trashed; /* number of trashed frames */
	size_t message_count; /* number of AMQP messages transmitted */
	uint64_t payload_size; /* size of the payload transmitted */
};

void hexdump(void *ptr, int len);
void msglog(int is_inbound, const char *format, ...);
void print_statistics(int is_inbound, struct statistics *stats);
int filter_frame(int is_inbound, int sesno, struct amqp09_frame_header *hdr, uint8_t *payload, struct statistics *stats);

/* end of file */
