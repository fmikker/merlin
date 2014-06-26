/*
 * Process In/Out
 *
 * This file contains functions that shuffle data from the module part of
 * the module (the timed and triggered events) to the thread part of the
 * module (the multiplexing networker), as well as functions that re-insert
 * the data from the network to the running Nagios process.
 * In short, these functions are only called from the triggered event
 * thingie.
 */
#include "module.h"
#include <nagios/objects.h>
#include <nagios/neberrors.h>
#include <nagios/common.h>
#include <nagios/comments.h>

static nebstruct_comment_data *block_comment;
static int check_dupes;
static merlin_event last_pkt;
static unsigned long long dupes, dupe_bytes;
static uint32_t event_mask;

struct merlin_check_stats {
	unsigned long long poller, peer, self, orphaned;
};
static struct merlin_check_stats service_checks, host_checks;


#ifdef DEBUG_DUPES_CAREFULLY
#define mos_case(vname) \
	if (offset >= offsetof(monitored_object_state, vname) && \
	    offset < offsetof(monitored_object_state, vname) + sizeof(mss.vname)) \
			return #vname
static const char *mos_offset_name(int offset)
{
	monitored_object_state mss;

	if (offset >= sizeof(monitored_object_state)) {
		return "string area";
	}

	mos_case(initial_state);
	mos_case(flap_detection_enabled);
	mos_case(low_flap_threshold);
	mos_case(high_flap_threshold);
	mos_case(check_freshness);
	mos_case(freshness_threshold);
	mos_case(process_performance_data);
	mos_case(checks_enabled);
	mos_case(accept_passive_checks);
	mos_case(event_handler_enabled);
	mos_case(obsess);
	mos_case(problem_has_been_acknowledged);
	mos_case(acknowledgement_type);
	mos_case(check_type);
	mos_case(current_state);
	mos_case(last_state);
	mos_case(last_hard_state);
	mos_case(state_type);
	mos_case(current_attempt);
	mos_case(current_event_id);
	mos_case(last_event_id);
	mos_case(current_problem_id);
	mos_case(last_problem_id);
	mos_case(latency);
	mos_case(execution_time);
	mos_case(notifications_enabled);
	mos_case(last_notification);
	mos_case(next_notification);
	mos_case(next_check);
	mos_case(should_be_scheduled);
	mos_case(last_check);
	mos_case(last_state_change);
	mos_case(last_hard_state_change);
	mos_case(last_time_up);
	mos_case(last_time_down);
	mos_case(last_time_unreachable);
	mos_case(has_been_checked);
	mos_case(current_notification_number);
	mos_case(current_notification_id);
	mos_case(check_flapping_recovery_notification);
	mos_case(scheduled_downtime_depth);
	mos_case(pending_flex_downtime);
	mos_case(is_flapping);
	mos_case(flapping_comment_id);
	mos_case(percent_state_change);
	mos_case(plugin_output);
	mos_case(long_plugin_output);
	mos_case(perf_data);

	return NULL;
}
#endif

static int is_dupe(merlin_event *pkt)
{
	if (!check_dupes) {
		return 0;
	}

	if (last_pkt.hdr.type != pkt->hdr.type) {
		return 0;
	}

	if (packet_size(&last_pkt) != packet_size(pkt)) {
		return 0;
	}

	/* if this is truly a dupe, return 1 and log every 100'th */
	if (!memcmp(&last_pkt, pkt, packet_size(pkt))) {
		dupe_bytes += packet_size(pkt);
		if (!(++dupes % 100)) {
			ldebug("%s in %llu duplicate packets dropped",
				   human_bytes(dupe_bytes), dupes);
		}
		return 1;
	}

#ifdef DEBUG_DUPES_CAREFULLY
	/*
	 * The "near-dupes" detection only works for host and
	 * service status events for now, so return early if
	 * the event type is neither of those
	 */
	if (pkt->hdr.type == NEBCALLBACK_SERVICE_STATUS_DATA ||
		pkt->hdr.type == NEBCALLBACK_HOST_STATUS_DATA)
	{
		int i, diffbytes = 0, diffvars = 0;
		const char *name, *last_name = NULL;
		unsigned char *a, *b;

		lwarn("Near-duplicate %s event created by Nagios",
			  callback_name(pkt->hdr.type));

		a = (unsigned char *)&last_pkt;
		b = (unsigned char *)pkt;
		for (i = 0; i < packet_size(pkt); i++) {
			if (a[i] == b[i])
				continue;
			diffbytes++;
			name = mos_offset_name(i);
			if (name && name == last_name)
				continue;
			diffvars++;
			ldebug("%s differs", name);
			last_name = name;
		}
		ldebug("%d variables and %d bytes differ", diffvars, diffbytes);
	}
#endif

	return 0;
}

static int send_generic(merlin_event *pkt, void *data)
{
	int result;

	/* avoid sending events that won't cause action in the daemon */
	if (!use_database && pkt->hdr.code == MAGIC_NONET && pkt->hdr.type != CTRL_PACKET) {
		return 0;
	}

	pkt->hdr.len = merlin_encode_event(pkt, data);
	if (!pkt->hdr.len) {
		lerr("Header len is 0 for callback %d. Update offset in hookinfo.h", pkt->hdr.type);
		return -1;
	}

	if (is_dupe(pkt)) {
		return 0;
	}

	/*
	 * preserve the event so we can check for dupes,
	 * but only if we successfully sent it
	 */
	result = ipc_send_event(pkt);
	ipc.stats.cb_count[pkt->hdr.type].out++;
	if (result < 0)
		memset(&last_pkt, 0, sizeof(last_pkt));
	else
		memcpy(&last_pkt, pkt, packet_size(pkt));

	return result;
}

static int get_selection(const char *key)
{
	node_selection *sel = node_selection_by_hostname(key);

	return sel ? sel->id & 0xffff : DEST_PEERS_MASTERS;
}

static int get_hostgroup_selection(const char *key)
{
	node_selection *sel = node_selection_by_name(key);

	return sel ? sel->id & 0xffff : DEST_PEERS_POLLERS;
}

static int send_host_status(merlin_event *pkt, int nebattr, host *obj)
{
	merlin_host_status st_obj;
	static host *last_obj = NULL;

	if (obj == merlin_recv_host)
		return 0;

	if (!obj) {
		lerr("send_host_status() called with NULL obj");
		return -1;
	}
	memset(&st_obj, 0, sizeof(st_obj));
	if (obj == last_obj) {
		check_dupes = 1;
	} else {
		check_dupes = 0;
		last_obj = obj;
	}

	st_obj.nebattr = nebattr;
	st_obj.name = obj->name;
	MOD2NET_STATE_VARS(st_obj.state, obj);
	if (!pkt->hdr.code)
		pkt->hdr.selection = get_selection(obj->name);

	return send_generic(pkt, &st_obj);
}

static int send_service_status(merlin_event *pkt, int nebattr, service *obj)
{
	merlin_service_status st_obj;
	static service *last_obj = NULL;

	if (!obj) {
		lerr("send_service_status() called with NULL obj");
		return -1;
	}
	memset(&st_obj, 0, sizeof(st_obj));
	if (obj == last_obj) {
		check_dupes = 1;
	} else {
		check_dupes = 0;
		last_obj = obj;
	}
	st_obj.nebattr = nebattr;
	st_obj.host_name = obj->host_name;
	st_obj.service_description = obj->description;
	MOD2NET_STATE_VARS(st_obj.state, obj);
	if (!pkt->hdr.code)
		pkt->hdr.selection = get_selection(obj->host_name);

	return send_generic(pkt, &st_obj);
}

static inline int should_run_check(unsigned int id)
{
	return assigned_peer(id, ipc.info.active_peers + 1) == ipc.peer_id;
}

/*
 * The hooks are called from broker.c in Nagios.
 */
static int hook_service_result(merlin_event *pkt, void *data)
{
	nebstruct_service_check_data *ds = (nebstruct_service_check_data *)data;
	service *s = (service *)ds->object_ptr;
	struct merlin_node *node;

	switch (ds->type) {
	case NEBTYPE_SERVICECHECK_ASYNC_PRECHECK:
		node = pgroup_service_node(s->id);
		schedule_expiration_event(SERVICE_CHECK, node, s);
		if (node != &ipc) {
			/* We're not responsible, so block this check here */
			return NEBERROR_CALLBACKCANCEL;
		}
		service_checks.self++;
		return 0;

	case NEBTYPE_SERVICECHECK_PROCESSED:
		unexpire_service(s);
		if (merlin_sender) {
			/* network-received events mustn't bounce back */
			pkt->hdr.code = MAGIC_NONET;
			set_service_check_node(merlin_sender, s, s->check_type == CHECK_TYPE_PASSIVE);
		} else {
			set_service_check_node(&ipc, s, ds->check_type == CHECK_TYPE_PASSIVE);
		}

		/* any check via check result transfer */
		if (merlin_recv_service == s)
			return 0;

		if (ds->check_type == CHECK_TYPE_PASSIVE) {
			/* never transfer passive checks to other nodes */
			pkt->hdr.code = MAGIC_NONET;
		}

		/*
		 * We fiddle with the last_check time here so that the time
		 * shown in nagios.log (for a service alert, e.g) is the same
		 * as that in the report_data to avoid (user) confusion
		 */
		s->last_check = (time_t) ds->end_time.tv_sec;
		return send_service_status(pkt, ds->attr, ds->object_ptr);
	}

	return 0;
}

static int hook_host_result(merlin_event *pkt, void *data)
{
	nebstruct_host_check_data *ds = (nebstruct_host_check_data *)data;
	struct host *h = (struct host *)ds->object_ptr;
	struct merlin_node *node;

	switch (ds->type) {
	case NEBTYPE_HOSTCHECK_ASYNC_PRECHECK:
	case NEBTYPE_HOSTCHECK_SYNC_PRECHECK:
		node = pgroup_host_node(h->id);
		schedule_expiration_event(HOST_CHECK, node, h);
		if (node != &ipc) {
			/* We're not responsible, so block this check here */
			return NEBERROR_CALLBACKCANCEL;
		}
		host_checks.self++;
		return 0;

	/* only send processed host checks */
	case NEBTYPE_HOSTCHECK_PROCESSED:
		unexpire_host(h);
		if (merlin_sender) {
			/* network-received events mustn't bounce back */
			pkt->hdr.code = MAGIC_NONET;
			set_host_check_node(merlin_sender, h, h->check_type == CHECK_TYPE_PASSIVE);
		} else {
			set_host_check_node(&ipc, h, ds->check_type == CHECK_TYPE_PASSIVE);
		}

		/* any check via check result transfer */
		if (merlin_recv_host == h)
			return 0;

		if (ds->check_type == CHECK_TYPE_PASSIVE) {
			/* never transfer passive checks to other nodes */
			pkt->hdr.code = MAGIC_NONET;
		}

		/*
		 * We fiddle with the last_check time here so that the time
		 * shown in nagios.log (for a service alert, e.g) is the same
		 * as that in the report_data to avoid (user) confusion
		 */
		h->last_check = (time_t) ds->end_time.tv_sec;
		return send_host_status(pkt, ds->attr, ds->object_ptr);
	}

	return 0;
}

/*
 * Comments are buggy as hell from Nagios, so we must block
 * some of them and make others cause object status events
 * pass through unmolested, even if they're being checked
 * by a poller.
 */
static int hook_comment(merlin_event *pkt, void *data)
{
	nebstruct_comment_data *ds = (nebstruct_comment_data *)data;

	/*
	 * comments always generate two events. One add and one load.
	 * We must make sure to skip one of them, and so far, load
	 * seems to be the sanest one to keep
	 */
	if (ds->type == NEBTYPE_COMMENT_ADD)
		return 0;

	/* avoid sending network-triggered comment events */
	if (merlin_sender != NULL)
		return 0;

	/*
	 * Downtime is notoriously tricky to handle since there are so many
	 * commands for scheduling it. We propagate downtime commands, but
	 * not downtime comments (since commands generate comments).
	 */
	if (ds->entry_type == DOWNTIME_COMMENT && ds->type != NEBTYPE_COMMENT_DELETE) {
		pkt->hdr.code = MAGIC_NONET;
	}

	/*
	 * same for acknowledgements
	 */
	if (ds->entry_type == ACKNOWLEDGEMENT_COMMENT && ds->type != NEBTYPE_COMMENT_DELETE) {
		pkt->hdr.code = MAGIC_NONET;
	}

	/*
	 * if the reaper thread is adding the comment we're getting an
	 * event for now, we'll need to block that comment from being
	 * sent to the daemon to avoid pingpong action and duplicate
	 * entries in the database.
	 */
	if (pkt->hdr.code != MAGIC_NONET && block_comment &&
		block_comment->entry_type == ds->entry_type &&
		block_comment->comment_type == ds->comment_type &&
		block_comment->expires == ds->expires &&
		block_comment->persistent == ds->persistent &&
		!strcmp(block_comment->host_name, ds->host_name) &&
		!strcmp(block_comment->author_name, ds->author_name) &&
		!strcmp(block_comment->comment_data, ds->comment_data) &&
		(block_comment->service_description == ds->service_description ||
		 !strcmp(block_comment->service_description, ds->service_description)))
	{
		/*
		 * This avoids USER_COMMENT and FLAPPING_COMMENT entry_type
		 * comments from bouncing back and forth indefinitely
		 */
		ldebug("CMNT: Marking event with MAGIC_NONET");
		pkt->hdr.code = MAGIC_NONET;
	} else {
		if (block_comment) {
			ldebug("We have a block_comment, but it doesn't match");
		}
		pkt->hdr.selection = get_selection(ds->host_name);
	}

	return send_generic(pkt, data);
}

static int hook_downtime(merlin_event *pkt, void *data)
{
	nebstruct_downtime_data *ds = (nebstruct_downtime_data *)data;

	/* avoid sending network-triggered downtime events */
	if (merlin_sender)
		return 0;

	/*
	 * Downtime delete and stop events are transferred.
	 * Adding is done on all nodes from the downtime command
	 * that always gets transferred, but if a user cancels
	 * downtime early, we get a "delete" event with attribute
	 * NEBATTR_DOWNTIME_STOP_CANCELLED that we must transfer
	 * properly, or the other node (which might be notifying)
	 * will think the node is still in downtime.
	 */
	if (ds->attr == NEBATTR_DOWNTIME_STOP_CANCELLED)
		pkt->hdr.selection = get_selection(ds->host_name);
	else
		pkt->hdr.code = MAGIC_NONET;

	return send_generic(pkt, data);
}

static int get_cmd_selection(char *cmd, int hostgroup)
{
	char *semi_colon;
	int ret;

	/*
	 * only global commands have no arguments at all. Those
	 * shouldn't end up here, but if they do we forward them
	 * to peers and pollers
	 */
	if (!cmd) {
		ldebug("Global command [%s] ended up in get_cmd_selection()", cmd);
		return DEST_PEERS_POLLERS;
	}

	semi_colon = strchr(cmd, ';');
	if (semi_colon)
		*semi_colon = '\0';
	if (!hostgroup) {
		ret = get_selection(cmd);
	} else {
		ret = get_hostgroup_selection(cmd);
	}
	if (semi_colon)
		*semi_colon = ';';

	return ret;
}

static int hook_external_command(merlin_event *pkt, void *data)
{
	nebstruct_external_command_data *ds = (nebstruct_external_command_data *)data;

	/*
	 * all comments generate two events, but we only want to
	 * send one of them, so focus on NEBTYPE_EXTERNALCOMMAND_END,
	 * since that one's only generated if the command is valid in
	 * the Nagios instance that generates it.
	 */
	if (ds->type != NEBTYPE_EXTERNALCOMMAND_END)
		return 0;

	switch (ds->command_type) {
		/*
		 * Comments are handled by their respective comment
		 * events, so we mustn't forward them.
		 */
	case CMD_DEL_HOST_COMMENT:
	case CMD_DEL_SVC_COMMENT:
	case CMD_ADD_HOST_COMMENT:
	case CMD_ADD_SVC_COMMENT:
		return 0;

		/*
		 * Custom notifications are always sent from the node where
		 * the user input the command, so we we must block them here
		 * in order to not generate multiple notifications.
		 */
	case CMD_SEND_CUSTOM_HOST_NOTIFICATION:
	case CMD_SEND_CUSTOM_SVC_NOTIFICATION:
		return 0;

		/*
		 * These only contain the downtime id, so they're mostly useless,
		 * but potentially dangerous.
		 * We'll forward the downtime_delete event instead.
		 */
	case CMD_DEL_HOST_DOWNTIME:
	case CMD_DEL_SVC_DOWNTIME:
		return 0;

		/*
		 * these are forwarded and handled specially on the
		 * receiving end
		 */
	case CMD_ACKNOWLEDGE_HOST_PROBLEM:
	case CMD_ACKNOWLEDGE_SVC_PROBLEM:

		/*
		 * downtime is a troll of its own. For now, downtime
		 * commands aren't blocked, but their comments are.
		 * We keep them stashed here though, in case we
		 * want to modify how we handle them later.
		 */
	case CMD_SCHEDULE_HOST_DOWNTIME:
	case CMD_SCHEDULE_SVC_DOWNTIME:
	case CMD_SCHEDULE_AND_PROPAGATE_TRIGGERED_HOST_DOWNTIME:
	case CMD_SCHEDULE_AND_PROPAGATE_HOST_DOWNTIME:
		/* fallthrough */

	case CMD_ENABLE_SVC_CHECK:
	case CMD_DISABLE_SVC_CHECK:
	case CMD_SCHEDULE_SVC_CHECK:
	case CMD_DELAY_SVC_NOTIFICATION:
	case CMD_DELAY_HOST_NOTIFICATION:
	case CMD_ENABLE_HOST_SVC_CHECKS:
	case CMD_DISABLE_HOST_SVC_CHECKS:
	case CMD_SCHEDULE_HOST_SVC_CHECKS:
	case CMD_DELAY_HOST_SVC_NOTIFICATIONS:
	case CMD_DEL_ALL_HOST_COMMENTS:
	case CMD_DEL_ALL_SVC_COMMENTS:
	case CMD_ENABLE_SVC_NOTIFICATIONS:
	case CMD_DISABLE_SVC_NOTIFICATIONS:
	case CMD_ENABLE_HOST_NOTIFICATIONS:
	case CMD_DISABLE_HOST_NOTIFICATIONS:
	case CMD_ENABLE_HOST_SVC_NOTIFICATIONS:
	case CMD_DISABLE_HOST_SVC_NOTIFICATIONS:
	case CMD_ENABLE_PASSIVE_SVC_CHECKS:
	case CMD_DISABLE_PASSIVE_SVC_CHECKS:
	case CMD_ENABLE_HOST_EVENT_HANDLER:
	case CMD_DISABLE_HOST_EVENT_HANDLER:
	case CMD_ENABLE_SVC_EVENT_HANDLER:
	case CMD_DISABLE_SVC_EVENT_HANDLER:
	case CMD_ENABLE_HOST_CHECK:
	case CMD_DISABLE_HOST_CHECK:
	case CMD_START_OBSESSING_OVER_SVC_CHECKS:
	case CMD_STOP_OBSESSING_OVER_SVC_CHECKS:
	case CMD_REMOVE_HOST_ACKNOWLEDGEMENT:
	case CMD_REMOVE_SVC_ACKNOWLEDGEMENT:
	case CMD_SCHEDULE_FORCED_HOST_SVC_CHECKS:
	case CMD_SCHEDULE_FORCED_SVC_CHECK:
	case CMD_ENABLE_HOST_FLAP_DETECTION:
	case CMD_DISABLE_HOST_FLAP_DETECTION:
	case CMD_ENABLE_SVC_FLAP_DETECTION:
	case CMD_DISABLE_SVC_FLAP_DETECTION:
	case CMD_DISABLE_PASSIVE_HOST_CHECKS:
	case CMD_SCHEDULE_HOST_CHECK:
	case CMD_SCHEDULE_FORCED_HOST_CHECK:
	case CMD_CHANGE_HOST_EVENT_HANDLER:
	case CMD_CHANGE_SVC_EVENT_HANDLER:
	case CMD_CHANGE_HOST_CHECK_COMMAND:
	case CMD_CHANGE_SVC_CHECK_COMMAND:
	case CMD_CHANGE_NORMAL_HOST_CHECK_INTERVAL:
	case CMD_CHANGE_NORMAL_SVC_CHECK_INTERVAL:
	case CMD_CHANGE_RETRY_SVC_CHECK_INTERVAL:
	case CMD_CHANGE_MAX_HOST_CHECK_ATTEMPTS:
	case CMD_CHANGE_MAX_SVC_CHECK_ATTEMPTS:
	case CMD_ENABLE_HOST_AND_CHILD_NOTIFICATIONS:
	case CMD_DISABLE_HOST_AND_CHILD_NOTIFICATIONS:
	case CMD_ENABLE_HOST_FRESHNESS_CHECKS:
	case CMD_DISABLE_HOST_FRESHNESS_CHECKS:
	case CMD_SET_HOST_NOTIFICATION_NUMBER:
	case CMD_SET_SVC_NOTIFICATION_NUMBER:
	case CMD_CHANGE_HOST_CHECK_TIMEPERIOD:
	case CMD_CHANGE_SVC_CHECK_TIMEPERIOD:
	case CMD_CHANGE_CUSTOM_HOST_VAR:
	case CMD_CHANGE_CUSTOM_SVC_VAR:
	case CMD_ENABLE_CONTACT_HOST_NOTIFICATIONS:
	case CMD_DISABLE_CONTACT_HOST_NOTIFICATIONS:
	case CMD_ENABLE_CONTACT_SVC_NOTIFICATIONS:
	case CMD_DISABLE_CONTACT_SVC_NOTIFICATIONS:
	case CMD_ENABLE_CONTACTGROUP_HOST_NOTIFICATIONS:
	case CMD_DISABLE_CONTACTGROUP_HOST_NOTIFICATIONS:
	case CMD_ENABLE_CONTACTGROUP_SVC_NOTIFICATIONS:
	case CMD_DISABLE_CONTACTGROUP_SVC_NOTIFICATIONS:
	case CMD_CHANGE_RETRY_HOST_CHECK_INTERVAL:
	case CMD_CHANGE_HOST_NOTIFICATION_TIMEPERIOD:
	case CMD_CHANGE_SVC_NOTIFICATION_TIMEPERIOD:
	case CMD_CHANGE_CONTACT_HOST_NOTIFICATION_TIMEPERIOD:
	case CMD_CHANGE_CONTACT_SVC_NOTIFICATION_TIMEPERIOD:
	case CMD_CHANGE_HOST_MODATTR:
	case CMD_CHANGE_SVC_MODATTR:
	case CMD_PROCESS_HOST_CHECK_RESULT:
	case CMD_PROCESS_SERVICE_CHECK_RESULT:
		/*
		 * looks like we have everything we need, so get the
		 * selection based on the hostname so the daemon knows
		 * which node(s) to send the command to (could very well
		 * be 'nowhere')
		 */
		pkt->hdr.selection = get_cmd_selection(ds->command_args, 0);
		break;

	/* XXX downtime stuff on top */
	/*
	 * service- and hostgroup commands get sent to all peers
	 * and pollers, but not to masters since we can't know if
	 * we'd affect more than our fair share of services on the
	 * master.
	 */
	case CMD_SCHEDULE_HOSTGROUP_HOST_DOWNTIME:
	case CMD_SCHEDULE_HOSTGROUP_SVC_DOWNTIME:
	case CMD_ENABLE_HOSTGROUP_SVC_NOTIFICATIONS:
	case CMD_DISABLE_HOSTGROUP_SVC_NOTIFICATIONS:
	case CMD_ENABLE_HOSTGROUP_HOST_NOTIFICATIONS:
	case CMD_DISABLE_HOSTGROUP_HOST_NOTIFICATIONS:
	case CMD_ENABLE_HOSTGROUP_SVC_CHECKS:
	case CMD_DISABLE_HOSTGROUP_SVC_CHECKS:
	case CMD_ENABLE_HOSTGROUP_HOST_CHECKS:
	case CMD_DISABLE_HOSTGROUP_HOST_CHECKS:
	case CMD_ENABLE_HOSTGROUP_PASSIVE_SVC_CHECKS:
	case CMD_DISABLE_HOSTGROUP_PASSIVE_SVC_CHECKS:
	case CMD_ENABLE_HOSTGROUP_PASSIVE_HOST_CHECKS:
	case CMD_DISABLE_HOSTGROUP_PASSIVE_HOST_CHECKS:
		pkt->hdr.selection = get_cmd_selection(ds->command_args, 1);
		break;
	case CMD_SCHEDULE_SERVICEGROUP_HOST_DOWNTIME:
	case CMD_SCHEDULE_SERVICEGROUP_SVC_DOWNTIME:
	case CMD_ENABLE_SERVICEGROUP_SVC_NOTIFICATIONS:
	case CMD_DISABLE_SERVICEGROUP_SVC_NOTIFICATIONS:
	case CMD_ENABLE_SERVICEGROUP_HOST_NOTIFICATIONS:
	case CMD_DISABLE_SERVICEGROUP_HOST_NOTIFICATIONS:
	case CMD_ENABLE_SERVICEGROUP_SVC_CHECKS:
	case CMD_DISABLE_SERVICEGROUP_SVC_CHECKS:
	case CMD_ENABLE_SERVICEGROUP_HOST_CHECKS:
	case CMD_DISABLE_SERVICEGROUP_HOST_CHECKS:
	case CMD_ENABLE_SERVICEGROUP_PASSIVE_SVC_CHECKS:
	case CMD_DISABLE_SERVICEGROUP_PASSIVE_SVC_CHECKS:
	case CMD_ENABLE_SERVICEGROUP_PASSIVE_HOST_CHECKS:
	case CMD_DISABLE_SERVICEGROUP_PASSIVE_HOST_CHECKS:
		if (num_masters) {
			linfo("Submitting servicegroup commands on pollers isn't necessarily a good idea");
		}
		pkt->hdr.selection = DEST_PEERS_POLLERS;
		break;

	default:
		/*
		 * global commands get filtered in the daemon so only
		 * peers and pollers get them, but we block them right
		 * here if we have neither of those
		 */
		if (!(num_peers + num_pollers)) {
			ldebug("No peers or pollers. Not sending command %d anywhere",
			       ds->command_type);
			return 0;
		}

		pkt->hdr.selection = DEST_PEERS_POLLERS;
		break;
	}

	return send_generic(pkt, data);
}

static int hook_contact_notification_method(merlin_event *pkt, void *data)
{
	nebstruct_contact_notification_method_data *ds =
		(nebstruct_contact_notification_method_data *)data;

	if (ds->type != NEBTYPE_CONTACTNOTIFICATIONMETHOD_END)
		return 0;

	return send_generic(pkt, data);
}

/*
 * Called when a notification chain starts. This is used to
 * avoid sending notifications from a node that isn't supposed
 * to send it
 */
static int hook_notification(merlin_event *pkt, void *data)
{
	nebstruct_notification_data *ds = (nebstruct_notification_data *)data;
	unsigned int id, check_type = 0, rtype;
	struct merlin_notify_stats *mns = NULL;
	struct service *s = NULL;
	struct host *h = NULL;

	if (ds->type == NEBTYPE_NOTIFICATION_END){
		if (ds->notification_type == HOST_NOTIFICATION) {
			host *hst = ds->object_ptr;
			ds->object_ptr = (void *)(uintptr_t)(hst->current_notification_number);
			ds->start_time.tv_usec = hst->no_more_notifications;
			ds->start_time.tv_sec = hst->last_notification;
			ds->end_time.tv_usec = 0;
			ds->end_time.tv_sec = hst->next_notification;
		} else if (ds->notification_type == SERVICE_NOTIFICATION) {
			service *svc = ds->object_ptr;
			ds->object_ptr = (void *)(uintptr_t)(svc->current_notification_number);
			ds->start_time.tv_usec = svc->no_more_notifications;
			ds->start_time.tv_sec = svc->last_notification;
			ds->end_time.tv_usec = 0;
			ds->end_time.tv_sec = svc->next_notification;
		} else {
			lerr("Unknown notification type %i", ds->notification_type);
		}
		return send_generic(pkt, data);
	}

	/* don't count or (try to) block notifications after they're sent */
	if (ds->type != NEBTYPE_NOTIFICATION_START)
		return 0;

	if (ds->notification_type == SERVICE_NOTIFICATION) {
		s = (service *)ds->object_ptr;
		check_type = s->check_type;
		id = s->id;
		ldebug("notif: Checking service notification for %s;%s",
		       s->host_name, s->description);
	} else {
		h = (struct host *)ds->object_ptr;
		id = h->id;
		check_type = h->check_type;
		ldebug("notif: Checking host notification for %s", h->name);
	}

	/* handle NOTIFICATION_CUSTOM being 99 in some releases */
	rtype = ds->reason_type;
	if (rtype > 8)
		rtype = 8;
	mns = &merlin_notify_stats[rtype][ds->notification_type][check_type];

	/* Break out if we only notify when no masters are present and we have masters */
	if (online_masters && !(ipc.flags & MERLIN_NODE_NOTIFIES)) {
		ldebug("notif: poller blocking notification in favour of master");
		mns->master++;
		return NEBERROR_CALLBACKCANCEL;
	}

	/*
	 * network-received events can go one of two ways:
	 * If the sender is a poller that can't notify on its own, we may
	 * have to send the notification, unless one of our peers is
	 * supposed to do it.
	 * If the sender is not a poller, or the poller can notify, we
	 * must never notify on this state.
	 */
	if (merlin_sender) {
		ldebug("notif: merlin_sender is %s %s", node_type(merlin_sender), merlin_sender->name);
		if (merlin_sender->type != MODE_POLLER) {
			mns->net++;
			ldebug("notif: Sender is not a poller. Cancelling notification");
			return NEBERROR_CALLBACKCANCEL;
		}
		if (merlin_sender->flags & MERLIN_NODE_NOTIFIES) {
			ldebug("notif: Poller can notify. Cancelling notification");
			return NEBERROR_CALLBACKCANCEL;
		}
		/*
		 * seems the sender is a poller that can't notify, so check if
		 * we should do it and, if so, allow it
		 */
		if (!num_peers || should_run_check(id)) {
			mns->sent++;
			ldebug("notif: Poller can't notify and we're responsible, so notifying");
			return 0;
		}

		ldebug("notif: A peer handles poller-sent check. Blocking notifications");
		mns->peer++;
		return NEBERROR_CALLBACKCANCEL;
	}

	/* never block normal, local notificatons from passive checks */
	if (check_type == CHECK_TYPE_PASSIVE && ds->reason_type == NOTIFICATION_NORMAL) {
		ldebug("notif: passive check delivered to us, so we notify");
		mns->sent++;
		return 0;
	}

	/* if we have no peers we won't block the notification at this point */
	if (!num_peers) {
		ldebug("notif: We have no peers, so won't block notification");
		mns->sent++;
		return 0;
	}

	/*
	 * command-triggered notifications are sent immediately
	 * from the node where they originated and blocked
	 * everywhere else
	 */
	switch (ds->reason_type) {
	case NOTIFICATION_ACKNOWLEDGEMENT:
	case NOTIFICATION_CUSTOM:
		ldebug("notif: command-triggered and delivered to us, so allowing");
		mns->sent++;
		return 0;
	}

	if (!num_peers || should_run_check(id)) {
		ldebug("notif: We're responsible for this notification, so allowing it");
		return 0;
	} else {
		ldebug("notif: Blocking notification. A peer is supposed to send it");
		mns->peer++;
		return NEBERROR_CALLBACKCANCEL;
	}

	mns->sent++;
	ldebug("notif: Fell through to the end");
	return 0;
}

int merlin_mod_hook(int cb, void *data)
{
	merlin_event pkt;
	int result = 0;
	static time_t last_pulse = 0;
	time_t now;

	if (!data) {
		lerr("eventbroker module called with NULL data");
		return -1;
	} else if (cb < 0 || cb > NEBCALLBACK_NUMITEMS) {
		lerr("merlin_mod_hook() called with invalid callback id");
		return -1;
	}

	/*
	 * must reset this here so events we don't check for
	 * dupes are always sent properly
	 */
	check_dupes = 0;

	/* If we've lost sync, we must make sure we send the paths again */
	if (merlin_should_send_paths && merlin_should_send_paths < time(NULL)) {
		/* send_paths resets merlin_should_send_paths if successful */
		send_paths();
	}

	/* self-heal nodes that have missed out on the fact that we're up */
	now = time(NULL);
	if(!last_pulse || now - last_pulse > 15)
		node_send_ctrl_active(&ipc, CTRL_GENERIC, &ipc.info, 0);
	last_pulse = now;

	memset(&pkt, 0, sizeof(pkt));
	pkt.hdr.type = cb;
	pkt.hdr.selection = DEST_BROADCAST;
	switch (cb) {
	case NEBCALLBACK_NOTIFICATION_DATA:
		result = hook_notification(&pkt, data);
		break;

	case NEBCALLBACK_CONTACT_NOTIFICATION_METHOD_DATA:
		result = hook_contact_notification_method(&pkt, data);
		break;

	case NEBCALLBACK_HOST_CHECK_DATA:
		result = hook_host_result(&pkt, data);
		break;

	case NEBCALLBACK_SERVICE_CHECK_DATA:
		result = hook_service_result(&pkt, data);
		break;

	case NEBCALLBACK_COMMENT_DATA:
		result = hook_comment(&pkt, data);
		break;

	case NEBCALLBACK_DOWNTIME_DATA:
		result = hook_downtime(&pkt, data);
		break;

	case NEBCALLBACK_EXTERNAL_COMMAND_DATA:
		result = hook_external_command(&pkt, data);
		break;

	case NEBCALLBACK_FLAPPING_DATA:
		result = send_generic(&pkt, data);
		break;

	case NEBCALLBACK_PROGRAM_STATUS_DATA:
		result = send_generic(&pkt, data);
		break;

	case NEBCALLBACK_PROCESS_DATA:
		result = send_generic(&pkt, data);
		break;

	case NEBCALLBACK_HOST_STATUS_DATA:
	case NEBCALLBACK_SERVICE_STATUS_DATA:
		lerr("Got %s, which is unhandled nowadays. What voodoo is this?\n", callback_name(cb));
		break;
	default:
		lerr("Unhandled callback '%s' in merlin_hook()", callback_name(cb));
	}

	if (result < 0) {
		lwarn("Daemon is flooded and backlogging failed. Staying dormant for %d seconds", MERLIN_SENDPATH_INTERVAL);
		merlin_should_send_paths = time(NULL) + MERLIN_SENDPATH_INTERVAL;
	}

	return result;
}

#define CB_ENTRY(pollers_only, type, hook) \
	{ pollers_only, type, #type, #hook }
static struct callback_struct {
	int network_only;
	int type;
	const char *name;
	const char *hook_name;
} callback_table[] = {
	CB_ENTRY(0, NEBCALLBACK_PROCESS_DATA, hook_generic),
/*
	CB_ENTRY(0, NEBCALLBACK_LOG_DATA, hook_generic),
	CB_ENTRY(0, NEBCALLBACK_SYSTEM_COMMAND_DATA, hook_generic),
	CB_ENTRY(0, NEBCALLBACK_EVENT_HANDLER_DATA, hook_generic),
*/
	CB_ENTRY(1, NEBCALLBACK_NOTIFICATION_DATA, hook_notification),
/*	CB_ENTRY(0, NEBCALLBACK_CONTACT_NOTIFICATION_DATA, hook_contact_notification),
 */
	CB_ENTRY(0, NEBCALLBACK_CONTACT_NOTIFICATION_METHOD_DATA, hook_contact_notification_method),

	CB_ENTRY(0, NEBCALLBACK_SERVICE_CHECK_DATA, hook_service_result),
	CB_ENTRY(0, NEBCALLBACK_HOST_CHECK_DATA, hook_host_result),
	CB_ENTRY(0, NEBCALLBACK_COMMENT_DATA, hook_generic),
	CB_ENTRY(0, NEBCALLBACK_DOWNTIME_DATA, hook_generic),
	CB_ENTRY(0, NEBCALLBACK_FLAPPING_DATA, hook_generic),
	CB_ENTRY(0, NEBCALLBACK_PROGRAM_STATUS_DATA, hook_generic),
	CB_ENTRY(0, NEBCALLBACK_HOST_STATUS_DATA, hook_host_status),
	CB_ENTRY(0, NEBCALLBACK_SERVICE_STATUS_DATA, hook_service_status),
	CB_ENTRY(1, NEBCALLBACK_EXTERNAL_COMMAND_DATA, hook_generic),
};

extern void *neb_handle;
int merlin_hooks_init(uint32_t mask)
{
	uint i;
	event_mask = mask;

	for (i = 0; i < ARRAY_SIZE(callback_table); i++) {
		struct callback_struct *cb = &callback_table[i];

		if (cb->network_only && !num_nodes) {
			ldebug("No pollers, peers or masters. Ignoring %s events", callback_name(cb->type));
			continue;
		}

		/* ignored filtered-out eventtypes */
		if (!(mask & (1 << cb->type))) {
			ldebug("EVENTFILTER: Ignoring %s events from Nagios", callback_name(cb->type));
			continue;
		}

		neb_register_callback(cb->type, neb_handle, 0, merlin_mod_hook);
	}

	return 0;
}

/*
 * We ignore any event masks here. Nagios should handle a module
 * unloading a function it hasn't registered gracefully anyways.
 */
int merlin_hooks_deinit(void)
{
	uint i;

	for (i = 0; i < ARRAY_SIZE(callback_table); i++) {
		struct callback_struct *cb = &callback_table[i];

		if (!num_nodes && cb->network_only)
			continue;

		if (event_mask & (1 << cb->type))
			neb_deregister_callback(cb->type, merlin_mod_hook);
	}

	return 0;
}

void merlin_set_block_comment(nebstruct_comment_data *cmnt)
{
	block_comment = cmnt;
}
