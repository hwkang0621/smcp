#include <AssertMacros.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include "smcp.h"
#include <string.h>
#include <sys/errno.h>


smcp_status_t
action_func(
	smcp_action_node_t	node,
	const char*			headers[],
	const char*			content,
	size_t				content_length,
	const char*			content_type,
	struct sockaddr*	saddr,
	socklen_t			socklen,
	void*				context
) {
	fprintf(stderr,
		"Received Action! content_length = %d\n",
		    (int)content_length);
	if(content_length) fprintf(stderr, "content = \"%s\"\n", content);
	return 0;
}

smcp_status_t
loadavg_get_func(
	smcp_variable_node_t	node,
	const char*				headers[],
	char*					content,
	size_t*					content_length,
	const char**			content_type,
	struct sockaddr*		saddr,
	socklen_t				socklen,
	void*					context
) {
	int ret = 0;
	double loadavg[3] = { };

	require_action(node, bail, ret = -1);
	require_action(content, bail, ret = -1);
	require_action(content_length, bail, ret = -1);

	getloadavg(loadavg, sizeof(loadavg) / sizeof(*loadavg));

	snprintf(content, *content_length, "v=%0.2f", loadavg[0]);
	*content_length = strlen(content);
	*content_type = SMCP_CONTENT_TYPE_FORM;

bail:
	return ret;
}

smcp_variable_node_t create_load_average_variable_node(
	smcp_device_node_t parent, const char* name
) {
	smcp_variable_node_t ret = NULL;
	smcp_event_node_t changed_event = NULL;

	ret = smcp_node_add_variable((void*)parent, name);

	changed_event = smcp_node_add_event((void*)ret, "changed");
	ret->get_func = loadavg_get_func;

	return ret;
}


void list_response_handler(
	smcp_daemon_t		self,
	const char*			version,
	int					statuscode,
	const char*			headers[],
	const char*			content,
	size_t				content_length,
	struct sockaddr*	saddr,
	socklen_t			socklen,
	void*				context
) {
	printf("*** GOT LIST RESPONSE!!! ***\n");
	printf("*** STATUS  CODE = %d\n", statuscode);

	if(content) {
		char contentBuffer[500] = "";
		memcpy(contentBuffer, content, content_length);

		printf("*** CONTENT = \"%s\"\n", contentBuffer);
	}
}


static bool
util_add_header(
	const char* headerList[],
	int			maxHeaders,
	const char* name,
	const char* value
) {
	for(; maxHeaders && headerList[0]; maxHeaders--, headerList += 2) ;
	if(maxHeaders) {
		headerList[0] = name;
		headerList[1] = value;
		headerList[2] = NULL;
		return true;
	}
	return false;
}


int
main(
	int argc, const char * argv[]
) {
	int i;
	smcp_daemon_t smcp_daemon;
	smcp_daemon_t smcp_daemon2;

	smcp_device_node_t device_node;
	smcp_action_node_t action_node;
	smcp_variable_node_t var_node;

//	smcp_event_node_t event_node;
//	char tmp_string[255] = "???";

	smcp_daemon = smcp_daemon_create(0);
	smcp_daemon2 = smcp_daemon_create(54321);

	smcp_node_add_subdevice(smcp_daemon_get_root_node(
			smcp_daemon), "device2");

	device_node =
	    smcp_node_add_subdevice(smcp_daemon_get_root_node(
			smcp_daemon), "device");
//	event_node = smcp_node_add_event((smcp_node_t)device_node, "event");

	smcp_node_add_event((smcp_node_t)device_node, "event");
	smcp_node_add_subdevice(smcp_daemon_get_root_node(
			smcp_daemon), "device3");
	smcp_node_add_subdevice(smcp_daemon_get_root_node(
			smcp_daemon), "device4");

	var_node = create_load_average_variable_node(device_node, "loadavg");

	action_node =
	    smcp_node_add_action(smcp_daemon_get_root_node(
			smcp_daemon2), "action");
	action_node->post_func = action_func;


	smcp_node_pair_with_uri((smcp_node_t)smcp_node_find_with_path((
			    smcp_node_t)var_node,
			"!changed"), "smcp://127.0.0.1:54321/?action", 0);
	smcp_node_pair_with_uri((smcp_node_t)action_node,
		"smcp://127.0.0.1/device/loadavg!changed",
		0);

	{
		const char* headers[SMCP_MAX_HEADERS * 2 + 1] = { NULL };
		char idValue[30];
		snprintf(idValue, sizeof(idValue), "%08x", arc4random());

		util_add_header(headers, SMCP_MAX_HEADERS, SMCP_HEADER_ID, idValue);
		//util_add_header(headers,SMCP_MAX_HEADERS,SMCP_HEADER_NEXT,"device2/");

		smcp_daemon_add_response_handler(
			smcp_daemon2,
			idValue,
			5000,
			0, // Flags
			&list_response_handler,
			NULL
		);

		smcp_daemon_send_request_to_url(
			smcp_daemon2,
			SMCP_METHOD_LIST,
#if 0
			"smcp://["SMCP_IPV 6_MULTICAST_ADDRESS "]/device/",
#else
			"smcp://[::1]/device/",
#endif
			"SMCP/0.1",
			headers,
			NULL,
			0
		);
	}

	for(i = 0; i < 300; i++) {
		if(i % 50 == 0)
			smcp_daemon_refresh_variable(smcp_daemon, var_node);
		smcp_daemon_process(smcp_daemon, 50);
		smcp_daemon_process(smcp_daemon2, 50);
	}

	smcp_daemon_release(smcp_daemon);
	smcp_daemon_release(smcp_daemon2);

	return 0;
}
