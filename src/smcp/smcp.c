/*	@file smcp.c
**	@author Robert Quattlebaum <darco@deepdarc.com>
**
**	Copyright (C) 2011,2012 Robert Quattlebaum
**
**	Permission is hereby granted, free of charge, to any person
**	obtaining a copy of this software and associated
**	documentation files (the "Software"), to deal in the
**	Software without restriction, including without limitation
**	the rights to use, copy, modify, merge, publish, distribute,
**	sublicense, and/or sell copies of the Software, and to
**	permit persons to whom the Software is furnished to do so,
**	subject to the following conditions:
**
**	The above copyright notice and this permission notice shall
**	be included in all copies or substantial portions of the
**	Software.
**
**	THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY
**	KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
**	WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
**	PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS
**	OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
**	OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
**	OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
**	SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#if HAVE_CONFIG_H
#include <config.h>
#endif

#ifndef VERBOSE_DEBUG
#define VERBOSE_DEBUG 0
#endif

#ifndef DEBUG
#define DEBUG VERBOSE_DEBUG
#endif

#define __APPLE_USE_RFC_3542 1

#include "assert_macros.h"

#if CONTIKI
#include "contiki.h"
#include "net/uip-udp-packet.h"
#include "net/uiplib.h"
#include "net/tcpip.h"
#include "net/resolv.h"
extern uint16_t uip_slen;
#endif

#include <stdarg.h>

#include "smcp.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "ll.h"

#include "url-helpers.h"
#include "smcp-node.h"
#include "smcp-logging.h"
#include "smcp-auth.h"

#if SMCP_USE_BSD_SOCKETS
#include <poll.h>
#include <netdb.h>
#include <sys/socket.h>
#include <net/if.h>
#include <sys/sysctl.h>
#include <sys/errno.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "smcp-helpers.h"

#include "smcp-timer.h"
#include "smcp-internal.h"

#pragma mark -

#if SMCP_EMBEDDED
struct smcp_s smcp_global_instance;
#else
static smcp_t smcp_current_instance;
void
smcp_set_current_instance(smcp_t x) {
	smcp_current_instance = (x);
}
smcp_t
smcp_get_current_instance() {
	return smcp_current_instance;
}
#endif

#pragma mark -
#pragma mark SMCP Globals

#if SMCP_NO_MALLOC
static struct smcp_transaction_s smcp_transaction_pool[SMCP_CONF_MAX_TRANSACTIONS];
#endif // SMCP_NO_MALLOC

#pragma mark -
#pragma mark SMCP Implementation

#if !SMCP_NO_MALLOC && !SMCP_EMBEDDED
smcp_t
smcp_create(uint16_t port) {
	smcp_t ret = NULL;

	ret = (smcp_t)calloc(1, sizeof(struct smcp_s));

	require(ret != NULL, bail);

	ret = smcp_init(ret, port);

bail:
	return ret;
}
#endif

smcp_t
smcp_init(
	smcp_t self, uint16_t port
) {
	SMCP_EMBEDDED_SELF_HOOK;

	if(port == 0)
		port = SMCP_DEFAULT_PORT;

#if SMCP_USE_BSD_SOCKETS
	struct sockaddr_in6 saddr = {
#if SOCKADDR_HAS_LENGTH_FIELD
		.sin6_len		= sizeof(struct sockaddr_in6),
#endif
		.sin6_family	= AF_INET6,
		.sin6_port		= htons(port),
	};
#endif

	require(self != NULL, bail);

	// Set up the UDP port for listening.
#if SMCP_USE_BSD_SOCKETS
	uint16_t attempts = 0x7FFF;

	self->mcfd = -1;
	self->fd = -1;
	errno = 0;

	self->fd = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
	int prev_errno = errno;

	require_action_string(
		self->fd >= 0,
		bail, (
			smcp_release(self),
			self = NULL
		),
		strerror(prev_errno)
	);

	// Keep attempting to bind until we find a port that works.
	while(bind(self->fd, (struct sockaddr*)&saddr, sizeof(saddr)) != 0) {
		// We should only continue trying if errno == EADDRINUSE.
		require_action_string(errno == EADDRINUSE, bail,
			{ DEBUG_PRINTF(CSTR("errno=%d"), errno); smcp_release(
				    self); self = NULL; }, "Failed to bind socket");
		port++;

		// Make sure we aren't in an infinite loop.
		require_action_string(--attempts, bail,
			{ DEBUG_PRINTF(CSTR("errno=%d"), errno); smcp_release(
				    self); self = NULL; }, "Failed to bind socket (ran out of ports)");

		saddr.sin6_port = htons(port);
	}

#ifdef IPV6_PREFER_TEMPADDR
#ifndef IP6PO_TEMPADDR_NOTPREFER
#define IP6PO_TEMPADDR_NOTPREFER 0
#endif
	{
		int value = IP6PO_TEMPADDR_NOTPREFER;
		setsockopt(self->fd, IPPROTO_IPV6, IPV6_PREFER_TEMPADDR, &value, sizeof(value));
	}
#endif

#elif CONTIKI
	self->udp_conn = udp_new(NULL, 0, NULL);
	uip_udp_bind(self->udp_conn, htons(port));
	self->udp_conn->rport = 0;
#endif

	// Go ahead and start listening on our multicast address as well.
#if SMCP_USE_BSD_SOCKETS
	{   // Join the multicast group for SMCP_IPV6_MULTICAST_ADDRESS
		struct ipv6_mreq imreq;
		int btrue = 1;
		struct hostent *tmp = gethostbyname2(SMCP_IPV6_MULTICAST_ADDRESS,
			AF_INET6);
		memset(&imreq, 0, sizeof(imreq));
		self->mcfd = socket(AF_INET6, SOCK_DGRAM, 0);

		require(!h_errno && tmp, bail);
		require(tmp->h_length > 1, bail);

		memcpy(&imreq.ipv6mr_multiaddr.s6_addr, tmp->h_addr_list[0], 16);

		require(0 ==
			setsockopt(self->mcfd, IPPROTO_IPV6, IPV6_MULTICAST_LOOP,
				&btrue,
				sizeof(btrue)), bail);

		// Do a precautionary leave group, to clear any stake kernel data.
		setsockopt(self->mcfd,
			IPPROTO_IPV6,
			IPV6_LEAVE_GROUP,
			&imreq,
			sizeof(imreq));

		require(0 ==
			setsockopt(self->mcfd, IPPROTO_IPV6, IPV6_JOIN_GROUP, &imreq,
				sizeof(imreq)), bail);
	}
#endif

	self->is_processing_message = false;
	require_string(
		smcp_node_init(&self->root_node,NULL,NULL) != NULL,
		bail,
		"Unable to initialize root node"
	);

bail:
	return self;
}

void
smcp_release(smcp_t self) {
	SMCP_EMBEDDED_SELF_HOOK;
	require(self, bail);

	// Delete all pending transactions
	while(self->transactions) {
		smcp_transaction_end(self, self->transactions);
	}

	// Delete all timers
	while(self->timers) {
		smcp_timer_t timer = self->timers;
		if(timer->cancel)
			timer->cancel(self, timer->context);
		smcp_invalidate_timer(self, timer);
	}

	// Delete all nodes
	smcp_node_delete(smcp_get_root_node(self));

#if SMCP_USE_BSD_SOCKETS
	if(self->fd>=0)
		close(self->fd);
	if(self->mcfd>=0)
		close(self->mcfd);
#elif CONTIKI
	if(self->udp_conn)
		uip_udp_remove(self->udp_conn);
#endif

#if !SMCP_NO_MALLOC && !SMCP_EMBEDDED
	// TODO: Make sure we were actually alloc'd!
	free(self);
#endif

bail:
	return;
}

uint16_t
smcp_get_port(smcp_t self) {
#if SMCP_USE_BSD_SOCKETS
	SMCP_EMBEDDED_SELF_HOOK;
	struct sockaddr_in6 saddr;
	socklen_t socklen = sizeof(saddr);
	getsockname(self->fd, (struct sockaddr*)&saddr, &socklen);
	return ntohs(saddr.sin6_port);
#elif CONTIKI
	SMCP_EMBEDDED_SELF_HOOK;
	return ntohs(self->udp_conn->lport);
#endif
}

#if SMCP_USE_BSD_SOCKETS
int
smcp_get_fd(smcp_t self) {
	SMCP_EMBEDDED_SELF_HOOK;
	return self->fd;
}
#elif defined(CONTIKI)
struct uip_udp_conn*
smcp_get_udp_conn(smcp_t self) {
	SMCP_EMBEDDED_SELF_HOOK;
	return self->udp_conn;
}
#endif

void
smcp_set_proxy_url(smcp_t self,const char* url) {
	SMCP_EMBEDDED_SELF_HOOK;
	assert(self);
	free((void*)self->proxy_url);
	if(url)
		self->proxy_url = strdup(url);
	else
		self->proxy_url = NULL;
	DEBUG_PRINTF("CoAP Proxy URL set to %s",self->proxy_url);
}

#if !SMCP_EMBEDDED
smcp_node_t
smcp_get_root_node(smcp_t self) {
	return &self->root_node;
}
#endif

#pragma mark -
#pragma mark Inbound packet parsing functions

void
smcp_inbound_reset_next_option() {
	smcp_t const self = smcp_get_current_instance();
	self->inbound.last_option_key = 0;
	self->inbound.this_option = self->inbound.packet->token + self->inbound.packet->token_len;
}

static coap_option_key_t
smcp_inbound_next_option_(const uint8_t** value, size_t* len) {
	smcp_t const self = smcp_get_current_instance();
	if(self->inbound.this_option<((uint8_t*)self->inbound.packet+self->inbound.packet_len)
		&& self->inbound.this_option[0]!=0xFF
	) {
		self->inbound.this_option = coap_decode_option(
			self->inbound.this_option,
			&self->inbound.last_option_key,
			value,
			len
		);
	} else {
		self->inbound.last_option_key = COAP_HEADER_INVALID;
	}
	return self->inbound.last_option_key;
}

coap_option_key_t
smcp_inbound_next_option(const uint8_t** value, size_t* len) {
	coap_option_key_t ret;
	ret = smcp_inbound_next_option_(value, len);
	return ret;
}

coap_option_key_t
smcp_inbound_peek_option(const uint8_t** value, size_t* len) {
	smcp_t const self = smcp_get_current_instance();
	coap_option_key_t ret = self->inbound.last_option_key;
	if(self->inbound.last_option_key!=COAP_HEADER_INVALID
		&& self->inbound.this_option<((uint8_t*)self->inbound.packet+self->inbound.packet_len)
		&& self->inbound.this_option[0]!=0xFF
	) {
		coap_decode_option(
			self->inbound.this_option,
			&ret,
			value,
			len
		);

	} else {
		ret = COAP_HEADER_INVALID;
	}
	return ret;
}

bool
smcp_inbound_option_strequal(coap_option_key_t key,const char* cstr) {
	smcp_t const self = smcp_get_current_instance();
	coap_option_key_t curr_key = self->inbound.last_option_key;
	const char* value;
	size_t value_len;
	size_t i;

	if(!self->inbound.this_option)
		return false;

	coap_decode_option(self->inbound.this_option, &curr_key, (const uint8_t**)&value, &value_len);

	if(curr_key != key)
		return false;

	for(i=0;i<value_len;i++) {
		if(!cstr[i] || (value[i]!=cstr[i]))
			return false;
	}
	return cstr[i]==0;
}

const struct coap_header_s*
smcp_inbound_get_packet() {
	return smcp_get_current_instance()->inbound.packet;
}

size_t smcp_inbound_get_packet_length() {
	return smcp_get_current_instance()->inbound.packet_len;
}

const char*
smcp_inbound_get_content_ptr() {
	return smcp_get_current_instance()->inbound.content_ptr;
}

size_t
smcp_inbound_get_content_len() {
	return smcp_get_current_instance()->inbound.content_len;
}

coap_content_type_t
smcp_inbound_get_content_type() {
	return smcp_get_current_instance()->inbound.content_type;
}


#if SMCP_USE_BSD_SOCKETS
struct sockaddr* smcp_inbound_get_saddr() {
	return smcp_get_current_instance()->inbound.saddr;
}

socklen_t smcp_inbound_get_socklen() {
	return smcp_get_current_instance()->inbound.socklen;
}
#elif defined(CONTIKI)
const uip_ipaddr_t* smcp_inbound_get_ipaddr() {
	return &smcp_get_current_instance()->inbound.toaddr;
}

const uint16_t smcp_inbound_get_ipport() {
	return smcp_get_current_instance()->inbound.toport;
}
#endif

bool
smcp_inbound_is_dupe() {
	return smcp_get_current_instance()->inbound.is_dupe;
}

bool
smcp_inbound_origin_is_local() {
#if SMCP_USE_BSD_SOCKETS
	struct sockaddr_in6* const saddr = (struct sockaddr_in6*)smcp_inbound_get_saddr();
	// Is this adequate?
	return IN6_IS_ADDR_LOOPBACK(&saddr->sin6_addr)
		&& saddr->sin6_port==htonl(smcp_get_port(smcp_get_current_instance()));
#else
	return false;
#endif
}

#pragma mark -

smcp_status_t
smcp_handle_inbound_packet(
	smcp_t	self,
	char*			buffer,
	size_t			packet_length,
	SMCP_SOCKET_ARGS
) {
	SMCP_EMBEDDED_SELF_HOOK;
	smcp_status_t ret = 0;
	struct coap_header_s* const packet = (void*)buffer; // Should not use stack space.

	require_action(coap_verify_packet(buffer,packet_length),bail,ret=SMCP_STATUS_BAD_PACKET);

	smcp_set_current_instance(self);

#if VERBOSE_DEBUG
	{
		char addr_str[50] = "???";
		uint16_t port = 0;
#if SMCP_USE_BSD_SOCKETS
		inet_ntop(AF_INET6,&((struct sockaddr_in6*)saddr)->sin6_addr,addr_str,sizeof(addr_str)-1);
		port = ntohs(((struct sockaddr_in6*)saddr)->sin6_port);
#elif CONTIKI
		port = ntohs(toport);
#define CSTR_FROM_6ADDR(dest,addr) sprintf(dest,"%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x", ((uint8_t *)addr)[0], ((uint8_t *)addr)[1], ((uint8_t *)addr)[2], ((uint8_t *)addr)[3], ((uint8_t *)addr)[4], ((uint8_t *)addr)[5], ((uint8_t *)addr)[6], ((uint8_t *)addr)[7], ((uint8_t *)addr)[8], ((uint8_t *)addr)[9], ((uint8_t *)addr)[10], ((uint8_t *)addr)[11], ((uint8_t *)addr)[12], ((uint8_t *)addr)[13], ((uint8_t *)addr)[14], ((uint8_t *)addr)[15])
		CSTR_FROM_6ADDR(addr_str,toaddr);
#endif
		DEBUG_PRINTF(CSTR("smcp(%p): Inbound packet from [%s]:%d"), self,addr_str,(int)port);
		coap_dump_header(
			SMCP_DEBUG_OUT_FILE,
			"Inbound:\t",
			(struct coap_header_s*)buffer,
			packet_length
		);
	}
#endif

	// Reset all inbound packet state.
	memset(&self->inbound,0,sizeof(self->inbound));

	// We are processing a message.
	self->is_processing_message = true;
	self->did_respond = false;

	self->inbound.packet = packet;
	self->inbound.packet_len = packet_length;

#if SMCP_USE_BSD_SOCKETS
	self->inbound.saddr = saddr;
	self->inbound.socklen = socklen;
#elif CONTIKI
	memcpy(&self->inbound.toaddr,toaddr,sizeof(*toaddr));
	self->inbound.toport = toport;
#endif

	// TODO: Set `self->inbound.was_sent_to_multicast` properly!
	// self->inbound.was_sent_to_multicast = ???

	{
		int i;

		// Update dupe hash.
		fasthash_start(0);
#if SMCP_USE_BSD_SOCKETS
		fasthash_feed((void*)self->inbound.saddr,self->inbound.socklen);
#elif CONTIKI
		fasthash_feed((void*)&self->inbound.toaddr,sizeof(self->inbound.toaddr));
		fasthash_feed((void*)&self->inbound.toport,sizeof(self->inbound.toport));
#endif
		fasthash_feed_byte(self->inbound.packet->code);
		fasthash_feed((const uint8_t*)&self->inbound.packet->msg_id,sizeof(self->inbound.packet->msg_id));

		self->inbound.transaction_hash = fasthash_finish_uint32();
		i = SMCP_CONF_DUPE_BUFFER_SIZE;
		while(i--) {
			if(self->dupe[i].hash == self->inbound.transaction_hash) {
				self->inbound.is_dupe = true;
				break;
			}
		}
	}

	// Version check.
	require_action(packet->version==COAP_VERSION,bail,ret=SMCP_STATUS_FAILURE);

	// Make sure there is a zero at the end of the packet, so that
	// if the content is a string it will be conveniently zero terminated.
	// Kind of a hack, but very convenient.
	buffer[packet_length] = 0;

	if(self->inbound.was_sent_to_multicast) {
		// If this was multicast, make sure it isn't confirmable.
		require_action(
			packet->tt!=COAP_TRANS_TYPE_CONFIRMABLE,
			bail,
			ret=SMCP_STATUS_FAILURE
		);
	}

//	DEBUG_PRINTF(CSTR("%p: tt=%d"), self,packet->tt);
//	DEBUG_PRINTF(CSTR("%p: http.code=%d, coap.code=%d"),self,coap_to_http_code(packet->code),packet->code);

	smcp_inbound_reset_next_option();

	{	// Initial options scan.
		coap_option_key_t key;
		do {
			const uint8_t* value;
			size_t value_len;
			key = smcp_inbound_peek_option(&value,&value_len);
//			if(key==COAP_HEADER_TOKEN) {
//				self->inbound.token_option = self->inbound.this_option;
//				if((self->inbound.token_option[0]&0xF0)==0xF0)
//					self->inbound.token_option+=self->inbound.token_option[0]&0xf;
//			} else
			if(key==COAP_HEADER_CONTENT_TYPE) {
				uint8_t i;
				self->inbound.content_type = 0;
				for(i = 0; i < value_len; i++)
					self->inbound.content_type = (self->inbound.content_type << 8) + value[i];
			} else if(key==COAP_HEADER_BLOCK2) {
				uint8_t i;
				self->inbound.block2_value = 0;
				for(i = 0; i < value_len; i++)
					self->inbound.block2_value = (self->inbound.block2_value << 8) + value[i];
			} else if(key==COAP_HEADER_OBSERVE) {
				uint8_t i;
				self->inbound.has_observe_option = 1;
				self->inbound.observe_value = 0;
				for(i = 0; i < value_len; i++)
					self->inbound.observe_value = (self->inbound.observe_value << 8) + value[i];
			} else if(key==COAP_HEADER_MAX_AGE) {
				uint8_t i;
				self->inbound.max_age = 0;
				for(i = 0; i < value_len; i++)
					self->inbound.max_age = (self->inbound.max_age << 8) + value[i];
				self->inbound.max_age += 1;
				if(self->inbound.max_age<5)
					self->inbound.max_age = 5;
			}
		} while(smcp_inbound_next_option_(NULL,NULL)!=COAP_HEADER_INVALID);

		require(((unsigned)(self->inbound.this_option-(uint8_t*)buffer)==packet_length) || self->inbound.this_option[0]==0xFF,bail);

		// Now that we are at the end of the options, we know
		// where the content starts.
		self->inbound.content_ptr = (char*)self->inbound.this_option;
		self->inbound.content_len = self->inbound.packet_len-(self->inbound.content_ptr-buffer);
		if(self->inbound.content_len) {
			self->inbound.content_ptr++;
			self->inbound.content_len--;
		}
	}

	smcp_inbound_reset_next_option();

#if SMCP_USE_CASCADE_COUNT
	self->cascade_count--;
#endif

	if(COAP_CODE_IS_REQUEST(packet->code)) {
		ret = smcp_handle_request(self);
	} else if(COAP_CODE_IS_RESULT(packet->code)) {
		ret = smcp_handle_response(self);
	}

	check_string(ret == SMCP_STATUS_OK, smcp_status_to_cstr(ret));

	if(	(ret == SMCP_STATUS_OK)
		&& !self->inbound.is_fake
		&& !self->inbound.is_dupe
		&& (self->inbound.packet->code != COAP_METHOD_GET
			|| (self->did_respond
				&& self->outbound.packet->code == COAP_CODE_EMPTY
				&& self->outbound.packet->tt == COAP_TRANS_TYPE_ACK
			)
		)
	) {
		// This is not a dupe, add it to the list.
		self->dupe[self->dupe_index].hash = self->inbound.transaction_hash;
		self->dupe_index++;
		self->dupe_index%=SMCP_CONF_DUPE_BUFFER_SIZE;
	}

	// Check to make sure we have responded by now. If not, we need to.
	if(!self->did_respond && (packet->tt==COAP_TRANS_TYPE_CONFIRMABLE)) {
		if(COAP_CODE_IS_REQUEST(packet->code)) {
			int result_code = smcp_convert_status_to_result_code(ret);
			if(self->inbound.is_dupe)
				ret = 0;
			if(ret == SMCP_STATUS_OK) {
				if(packet->code==COAP_METHOD_GET)
					result_code = COAP_RESULT_205_CONTENT;
				else if(packet->code==COAP_METHOD_POST || packet->code==COAP_METHOD_PUT)
					result_code = COAP_RESULT_204_CHANGED;
				else if(packet->code==COAP_METHOD_DELETE)
					result_code = COAP_RESULT_202_DELETED;
			}
			smcp_outbound_begin_response(result_code);
		} else {
			smcp_outbound_begin_response(0);
			if(ret && !self->inbound.is_dupe)
				self->outbound.packet->tt = COAP_TRANS_TYPE_RESET;
			else
				self->outbound.packet->tt = COAP_TRANS_TYPE_ACK;
			smcp_outbound_set_token(NULL, 0);
		}
		ret = smcp_outbound_send();
	}

bail:
	smcp_set_current_instance(NULL);
	self->is_processing_message = false;
	self->force_current_outbound_code = false;
	self->inbound.content_ptr = NULL;
	self->inbound.content_len = 0;
	return ret;
}

smcp_status_t
smcp_process(
	smcp_t self, cms_t cms
) {
	SMCP_EMBEDDED_SELF_HOOK;
	smcp_status_t ret = 0;

#if SMCP_USE_BSD_SOCKETS
	int tmp;
	struct pollfd pollee = { self->fd, POLLIN | POLLHUP, 0 };

	if(cms >= 0)
		cms = MIN(cms, smcp_get_timeout(self));
	else
		cms = smcp_get_timeout(self);

	errno = 0;

	tmp = poll(&pollee, 1, cms);

	// Ensure that poll did not fail with an error.
	require_action_string(errno == 0,
		bail,
		ret = SMCP_STATUS_ERRNO,
		strerror(errno)
	);

	if(tmp > 0) {
		char packet[SMCP_MAX_PACKET_LENGTH];
		size_t packet_length = sizeof(packet);
		struct sockaddr_in6 packet_saddr;
		socklen_t packet_saddr_len = sizeof(packet_saddr);

		packet_length = recvfrom(
			self->fd,
			(void*)packet,
			packet_length,
			0,
			(struct sockaddr*)&packet_saddr,
			&packet_saddr_len
		);

		require_action(packet_length > 0, bail, ret = SMCP_STATUS_ERRNO);

		packet[packet_length] = 0;

		ret = smcp_handle_inbound_packet(
			self,
			packet,
			packet_length,
			(struct sockaddr*)&packet_saddr,
			packet_saddr_len
		);
	}
#endif

	smcp_set_current_instance(self);
	smcp_handle_timers(self);

bail:
	smcp_set_current_instance(NULL);
	self->is_responding = false;
	return ret;
}


#pragma mark -
#pragma mark Transaction Support

static bt_compare_result_t
smcp_transaction_compare(
	const void* lhs_, const void* rhs_, void* context
) {
	const smcp_transaction_t lhs = (smcp_transaction_t)lhs_;
	const smcp_transaction_t rhs = (smcp_transaction_t)rhs_;

	if(lhs->msg_id > rhs->msg_id)
		return 1;
	if(lhs->msg_id < rhs->msg_id)
		return -1;
	return 0;
}

static bt_compare_result_t
smcp_transaction_compare_msg_id(
	const void* lhs_, const void* rhs_, void* context
) {
	const smcp_transaction_t lhs = (smcp_transaction_t)lhs_;
	coap_msg_id_t rhs = (coap_msg_id_t)(uintptr_t)rhs_;

	if(lhs->msg_id > rhs)
		return 1;
	if(lhs->msg_id < rhs)
		return -1;
	return 0;
}

smcp_transaction_t
smcp_transaction_find_via_msg_id(smcp_t self, coap_msg_id_t msg_id) {
	SMCP_EMBEDDED_SELF_HOOK;
	return (smcp_transaction_t)bt_find(
		(void**)&self->transactions,
		(void*)(uintptr_t)msg_id,
		(bt_compare_func_t)smcp_transaction_compare_msg_id,
		self
	);
}

smcp_transaction_t
smcp_transaction_find_via_token(smcp_t self, coap_msg_id_t token) {
	SMCP_EMBEDDED_SELF_HOOK;
	smcp_transaction_t ret = bt_first(self->transactions);

	// Ouch. Linear search.
	while(ret && ret->token!=token) ret = bt_next(ret);

	return ret;
}

void
smcp_internal_delete_transaction_(
	smcp_transaction_t handler,
	smcp_t			self
) {
	DEBUG_PRINTF("smcp_internal_delete_transaction_: %p",handler);

	if(!smcp_get_current_instance())
		smcp_set_current_instance(self);

	check(self==smcp_get_current_instance());

	// Remove the timer associated with this handler.
	smcp_invalidate_timer(self, &handler->timer);

	handler->active = 0;

	// Fire the callback to signal that this handler is now invalidated.
	if(handler->callback) {
		(*handler->callback)(
			SMCP_STATUS_TRANSACTION_INVALIDATED,
			handler->context
		);
	}

#if SMCP_NO_MALLOC
	if(handler->should_dealloc)
		handler->callback = NULL;
#else
	if(handler->should_dealloc)
		free(handler);
#endif
}

static int smcp_rtt = COAP_RESPONSE_TIMEOUT;

static int
calc_retransmit_timeout(int retries_) {
	int ret = smcp_rtt;

	ret = (ret<<retries_);

	ret *= (1000 - 150) + (SMCP_FUNC_RANDOM_UINT32() % 300);

	ret /= 1000;

	if(ret > 5000)
		ret = 5000;

	return ret;
}

static void
smcp_transaction_new_msg_id(
	smcp_t			self,
	smcp_transaction_t handler,
	coap_msg_id_t msg_id
) {
	SMCP_EMBEDDED_SELF_HOOK;
	require(handler->active,bail);

	bt_remove(
		(void**)&self->transactions,
		handler,
		(bt_compare_func_t)smcp_transaction_compare,
		(bt_delete_func_t)NULL,
		self
	);

	assert(!smcp_transaction_find_via_msg_id(self, msg_id));

	handler->msg_id = msg_id;

	bt_insert(
		(void**)&self->transactions,
		handler,
		(bt_compare_func_t)smcp_transaction_compare,
		(bt_delete_func_t)smcp_internal_delete_transaction_,
		self
	);

bail:
	return;
}

void
smcp_internal_transaction_timeout_(
	smcp_t			self,
	smcp_transaction_t handler
) {
	smcp_status_t status = SMCP_STATUS_TIMEOUT;
	void* context = handler->context;
	cms_t cms = convert_timeval_to_cms(&handler->expiration);

	self->current_transaction = handler;

	if(cms > 0) {
		if(	(handler->flags&SMCP_TRANSACTION_KEEPALIVE)
			&& cms>SMCP_OBSERVATION_KEEPALIVE_INTERVAL
		) {
			cms = SMCP_OBSERVATION_KEEPALIVE_INTERVAL;
		}

		if(handler->waiting_for_async_response && !(handler->flags&SMCP_TRANSACTION_KEEPALIVE)) {
			status = SMCP_STATUS_OK;
		}

		if(status == SMCP_STATUS_TIMEOUT && handler->resendCallback) {
			// Resend.
			self->outbound.next_tid = handler->msg_id;
			self->is_processing_message = false;
			self->is_responding = false;
			self->did_respond = false;

			status = handler->resendCallback(context);

			if(status == SMCP_STATUS_OK) {
				if(self->outbound.packet->tt!=COAP_TRANS_TYPE_NONCONFIRMABLE
					|| handler->attemptCount<2
				) {
					cms = MIN(cms,calc_retransmit_timeout(handler->attemptCount++));
				}
			} else if(status == SMCP_STATUS_WAIT_FOR_DNS) {
				cms = 100;
				status = SMCP_STATUS_OK;
			}
		}

		smcp_schedule_timer(
			self,
			&handler->timer,
			cms
		);
	} else if((handler->flags&SMCP_TRANSACTION_OBSERVE)) {
		// We have expired and we are observable. In this case we
		// need to restart the observing process.

		DEBUG_PRINTF("Observe-Transaction-Timeout: Starting over for %p",handler);

		handler->waiting_for_async_response = false;
		handler->attemptCount = 0;
		handler->last_observe = 0;
		handler->next_block2 = 0;
		smcp_transaction_new_msg_id(self,handler,smcp_get_next_msg_id(self, NULL));
		convert_cms_to_timeval(&handler->expiration, SMCP_OBSERVATION_DEFAULT_MAX_AGE);

		if(handler->resendCallback) {
			// In this case we will be reattempting for a given duration.
			// The first attempt should happen pretty much immediately.
			cms = 0;

			if(handler->flags&SMCP_TRANSACTION_DELAY_START) {
				// Unless this flag is set. Then we need to wait a moment.
				cms = 10 + (SMCP_FUNC_RANDOM_UINT32() % 290);
			}
		}

		if(	(handler->flags&SMCP_TRANSACTION_KEEPALIVE)
			&& cms>SMCP_OBSERVATION_KEEPALIVE_INTERVAL
		) {
			cms = SMCP_OBSERVATION_KEEPALIVE_INTERVAL;
		}

		status = smcp_schedule_timer(
			self,
			&handler->timer,
			cms
		);
	}

	if(status) {
		smcp_response_handler_func callback = handler->callback;

		if(handler->flags&SMCP_TRANSACTION_OBSERVE) {
			// If we are an observing transaction, we need to clean up
			// first by sending one last request without an observe option.
			// TODO: Implement this!
		}

		if(!(handler->flags&SMCP_TRANSACTION_ALWAYS_INVALIDATE))
			handler->callback = NULL;
		if(callback)
			(*callback)(status,context);
		if(handler != self->current_transaction)
			return;
		smcp_transaction_end(self, handler);
	}

	self->current_transaction = NULL;
}


smcp_transaction_t
smcp_transaction_init(
	smcp_transaction_t handler,
	int	flags,
	smcp_inbound_resend_func resendCallback,
	smcp_response_handler_func	callback,
	void* context
) {
	if(!handler) {
#if SMCP_NO_MALLOC
		uint8_t i;
		for(i=0;i<SMCP_CONF_MAX_TRANSACTIONS;i++) {
			handler = &smcp_transaction_pool[i];
			if(handler->callback) {
				handler = NULL;
				continue;
			}
		}
#else
		handler = (smcp_transaction_t)calloc(sizeof(*handler), 1);
#endif
		handler->should_dealloc = 1;
	} else {
		memset(handler, sizeof(*handler),0);
	}

	require(handler!=NULL, bail);

	handler->resendCallback = resendCallback;
	handler->callback = callback;
	handler->context = context;
	handler->flags = flags;

bail:
	return handler;
}

smcp_status_t
smcp_transaction_tickle(
	smcp_t self,
	smcp_transaction_t handler
) {
	SMCP_EMBEDDED_SELF_HOOK;

	smcp_invalidate_timer(self, &handler->timer);

	smcp_schedule_timer(
		self,
		&handler->timer,
		0
	);

	return 0;
}

smcp_status_t
smcp_transaction_begin(
	smcp_t self,
	smcp_transaction_t handler,
	cms_t expiration
) {
	SMCP_EMBEDDED_SELF_HOOK;
	require(handler!=NULL, bail);

	DEBUG_PRINTF("smcp_transaction_begin: %p",handler);

	bt_remove(
		(void**)&self->transactions,
		(void*)handler,
		(bt_compare_func_t)smcp_transaction_compare,
		NULL,
		self
	);

	handler->token = smcp_get_next_msg_id(self, NULL);
	handler->msg_id = handler->token;
	handler->waiting_for_async_response = false;
	handler->attemptCount = 0;
	handler->last_observe = 0;
	handler->next_block2 = 0;
	handler->active = 1;
	convert_cms_to_timeval(&handler->expiration, expiration);

	if(handler->resendCallback) {
		// In this case we will be reattempting for a given duration.
		// The first attempt should happen pretty much immediately.
		expiration = 0;

		if(handler->flags&SMCP_TRANSACTION_DELAY_START) {
			// Unless this flag is set. Then we need to wait a moment.
			expiration = 10 + (SMCP_FUNC_RANDOM_UINT32() % 290);
		}
	}

	if(	(handler->flags&SMCP_TRANSACTION_KEEPALIVE)
		&& expiration>SMCP_OBSERVATION_KEEPALIVE_INTERVAL
	) {
		expiration = SMCP_OBSERVATION_KEEPALIVE_INTERVAL;
	}

	smcp_schedule_timer(
		self,
		smcp_timer_init(
			&handler->timer,
			(smcp_timer_callback_t)&smcp_internal_transaction_timeout_,
			NULL,
			handler
		),
		expiration
	);

	bt_insert(
		(void**)&self->transactions,
		handler,
		(bt_compare_func_t)smcp_transaction_compare,
		(bt_delete_func_t)smcp_internal_delete_transaction_,
		self
	);

	DEBUG_PRINTF(CSTR("%p: Total Pending Transactions: %d"), self,
		(int)bt_count((void**)&self->transactions));

bail:
	return 0;
}

smcp_status_t
smcp_transaction_end(
	smcp_t self,
	smcp_transaction_t transaction
) {
	SMCP_EMBEDDED_SELF_HOOK;
	DEBUG_PRINTF("smcp_transaction_end: %p",transaction);

	if(transaction->flags&SMCP_TRANSACTION_OBSERVE) {
		// If we are an observing transaction, we need to clean up
		// first by sending one last request without an observe option.
		// TODO: Implement this!
	}

	if(transaction == self->current_transaction)
		self->current_transaction = NULL;

	if(transaction->active) {
		transaction->active = 0; // Maybe we should remove this line? May be hiding bad behavior.
		bt_remove(
			(void**)&self->transactions,
			(void*)transaction,
			(bt_compare_func_t)smcp_transaction_compare,
			(bt_delete_func_t)smcp_internal_delete_transaction_,
			self
		);
	}
	return 0;
}





//! DEPRECATED.
smcp_status_t
smcp_begin_transaction_old(
	smcp_t						self,
	coap_msg_id_t		tid,
	cms_t						cmsExpiration,
	int							flags,
	smcp_inbound_resend_func	resendCallback,
	smcp_response_handler_func	callback,
	void*						context
) {
	smcp_status_t ret = 0;
	smcp_transaction_t transaction = NULL;

	transaction = smcp_transaction_init(
		transaction,
		flags,
		resendCallback,
		callback,
		context
	);

	ret = smcp_transaction_begin(self,transaction,cmsExpiration);

	require_noerr(ret,bail);
	require(transaction!=NULL,bail);

	transaction->token = tid;

bail:
	return ret;
}

//! DEPRECATED.
smcp_status_t
smcp_invalidate_transaction_old(
	smcp_t			self,
	coap_msg_id_t	tid
) {
	smcp_transaction_t transaction = smcp_transaction_find_via_msg_id(self, tid);
	if(!transaction) {
		transaction = smcp_transaction_find_via_token(self, tid);
	}
	if(transaction)
		smcp_transaction_end(self, transaction);
	return 0;
}

#pragma mark -
#pragma mark Request/Response Handlers

smcp_status_t
smcp_default_request_handler(
   smcp_node_t node,
   smcp_method_t method
) {
   if(method == COAP_METHOD_GET) {
	   return smcp_handle_list(node,method);
   }
   return SMCP_STATUS_NOT_ALLOWED;
}

smcp_status_t
smcp_handle_request(
	smcp_t	self
) {
	smcp_status_t ret = 0;

#if VERBOSE_DEBUG
	{   // Print out debugging information.
		DEBUG_PRINTF(
			"smcp(%p): %sIncoming request!",
			self,
			(self->inbound.is_fake)?"(FAKE) ":""
		);
//		coap_dump_header(
//			SMCP_DEBUG_OUT_FILE,
//			"Inbound:\t",
//			self->inbound.packet,
//			0
//		);
	}
#endif

	smcp_node_t node = smcp_get_root_node(self);
	smcp_inbound_handler_func request_handler = &smcp_default_request_handler;

	// Authenticate this request.
	ret = smcp_auth_verify_request();
	require_noerr(ret,bail);

	smcp_inbound_reset_next_option();

	{
		const uint8_t* prev_option_ptr = self->inbound.this_option;
		coap_option_key_t prev_key = 0;
		coap_option_key_t key;
		const uint8_t* value;
		size_t value_len;
		while((key=smcp_inbound_next_option_(&value, &value_len))!=COAP_HEADER_INVALID) {
			if(key>COAP_HEADER_URI_PATH) {
				self->inbound.this_option = prev_option_ptr;
				self->inbound.last_option_key = prev_key;
//				self->inbound.options_left++;
				break;
			} else if(key==COAP_HEADER_PROXY_URI) {
				// Skip the proxy URI for now.
			} else if(key==COAP_HEADER_URI_PATH) {
				smcp_node_t next = smcp_node_find(
					node,
					(const char*)value,
					value_len
				);
				if(next) {
					node = next;
				} else {
					self->inbound.this_option = prev_option_ptr;
					self->inbound.last_option_key = prev_key;
//					self->inbound.options_left++;
					break;
				}
			} else if(key==COAP_HEADER_URI_HOST) {
				// Skip host at the moment,
				// because we don't do virtual hosting yet.
			} else if(key==COAP_HEADER_URI_PORT) {
				// Skip port at the moment,
				// because we don't do virtual hosting yet.
			} else if(key==COAP_HEADER_CONTENT_TYPE) {
				// Skip the content type for now,
				// but we will need to keep track of this later.
			} else {
				if(COAP_HEADER_IS_REQUIRED(key)) {
					ret=SMCP_STATUS_BAD_OPTION;
					assert_printf("Unrecognized option %d, \"%s\"",
						key,
						coap_option_key_to_cstr(key, false)
					);
					goto bail;
				}
			}
			prev_option_ptr = self->inbound.this_option;
			prev_key = self->inbound.last_option_key;
		}
	}

	if(node->request_handler)
		request_handler = node->request_handler;

	// By returning directly here we can
	// possibly avoid having the overhead of
	// the current function on the stack.
	return (*request_handler)(
	    node,
	    self->inbound.packet->code
	);

bail:
	return ret;
}

smcp_status_t
smcp_handle_response(
	smcp_t	self
) {
	smcp_status_t ret = 0;
	smcp_transaction_t handler = NULL;
	coap_msg_id_t msg_id;

#if VERBOSE_DEBUG
	{   // Print out debugging information.
		DEBUG_PRINTF(
			"smcp(%p): Incoming response! tid=%d",
			self,
			smcp_inbound_get_msg_id()
		);
//		coap_dump_header(
//			SMCP_DEBUG_OUT_FILE,
//			"Inbound:\t",
//			self->inbound.packet,
//			self->inbound.packet_len
//		);
	}
#endif

	DEBUG_PRINTF(CSTR("%p: Total Pending Transactions: %d"), self,
		(int)bt_count((void**)&self->transactions));

	msg_id = smcp_inbound_get_msg_id();

	handler = smcp_transaction_find_via_msg_id(self,msg_id);

	if(!handler && self->inbound.packet->token_len==sizeof(coap_msg_id_t)) {
		coap_msg_id_t token;
		memcpy(&token,self->inbound.packet->token,sizeof(token));
		handler = smcp_transaction_find_via_token(self,token);

		if(handler && !handler->waiting_for_async_response)
			handler = NULL;
	}

	self->current_transaction = handler;

	// TODO: Make sure this packet didn't originate from multicast.
	// ...Or do what?

	if(!handler) {
		if(self->inbound.packet->tt <= COAP_TRANS_TYPE_NONCONFIRMABLE) {
			DEBUG_PRINTF("Inbound: Unknown Response, sending reset. . .");
			// We don't know what they are talking
			// about, so send them a reset so that they
			// will shut up.

			ret = smcp_outbound_begin_response(0);
			require_noerr(ret,bail);

			self->outbound.packet->tt = COAP_TRANS_TYPE_RESET;

			ret = smcp_outbound_send();
			require_noerr(ret,bail);
		}
	} else if(	(	self->inbound.packet->tt == COAP_TRANS_TYPE_ACK
			|| self->inbound.packet->tt == COAP_TRANS_TYPE_NONCONFIRMABLE
		)
		&& !self->inbound.packet->code
		&& (handler->sent_code<COAP_RESULT_100)
	) {
		DEBUG_PRINTF("Inbound: Async Response");
		handler->waiting_for_async_response = true;
	} else if(handler->callback) {
		coap_msg_id_t msg_id = handler->msg_id;

		// Handle any authentication heaers.
		ret = smcp_auth_handle_response(handler);
		require_noerr(ret,bail);

		smcp_inbound_reset_next_option();

		if((handler->flags & SMCP_TRANSACTION_OBSERVE) && self->inbound.has_observe_option) {
			cms_t cms = self->inbound.max_age*1000;

			if(	self->inbound.has_observe_option
				&& (self->inbound.observe_value<=handler->last_observe)
				&& ((handler->last_observe-self->inbound.observe_value)>0x7FFFFF)
			) {
				DEBUG_PRINTF("Inbound: Skipping older inbound observation. (%d<=%d)",self->inbound.observe_value,handler->last_observe);
				// We've already seen this one. Skip it.
				ret = SMCP_STATUS_DUPE;
				goto bail;
			}

			handler->last_observe = self->inbound.observe_value;

			ret = (*handler->callback)(
				self->inbound.packet->tt==COAP_TRANS_TYPE_RESET?SMCP_STATUS_RESET:self->inbound.packet->code,
				handler->context
			);

			if(!self->current_transaction) {
				handler = NULL;
				goto bail;
			}

			if(msg_id!=handler->msg_id) {
				handler = NULL;
				goto bail;
			}

			if(self->inbound.has_observe_option) {
				handler->waiting_for_async_response = true;
			}

			handler->attemptCount = 0;
			handler->timer.cancel = NULL;

			smcp_invalidate_timer(self, &handler->timer);

			if(!cms) {
				if(self->inbound.has_observe_option)
					cms = CMS_DISTANT_FUTURE;
				else
					cms = SMCP_OBSERVATION_DEFAULT_MAX_AGE;
			}

			convert_cms_to_timeval(&handler->expiration, cms);

			if(	(handler->flags&SMCP_TRANSACTION_KEEPALIVE)
				&& cms>SMCP_OBSERVATION_KEEPALIVE_INTERVAL
			) {
				cms = SMCP_OBSERVATION_KEEPALIVE_INTERVAL;
			}

			smcp_schedule_timer(
				self,
				&handler->timer,
				cms
			);
		} else {
			smcp_response_handler_func callback = handler->callback;
			if(!(handler->flags&SMCP_TRANSACTION_ALWAYS_INVALIDATE) && !(handler->flags&SMCP_TRANSACTION_OBSERVE)) {
				handler->callback = NULL;
			}
			ret = (*callback)(
				(self->inbound.packet->tt==COAP_TRANS_TYPE_RESET)?SMCP_STATUS_RESET:self->inbound.packet->code,
				handler->context
			);

			if(self->current_transaction != handler) {
				handler = NULL;
				goto bail;
			}

			handler->attemptCount = 0;
			handler->waiting_for_async_response = false;
			if(handler->active && msg_id==handler->msg_id) {
				if(!ret && (self->inbound.block2_value&(1<<3)) && (handler->flags&SMCP_TRANSACTION_ALWAYS_INVALIDATE)) {
					DEBUG_PRINTF("Inbound: Preparing to request next block...");
					handler->next_block2 = self->inbound.block2_value + (1<<4);
					smcp_transaction_new_msg_id(self, handler, smcp_get_next_msg_id(self, NULL));
					smcp_invalidate_timer(self, &handler->timer);
					smcp_schedule_timer(
						self,
						&handler->timer,
						0
					);
				} else if (!(handler->flags&SMCP_TRANSACTION_OBSERVE)) {
					handler->resendCallback = NULL;
					smcp_transaction_end(self, handler);
					handler = NULL;
				} else {
					cms_t cms = self->inbound.max_age*1000;
					handler->next_block2 = 0;

					smcp_invalidate_timer(self, &handler->timer);

					if(!cms) {
						if(self->inbound.has_observe_option)
							cms = CMS_DISTANT_FUTURE;
						else
							cms = SMCP_OBSERVATION_DEFAULT_MAX_AGE;
					}

					convert_cms_to_timeval(&handler->expiration, cms);

					if(	(handler->flags&SMCP_TRANSACTION_KEEPALIVE)
						&& cms>SMCP_OBSERVATION_KEEPALIVE_INTERVAL
					) {
						cms = SMCP_OBSERVATION_KEEPALIVE_INTERVAL;
					}

					smcp_schedule_timer(
						self,
						&handler->timer,
						cms
					);
				}
			}
		}
	}

bail:
	if(ret && handler) {
		smcp_transaction_end(self, handler);
	}
	return ret;
}

#pragma mark -
#pragma mark Asynchronous Response Support

smcp_status_t
smcp_outbound_set_async_response(struct smcp_async_response_s* x) {
	smcp_status_t ret = 0;
	smcp_t const self = smcp_get_current_instance();
	self->inbound.packet = &x->request.header;
	self->inbound.packet_len = x->request_len;
	self->inbound.content_ptr = (char*)x->request.header.token + x->request.header.token_len;
	self->inbound.last_option_key = 0;
	self->inbound.this_option = x->request.header.token;
	self->outbound.packet->tt = x->request.header.tt;
	self->inbound.is_fake = true;
	self->is_processing_message = true;
	self->did_respond = false;

	ret = smcp_outbound_set_token(x->request.header.token, x->request.header.token_len);
	require_noerr(ret, bail);

#if SMCP_USE_BSD_SOCKETS
	ret = smcp_outbound_set_destaddr((void*)&x->saddr,x->socklen);
#elif CONTIKI
	ret = smcp_outbound_set_destaddr(&x->toaddr,x->toport);
#endif

	require_noerr(ret, bail);

	assert(coap_verify_packet((const char*)x->request.bytes, x->request_len));
bail:
	return ret;
}

smcp_status_t
smcp_start_async_response(struct smcp_async_response_s* x,int flags) {
	smcp_status_t ret = 0;
	smcp_t const self = smcp_get_current_instance();

	require_action_string(x!=NULL,bail,ret=SMCP_STATUS_INVALID_ARGUMENT,"NULL async_response arg");

	// TODO: Be more graceful...?
	require_action_string(
		smcp_inbound_get_packet_length()-smcp_inbound_get_content_len()<=sizeof(x->request),
		bail,
		ret=SMCP_STATUS_FAILURE,
		"Request too big for async response"
	);

	x->request_len = smcp_inbound_get_packet_length()-smcp_inbound_get_content_len();
	memcpy(x->request.bytes,smcp_inbound_get_packet(),x->request_len);

	assert(coap_verify_packet((const char*)x->request.bytes, x->request_len));

#if SMCP_USE_BSD_SOCKETS
	x->socklen = self->inbound.socklen;
	memcpy(&x->saddr,self->inbound.saddr,sizeof(x->saddr));
#elif CONTIKI
	memcpy(&x->toaddr,&self->inbound.toaddr,sizeof(x->toaddr));
	x->toport = self->inbound.toport;
#endif

	if(!(flags & SMCP_ASYNC_RESPONSE_FLAG_DONT_ACK)) {
		// Fake inbound packets are created to tickle
		// content out of nodes by the pairing system.
		// Since we are asynchronous, this clearly isn't
		// going to work. Support for this will have to
		// come in the future.
		require_action(!self->inbound.is_fake,bail,ret = SMCP_STATUS_NOT_IMPLEMENTED);

		ret = smcp_outbound_begin_response(COAP_CODE_EMPTY);

		require_noerr(ret, bail);

		smcp_outbound_set_token(NULL, 0);

		ret = smcp_outbound_send();

		require_noerr(ret, bail);
	}

	if(self->inbound.is_dupe) {
		ret = SMCP_STATUS_DUPE;
		goto bail;
	}

bail:
	return ret;
}

smcp_status_t
smcp_finish_async_response(struct smcp_async_response_s* x) {
	// This method is largely a hook for future functionality.
	// It doesn't really do much at the moment, but it may later.

	return SMCP_STATUS_OK;
}

#pragma mark -
#pragma mark Other

coap_msg_id_t
smcp_get_next_msg_id(smcp_t self, void* context) {
#if 0
	static uint16_t table[16];
	uint8_t hash;

	fasthash_start(0x31337);
	fasthash_feed((uint8_t*)&context, sizeof(void*));
	hash = fasthash_finish_uint8();

	if(!table[hash])
		table[hash] = (uint16_t)SMCP_FUNC_RANDOM_UINT32();

	table[hash] = table[hash]*23873+41;

	return table[hash];
#else
	static coap_msg_id_t next_msg_id;

	if(!next_msg_id)
		next_msg_id = SMCP_FUNC_RANDOM_UINT32();

	return next_msg_id++;
#endif
}

const char* smcp_status_to_cstr(int x) {
	switch(x) {
	case SMCP_STATUS_OK: return "OK"; break;
	case SMCP_STATUS_HOST_LOOKUP_FAILURE: return "Hostname Lookup Failure";
		break;
	case SMCP_STATUS_FAILURE: return "Unspecified Failure"; break;
	case SMCP_STATUS_INVALID_ARGUMENT: return "Invalid Argument"; break;
	case SMCP_STATUS_BAD_NODE_TYPE: return "Bad Node Type"; break;
	case SMCP_STATUS_UNSUPPORTED_URI: return "Unsupported URI"; break;
	case SMCP_STATUS_MALLOC_FAILURE: return "Malloc Failure"; break;
	case SMCP_STATUS_TRANSACTION_INVALIDATED: return "Handler Invalidated";
		break;
	case SMCP_STATUS_TIMEOUT: return "Timeout"; break;
	case SMCP_STATUS_NOT_IMPLEMENTED: return "Not Implemented"; break;
	case SMCP_STATUS_NOT_FOUND: return "Not Found"; break;
	case SMCP_STATUS_H_ERRNO: return "HERRNO"; break;
	case SMCP_STATUS_RESPONSE_NOT_ALLOWED: return "Response not allowed";
		break;

	case SMCP_STATUS_LOOP_DETECTED: return "Loop detected"; break;
	case SMCP_STATUS_BAD_ARGUMENT: return "Bad Argument"; break;
	case SMCP_STATUS_MESSAGE_TOO_BIG: return "Message Too Big"; break;

	case SMCP_STATUS_NOT_ALLOWED: return "Not Allowed"; break;

	case SMCP_STATUS_BAD_OPTION: return "Bad Option"; break;
	case SMCP_STATUS_DUPE: return "Duplicate"; break;

	case SMCP_STATUS_RESET: return "Transaction Reset"; break;
	case SMCP_STATUS_URI_PARSE_FAILURE: return "URI Parse Failure"; break;

	case SMCP_STATUS_ERRNO:
#if SMCP_USE_BSD_SOCKETS
		return strerror(errno); break;
#else
		return "ERRNO!?"; break;
#endif
	default:
		{
			static char cstr[30];
			sprintf(cstr, "Unknown Status (%d)", x);
			return cstr;
		}
		break;
	}
	return NULL;
}

int smcp_convert_status_to_result_code(smcp_status_t status) {
	int ret = COAP_RESULT_500_INTERNAL_SERVER_ERROR;

	switch(status) {
	case SMCP_STATUS_OK:
		ret = COAP_RESULT_200;
		break;
	case SMCP_STATUS_NOT_FOUND:
		ret = COAP_RESULT_404_NOT_FOUND;
		break;
	case SMCP_STATUS_NOT_IMPLEMENTED:
		ret = COAP_RESULT_501_NOT_IMPLEMENTED;
		break;
	case SMCP_STATUS_NOT_ALLOWED:
		ret = COAP_RESULT_405_METHOD_NOT_ALLOWED;
		break;
	case SMCP_STATUS_BAD_NODE_TYPE:
	case SMCP_STATUS_UNSUPPORTED_URI:
		ret = COAP_RESULT_400_BAD_REQUEST;
		break;
	case SMCP_STATUS_BAD_OPTION:
		ret = COAP_RESULT_402_BAD_OPTION;
		break;
	}

	return ret;
}

///////////////////////////////////////////////////////////////////////////////
#pragma mark -
#pragma mark Fasthash

static struct {
	uint32_t hash;
	uint32_t bytes;
	uint32_t next;
} global_fasthash_state;

static void
fasthash_feed_block(uint32_t blk) {
	blk ^= (global_fasthash_state.bytes>>2);
	global_fasthash_state.hash ^= blk;
	global_fasthash_state.hash = global_fasthash_state.hash*1664525 + 1013904223;
}

void
fasthash_start(uint32_t salt) {
	memset((void*)&global_fasthash_state,sizeof(global_fasthash_state),0);
	fasthash_feed_block(salt);
}

void
fasthash_feed_byte(uint8_t data) {
	global_fasthash_state.next |= (data<<(8*(global_fasthash_state.bytes++&3)));
	if((global_fasthash_state.bytes&3)==0) {
		fasthash_feed_block(global_fasthash_state.next);
		global_fasthash_state.next = 0;
	}
}

void
fasthash_feed(const uint8_t* data, uint8_t len) {
	while(len--)
		fasthash_feed_byte(*data++);
}

uint32_t
fasthash_finish_uint32() {
	if(global_fasthash_state.bytes&3) {
		fasthash_feed_block(global_fasthash_state.next);
		global_fasthash_state.bytes = 0;
	}
	return global_fasthash_state.hash;
}

uint16_t
fasthash_finish_uint16() {
	return global_fasthash_state.hash>>16;
}

uint8_t
fasthash_finish_uint8() {
	return global_fasthash_state.hash>>24;
}
