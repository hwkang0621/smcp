#include "assert_macros.h"
#include "smcp_timer.h"
#include "smcp_timer_node.h"

#include <stdio.h>
#include <stdlib.h>
#include "smcp.h"
#include "url-helpers.h"
#include "smcp_node.h"
#include <string.h>
#include <ctype.h>

#include "smcp_helpers.h"
#include "smcp_pairing.h"

#if __AVR__
PROGMEM
#endif
const char list_content[] =
    "<running>;rel=\"var\";ct=205,"
    "<running?v=1>;rel=\"action\";n=\"Start\","
    "<running?v=0>;rel=\"action\";n=\"Stop\","
    "<running?v=!v>;rel=\"action\";n=\"Toggle\","
    "<?reset>;rel=\"action\","
    "<?restart>;rel=\"action\","
    "<!fire>;rel=\"event\","
    "<period>;rel=\"var\";ct=205,"
    "<remaining>;rel=\"var\";ct=205,"
    "<autorestart>;rel=\"var\";ct=205"
;


void
smcp_timer_node_dealloc(smcp_timer_node_t x) {
	free(x);
}

smcp_timer_node_t
smcp_timer_node_alloc() {
	smcp_timer_node_t ret =
	    (smcp_timer_node_t)calloc(sizeof(struct smcp_timer_node_s), 1);

	ret->node.finalize = (void (*)(smcp_node_t)) & smcp_timer_node_dealloc;
	return ret;
}

smcp_status_t smcp_static_content_fetcher(
	void* context,
	char* content,
	size_t* content_length,
	coap_content_type_t* content_type
) {

	if(content_length && content) {
		*content_length = snprintf(content, *content_length, "v=%s", context);
	}
	if(content_type)
		*content_type = SMCP_CONTENT_TYPE_APPLICATION_FORM_URLENCODED;

	return SMCP_STATUS_OK;
}

#if SMCP_CONF_TIMER_NODE_INCLUDE_COUNT
smcp_status_t smcp_timer_node_content_fetcher(
	smcp_timer_node_t self,
	char* content,
	size_t* content_length,
	coap_content_type_t* content_type
) {

	if(content_length && content) {
		*content_length = snprintf(content, *content_length, "count=%d", ++self->count);
	}
	if(content_type)
		*content_type = SMCP_CONTENT_TYPE_APPLICATION_FORM_URLENCODED;

	return SMCP_STATUS_OK;
}
#endif

void smcp_timer_node_fired(
	smcp_daemon_t smcp, void* context
) {
	smcp_timer_node_t const self = context;

#if SMCP_CONF_TIMER_NODE_INCLUDE_COUNT
	smcp_daemon_trigger_custom_event_with_node(smcp,
		&self->node,
		"!fire",
		(void*)&smcp_timer_node_content_fetcher,
		self
	);
#else
	smcp_daemon_trigger_custom_event_with_node(smcp,
		&self->node,
		"!fire",
		NULL,
		NULL
	);
#endif

	if(self->autorestart) {
		smcp_timer_init(&self->timer, &smcp_timer_node_fired, NULL, self);
		smcp_daemon_schedule_timer(
			smcp,
			&self->timer,
			self->period
		);
	} else {
		self->remaining = 0;
		smcp_daemon_trigger_custom_event_with_node(
			smcp,
			&self->node,
			"running",
			(void*)&smcp_static_content_fetcher,
			(void*)"0"
		);
	}
}

void smcp_timer_node_start(smcp_timer_node_t self) {
	if(!smcp_daemon_timer_is_scheduled(self->smcpd_instance,
			&self->timer)) {
		smcp_timer_init(&self->timer, &smcp_timer_node_fired, NULL, self);
		if(self->remaining)
			smcp_daemon_schedule_timer(self->smcpd_instance,
				&self->timer,
				self->remaining);
		else
			smcp_daemon_schedule_timer(self->smcpd_instance,
				&self->timer,
				self->period);
		smcp_daemon_trigger_custom_event_with_node(
			self->smcpd_instance,
			&self->node,
			"running",
			(void*)&smcp_static_content_fetcher,
			(void*)"1"
		);
	}
}

void smcp_timer_node_stop(smcp_timer_node_t self) {
	if(smcp_daemon_timer_is_scheduled(self->smcpd_instance,
			&self->timer)) {
		self->remaining = convert_timeval_to_cms(&self->timer.fire_date);
		smcp_daemon_invalidate_timer(self->smcpd_instance, &self->timer);
		smcp_daemon_trigger_custom_event_with_node(
			self->smcpd_instance,
			&self->node,
			"running",
			(void*)&smcp_static_content_fetcher,
			(void*)"0"
		);
	}
}

void smcp_timer_node_toggle(smcp_timer_node_t self) {
	if(smcp_daemon_timer_is_scheduled(self->smcpd_instance, &self->timer))
		smcp_timer_node_stop(self);
	else
		smcp_timer_node_start(self);
}

void smcp_timer_node_restart(smcp_timer_node_t self) {
	bool previously_running = smcp_daemon_timer_is_scheduled(
		self->smcpd_instance,
		&self->timer
	);

	if(previously_running)
		smcp_timer_node_stop(self);

	self->remaining = self->period;
#if SMCP_CONF_TIMER_NODE_INCLUDE_COUNT
	self->count = 0;
#endif

	if(!previously_running) {
		smcp_timer_node_start(self);
	} else {
		smcp_timer_init(&self->timer, &smcp_timer_node_fired, NULL, self);
		smcp_daemon_schedule_timer(self->smcpd_instance,
			&self->timer,
			self->period);
	}
}

void smcp_timer_node_reset(smcp_timer_node_t self) {
	if(smcp_daemon_timer_is_scheduled(self->smcpd_instance,
			&self->timer) || self->remaining == 0)
		smcp_timer_node_restart(self);
	else
		self->remaining = self->period;
#if SMCP_CONF_TIMER_NODE_INCLUDE_COUNT
	self->count = 0;
#endif
}

int smcp_timer_node_get_remaining(smcp_timer_node_t self) {
	if(smcp_daemon_timer_is_scheduled(self->smcpd_instance, &self->timer))
		return convert_timeval_to_cms(&self->timer.fire_date);
	return self->remaining;
}

void smcp_timer_node_set_autorestart(
	smcp_timer_node_t self, bool x
) {
	self->autorestart = x;
}

static smcp_status_t
smcp_timer_request_handler(
	smcp_node_t		node,
	smcp_method_t	method,
	const char*		path,
	const char*		content,
	size_t			content_length
) {
	smcp_status_t ret = 0;
	const smcp_daemon_t self = smcp_get_current_daemon();
	coap_content_type_t content_type =
	    SMCP_CONTENT_TYPE_APPLICATION_FORM_URLENCODED;
	char* query = NULL;

	{	// Get the query and content type from the headers.
		const coap_header_item_t *iter =
			smcp_daemon_get_current_request_headers();
		const coap_header_item_t *end = iter +
			smcp_daemon_get_current_request_header_count();

		for(; iter != end; ++iter) {
			if(iter->key == COAP_HEADER_CONTENT_TYPE) {
				content_type = *(unsigned char*)iter->value;
			} else if(iter->key == COAP_HEADER_URI_QUERY) {
				query = alloca(iter->value_len + 1);
				memcpy(query, iter->value, iter->value_len);
				query[iter->value_len] = 0;
			}
		}
	}

	if(!query &&
	        (content_type == SMCP_CONTENT_TYPE_APPLICATION_FORM_URLENCODED))
		query = (char*)content;

	if(!query)
		query = "";

	// TODO: Clean this up!
	if(!path || path[0] == 0) {
		if((method == COAP_METHOD_PUT) || (method == COAP_METHOD_POST)) {
			if(strhasprefix_const(query, "reset")) {
				smcp_timer_node_reset((smcp_timer_node_t)node);
			} else if(strequal_const(query, "restart")) {
				smcp_timer_node_reset((smcp_timer_node_t)node);
			} else {
				ret = SMCP_STATUS_NOT_ALLOWED;
			}
		} else if(method == COAP_METHOD_GET) {
			smcp_message_begin_response(HTTP_TO_COAP_CODE(HTTP_RESULT_CODE_OK));
			content_type = COAP_CONTENT_TYPE_APPLICATION_LINK_FORMAT;
			smcp_message_add_header(
				COAP_HEADER_CONTENT_TYPE,
				(const void*)&content_type,
				1
			);
			smcp_message_set_content(list_content, COAP_HEADER_CSTR_LEN);
			smcp_message_send();
		} else {
			ret = SMCP_STATUS_NOT_ALLOWED;
		}
	} else if(strhasprefix_const(path, "running")) {
		if(method == COAP_METHOD_GET) {
			smcp_message_begin_response(HTTP_TO_COAP_CODE(HTTP_RESULT_CODE_OK));
			smcp_message_set_var_content_int(
				smcp_daemon_timer_is_scheduled(
					smcp_get_current_daemon(),
					&((smcp_timer_node_t)node)->timer
				)
			);
			smcp_message_send();
		} else if((method == COAP_METHOD_PUT) ||
		        (method == COAP_METHOD_POST)) {
			char* key = NULL;
			char* value = NULL;
			while(url_form_next_value((char**)&query, &key,
					&value) && key && value) {
				if(strequal_const(key, "v")) {
					if(isdigit(value[0])) {
						if(strtol(value, NULL, 0))
							smcp_timer_node_start((smcp_timer_node_t)node);
						else
							smcp_timer_node_stop((smcp_timer_node_t)node);
					} else if(strequal_const(value, "!v")) {
						smcp_timer_node_toggle((smcp_timer_node_t)node);
					}
					break;
				}
			}
/*
		} else if(method == COAP_METHOD_PAIR) {
			ret = smcp_default_request_handler(
				node,
				method,
				path,
				content,
				content_length
			    );
			goto bail;
*/
		} else {
			ret = SMCP_STATUS_NOT_ALLOWED;
		}
	} else if(strequal_const(path, "!fire")) {
		ret = SMCP_STATUS_NOT_ALLOWED;
/*
		if(method == COAP_METHOD_PAIR) {
			ret = smcp_default_request_handler(
				node,
				method,
				path,
				content,
				content_length
			    );
			goto bail;
		} else {
			ret = SMCP_STATUS_NOT_ALLOWED;
		}
*/
	} else if(strhasprefix_const(path, "period")) {
		if(method == COAP_METHOD_GET) {
			smcp_message_begin_response(HTTP_TO_COAP_CODE(HTTP_RESULT_CODE_OK));
			smcp_message_set_var_content_unsigned_long_int(((smcp_timer_node_t)node)->period);
			smcp_message_send();
		} else if((method == COAP_METHOD_PUT) ||
		        (method == COAP_METHOD_POST)) {
			char* key = NULL;
			char* value = NULL;
			while(url_form_next_value((char**)&query, &key,
					&value) && key && value) {
				if(strequal_const(key, "v")) {
					    ((smcp_timer_node_t)node)->period = strtol(value,
						NULL,
						0);
					break;
				}
			}
		} else {
			ret = SMCP_STATUS_NOT_ALLOWED;
		}
	} else if(strhasprefix_const(path, "remaining")) {
		if(method == COAP_METHOD_GET) {
			smcp_message_begin_response(HTTP_TO_COAP_CODE(HTTP_RESULT_CODE_OK));
			smcp_message_set_var_content_int(smcp_timer_node_get_remaining((smcp_timer_node_t)node));
			smcp_message_send();
		} else if((method == COAP_METHOD_PUT) ||
		        (method == COAP_METHOD_POST)) {
			char* key = NULL;
			char* value = NULL;
			while(url_form_next_value((char**)&query, &key,
					&value) && key && value) {
				if(strequal_const(key, "v")) {
					    ((smcp_timer_node_t)node)->remaining = strtol(
						value,
						NULL,
						0);
					if(smcp_daemon_timer_is_scheduled(self,
							&((smcp_timer_node_t)node)->timer)) {
						smcp_daemon_invalidate_timer(self,
							&((smcp_timer_node_t)node)->timer);
						smcp_daemon_schedule_timer(self,
							&((smcp_timer_node_t)node)->timer,
							    ((smcp_timer_node_t)node)->remaining);
					}

					break;
				}
			}
		} else {
			ret = SMCP_STATUS_NOT_ALLOWED;
		}
	} else if(strhasprefix_const(path, "autorestart")) {
		if(method == COAP_METHOD_GET) {
			smcp_message_begin_response(HTTP_TO_COAP_CODE(HTTP_RESULT_CODE_OK));
			smcp_message_set_var_content_int(((smcp_timer_node_t)node)->autorestart);
			smcp_message_send();
		} else if((method == COAP_METHOD_PUT) ||
		        (method == COAP_METHOD_POST)) {
			char* key = NULL;
			char* value = NULL;
			while(url_form_next_value((char**)&query, &key,
					&value) && key && value) {
				if(strequal_const(key, "v")) {
					((smcp_timer_node_t)node)->autorestart =
						!!strtol(value,NULL,10);

					smcp_daemon_trigger_custom_event_with_node(
						smcp_get_current_daemon(),
						node,
						"autorestart",
						(void*)smcp_static_content_fetcher,//smcp_content_fetcher_func contentFetcher,
						((smcp_timer_node_t)node)->autorestart?"1":"0"//void *context
					);

					break;
				}
			}
/*
		} else if(method == COAP_METHOD_PAIR) {
			ret = smcp_default_request_handler(
				node,
				method,
				path,
				content,
				content_length
			);
			goto bail;
*/
		} else {
			ret = SMCP_STATUS_NOT_ALLOWED;
		}
	} else {
		ret = smcp_default_request_handler(
			node,
			method,
			path,
			content,
			content_length
		);
		goto bail;
	}

bail:
	return ret;
}


smcp_timer_node_t
smcp_timer_node_init(
	smcp_timer_node_t	self,
	smcp_daemon_t		smcp,
	smcp_node_t			parent,
	const char*			name
) {
	require(self || (self = smcp_timer_node_alloc()), bail);

	require(smcp_node_init(
			&self->node,
			    (void*)parent,
			name
		), bail);

	    ((smcp_node_t)&self->node)->request_handler =
	    &smcp_timer_request_handler;

	smcp_timer_init(&self->timer, &smcp_timer_node_fired, NULL, self);

	self->autorestart = true;
	self->period = 5 * MSEC_PER_SEC;
	self->smcpd_instance = smcp;

bail:
	return self;
}