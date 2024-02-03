/*
 * Copyright 2004-2024 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU General Public License version 2
 * or later (GPLv2+) WITHOUT ANY WARRANTY.
 */

#include <crm_internal.h>

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>

#include <sys/param.h>
#include <sys/types.h>

#include <glib.h>
#include <libxml/tree.h>

#include <crm/crm.h>
#include <crm/cib/internal.h>

#include <crm/common/xml.h>
#include <crm/common/ipc_internal.h>
#include <crm/common/xml_internal.h>
#include <crm/cluster/internal.h>

#include <pacemaker-based.h>

/* Maximum number of diffs to ignore while waiting for a resync */
#define MAX_DIFF_RETRY 5

bool based_is_primary = false;

xmlNode *the_cib = NULL;

int
cib_process_shutdown_req(const char *op, int options, const char *section, xmlNode * req,
                         xmlNode * input, xmlNode * existing_cib, xmlNode ** result_cib,
                         xmlNode ** answer)
{
    const char *host = crm_element_value(req, PCMK__XA_SRC);

    *answer = NULL;

    if (crm_element_value(req, PCMK__XA_CIB_ISREPLYTO) == NULL) {
        crm_info("Peer %s is requesting to shut down", host);
        return pcmk_ok;
    }

    if (cib_shutdown_flag == FALSE) {
        crm_err("Peer %s mistakenly thinks we wanted to shut down", host);
        return -EINVAL;
    }

    crm_info("Peer %s has acknowledged our shutdown request", host);
    terminate_cib(__func__, 0);
    return pcmk_ok;
}

// @COMPAT: Remove when PCMK__CIB_REQUEST_NOOP is removed
int
cib_process_noop(const char *op, int options, const char *section, xmlNode *req,
                 xmlNode *input, xmlNode *existing_cib, xmlNode **result_cib,
                 xmlNode **answer)
{
    crm_trace("Processing \"%s\" event", op);
    *answer = NULL;
    return pcmk_ok;
}

int
cib_process_readwrite(const char *op, int options, const char *section, xmlNode * req,
                      xmlNode * input, xmlNode * existing_cib, xmlNode ** result_cib,
                      xmlNode ** answer)
{
    int result = pcmk_ok;

    crm_trace("Processing \"%s\" event", op);

    if (pcmk__str_eq(op, PCMK__CIB_REQUEST_IS_PRIMARY, pcmk__str_none)) {
        if (based_is_primary) {
            result = pcmk_ok;
        } else {
            result = -EPERM;
        }
        return result;
    }

    if (pcmk__str_eq(op, PCMK__CIB_REQUEST_PRIMARY, pcmk__str_none)) {
        if (!based_is_primary) {
            crm_info("We are now in R/W mode");
            based_is_primary = true;
        } else {
            crm_debug("We are still in R/W mode");
        }

    } else if (based_is_primary) {
        crm_info("We are now in R/O mode");
        based_is_primary = false;
    }

    return result;
}

/* Set to 1 when a sync is requested, incremented when a diff is ignored,
 * reset to 0 when a sync is received
 */
static int sync_in_progress = 0;

void
send_sync_request(const char *host)
{
    xmlNode *sync_me = create_xml_node(NULL, "sync-me");
    crm_node_t *peer = NULL;

    crm_info("Requesting re-sync from %s", (host? host : "all peers"));
    sync_in_progress = 1;

    crm_xml_add(sync_me, PCMK__XA_T, T_CIB);
    crm_xml_add(sync_me, PCMK__XA_CIB_OP, PCMK__CIB_REQUEST_SYNC_TO_ONE);
    crm_xml_add(sync_me, PCMK__XA_CIB_DELEGATED_FROM,
                stand_alone? "localhost" : crm_cluster->uname);

    if (host != NULL) {
        peer = pcmk__get_node(0, host, NULL, pcmk__node_search_cluster);
    }
    send_cluster_message(peer, crm_msg_cib, sync_me, FALSE);
    free_xml(sync_me);
}

int
cib_process_ping(const char *op, int options, const char *section, xmlNode * req, xmlNode * input,
                 xmlNode * existing_cib, xmlNode ** result_cib, xmlNode ** answer)
{
    const char *host = crm_element_value(req, PCMK__XA_SRC);
    const char *seq = crm_element_value(req, F_CIB_PING_ID);
    char *digest = calculate_xml_versioned_digest(the_cib, FALSE, TRUE, CRM_FEATURE_SET);

    crm_trace("Processing \"%s\" event %s from %s", op, seq, host);
    *answer = create_xml_node(NULL, PCMK__XE_PING_RESPONSE);

    crm_xml_add(*answer, PCMK_XA_CRM_FEATURE_SET, CRM_FEATURE_SET);
    crm_xml_add(*answer, PCMK__XA_DIGEST, digest);
    crm_xml_add(*answer, F_CIB_PING_ID, seq);

    pcmk__if_tracing(
        {
            // Append additional detail so the receiver can log the differences
            add_message_xml(*answer, PCMK__XA_CIB_CALLDATA, the_cib);
        },
        if (the_cib != NULL) {
            // Always include at least the version details
            xmlNode *shallow = create_xml_node(NULL,
                                               (const char *) the_cib->name);

            copy_in_properties(shallow, the_cib);
            add_message_xml(*answer, PCMK__XA_CIB_CALLDATA, shallow);
            free_xml(shallow);
        }
    );

    crm_info("Reporting our current digest to %s: %s for %s.%s.%s",
             host, digest,
             crm_element_value(existing_cib, PCMK_XA_ADMIN_EPOCH),
             crm_element_value(existing_cib, PCMK_XA_EPOCH),
             crm_element_value(existing_cib, PCMK_XA_NUM_UPDATES));

    free(digest);

    return pcmk_ok;
}

int
cib_process_sync(const char *op, int options, const char *section, xmlNode * req, xmlNode * input,
                 xmlNode * existing_cib, xmlNode ** result_cib, xmlNode ** answer)
{
    return sync_our_cib(req, TRUE);
}

int
cib_process_upgrade_server(const char *op, int options, const char *section, xmlNode * req, xmlNode * input,
                           xmlNode * existing_cib, xmlNode ** result_cib, xmlNode ** answer)
{
    int rc = pcmk_ok;

    *answer = NULL;

    if(crm_element_value(req, F_CIB_SCHEMA_MAX)) {
        /* The originator of an upgrade request sends it to the DC, without
         * F_CIB_SCHEMA_MAX. If an upgrade is needed, the DC re-broadcasts the
         * request with F_CIB_SCHEMA_MAX, and each node performs the upgrade
         * (and notifies its local clients) here.
         */
        return cib_process_upgrade(
            op, options, section, req, input, existing_cib, result_cib, answer);

    } else {
        int new_version = 0;
        int current_version = 0;
        xmlNode *scratch = copy_xml(existing_cib);
        const char *host = crm_element_value(req, PCMK__XA_SRC);
        const char *value = crm_element_value(existing_cib,
                                              PCMK_XA_VALIDATE_WITH);
        const char *client_id = crm_element_value(req, PCMK__XA_CIB_CLIENTID);
        const char *call_opts = crm_element_value(req, PCMK__XA_CIB_CALLOPT);
        const char *call_id = crm_element_value(req, PCMK__XA_CIB_CALLID);

        crm_trace("Processing \"%s\" event", op);
        if (value != NULL) {
            current_version = get_schema_version(value);
        }

        rc = update_validation(&scratch, &new_version, 0, TRUE, TRUE);
        if (new_version > current_version) {
            xmlNode *up = create_xml_node(NULL, __func__);

            rc = pcmk_ok;
            crm_notice("Upgrade request from %s verified", host);

            crm_xml_add(up, PCMK__XA_T, T_CIB);
            crm_xml_add(up, PCMK__XA_CIB_OP, PCMK__CIB_REQUEST_UPGRADE);
            crm_xml_add(up, F_CIB_SCHEMA_MAX, get_schema_name(new_version));
            crm_xml_add(up, PCMK__XA_CIB_DELEGATED_FROM, host);
            crm_xml_add(up, PCMK__XA_CIB_CLIENTID, client_id);
            crm_xml_add(up, PCMK__XA_CIB_CALLOPT, call_opts);
            crm_xml_add(up, PCMK__XA_CIB_CALLID, call_id);

            if (cib_legacy_mode() && based_is_primary) {
                rc = cib_process_upgrade(
                    op, options, section, up, input, existing_cib, result_cib, answer);

            } else {
                send_cluster_message(NULL, crm_msg_cib, up, FALSE);
            }

            free_xml(up);

        } else if(rc == pcmk_ok) {
            rc = -pcmk_err_schema_unchanged;
        }

        if (rc != pcmk_ok) {
            // Notify originating peer so it can notify its local clients
            crm_node_t *origin = NULL;

            origin = pcmk__search_node_caches(0, host,
                                              pcmk__node_search_cluster);

            crm_info("Rejecting upgrade request from %s: %s "
                     CRM_XS " rc=%d peer=%s", host, pcmk_strerror(rc), rc,
                     (origin? origin->uname : "lost"));

            if (origin) {
                xmlNode *up = create_xml_node(NULL, __func__);

                crm_xml_add(up, PCMK__XA_T, T_CIB);
                crm_xml_add(up, PCMK__XA_CIB_OP, PCMK__CIB_REQUEST_UPGRADE);
                crm_xml_add(up, PCMK__XA_CIB_DELEGATED_FROM, host);
                crm_xml_add(up, PCMK__XA_CIB_ISREPLYTO, host);
                crm_xml_add(up, PCMK__XA_CIB_CLIENTID, client_id);
                crm_xml_add(up, PCMK__XA_CIB_CALLOPT, call_opts);
                crm_xml_add(up, PCMK__XA_CIB_CALLID, call_id);
                crm_xml_add_int(up, PCMK__XA_CIB_UPGRADE_RC, rc);
                if (send_cluster_message(origin, crm_msg_cib, up, TRUE)
                    == FALSE) {
                    crm_warn("Could not send CIB upgrade result to %s", host);
                }
                free_xml(up);
            }
        }
        free_xml(scratch);
    }
    return rc;
}

int
cib_process_sync_one(const char *op, int options, const char *section, xmlNode * req,
                     xmlNode * input, xmlNode * existing_cib, xmlNode ** result_cib,
                     xmlNode ** answer)
{
    return sync_our_cib(req, FALSE);
}

int
cib_server_process_diff(const char *op, int options, const char *section, xmlNode * req,
                        xmlNode * input, xmlNode * existing_cib, xmlNode ** result_cib,
                        xmlNode ** answer)
{
    int rc = pcmk_ok;

    if (sync_in_progress > MAX_DIFF_RETRY) {
        /* Don't ignore diffs forever; the last request may have been lost.
         * If the diff fails, we'll ask for another full resync.
         */
        sync_in_progress = 0;
    }

    // The primary instance should never ignore a diff
    if (sync_in_progress && !based_is_primary) {
        int diff_add_updates = 0;
        int diff_add_epoch = 0;
        int diff_add_admin_epoch = 0;

        int diff_del_updates = 0;
        int diff_del_epoch = 0;
        int diff_del_admin_epoch = 0;

        cib_diff_version_details(input,
                                 &diff_add_admin_epoch, &diff_add_epoch, &diff_add_updates,
                                 &diff_del_admin_epoch, &diff_del_epoch, &diff_del_updates);

        sync_in_progress++;
        crm_notice("Not applying diff %d.%d.%d -> %d.%d.%d (sync in progress)",
                   diff_del_admin_epoch, diff_del_epoch, diff_del_updates,
                   diff_add_admin_epoch, diff_add_epoch, diff_add_updates);
        return -pcmk_err_diff_resync;
    }

    rc = cib_process_diff(op, options, section, req, input, existing_cib, result_cib, answer);
    crm_trace("result: %s (%d), %s", pcmk_strerror(rc), rc,
              (based_is_primary? "primary": "secondary"));

    if ((rc == -pcmk_err_diff_resync) && !based_is_primary) {
        free_xml(*result_cib);
        *result_cib = NULL;
        send_sync_request(NULL);

    } else if (rc == -pcmk_err_diff_resync) {
        rc = -pcmk_err_diff_failed;
        if (options & cib_force_diff) {
            crm_warn("Not requesting full refresh in R/W mode");
        }

    } else if ((rc != pcmk_ok) && !based_is_primary && cib_legacy_mode()) {
        crm_warn("Requesting full CIB refresh because update failed: %s"
                 CRM_XS " rc=%d", pcmk_strerror(rc), rc);

        pcmk__log_xml_patchset(LOG_INFO, input);
        free_xml(*result_cib);
        *result_cib = NULL;
        send_sync_request(NULL);
    }

    return rc;
}

int
cib_process_replace_svr(const char *op, int options, const char *section, xmlNode * req,
                        xmlNode * input, xmlNode * existing_cib, xmlNode ** result_cib,
                        xmlNode ** answer)
{
    int rc =
        cib_process_replace(op, options, section, req, input, existing_cib, result_cib, answer);

    if ((rc == pcmk_ok) && pcmk__xe_is(input, PCMK_XE_CIB)) {
        sync_in_progress = 0;
    }
    return rc;
}

// @COMPAT: Remove when PCMK__CIB_REQUEST_ABS_DELETE is removed
int
cib_process_delete_absolute(const char *op, int options, const char *section, xmlNode * req,
                            xmlNode * input, xmlNode * existing_cib, xmlNode ** result_cib,
                            xmlNode ** answer)
{
    return -EINVAL;
}

static xmlNode *
cib_msg_copy(xmlNode *msg)
{
    static const char *field_list[] = {
        PCMK__XA_T,
        PCMK__XA_CIB_CLIENTID,
        PCMK__XA_CIB_CALLOPT,
        PCMK__XA_CIB_CALLID,
        PCMK__XA_CIB_OP,
        PCMK__XA_CIB_ISREPLYTO,
        PCMK__XA_CIB_SECTION,
        PCMK__XA_CIB_HOST,
        PCMK__XA_CIB_RC,
        PCMK__XA_CIB_DELEGATED_FROM,
        PCMK__XA_CIB_OBJECT,
        PCMK__XA_CIB_OBJECT_TYPE,
        F_CIB_TIMEOUT,
        F_CIB_GLOBAL_UPDATE,
        PCMK__XA_CIB_CLIENTNAME,
        F_CIB_USER,
        F_CIB_NOTIFY_TYPE,
        F_CIB_NOTIFY_ACTIVATE
    };

    xmlNode *copy = create_xml_node(NULL, "copy");

    CRM_ASSERT(copy != NULL);

    for (int lpc = 0; lpc < PCMK__NELEM(field_list); lpc++) {
        const char *field = field_list[lpc];
        const char *value = crm_element_value(msg, field);

        if (value != NULL) {
            crm_xml_add(copy, field, value);
        }
    }

    return copy;
}

int
sync_our_cib(xmlNode * request, gboolean all)
{
    int result = pcmk_ok;
    char *digest = NULL;
    const char *host = crm_element_value(request, PCMK__XA_SRC);
    const char *op = crm_element_value(request, PCMK__XA_CIB_OP);
    crm_node_t *peer = NULL;
    xmlNode *replace_request = NULL;

    CRM_CHECK(the_cib != NULL, return -EINVAL);
    CRM_CHECK(all || (host != NULL), return -EINVAL);

    crm_debug("Syncing CIB to %s", all ? "all peers" : host);

    replace_request = cib_msg_copy(request);

    if (host != NULL) {
        crm_xml_add(replace_request, PCMK__XA_CIB_ISREPLYTO, host);
    }
    if (all) {
        xml_remove_prop(replace_request, PCMK__XA_CIB_HOST);
    }

    crm_xml_add(replace_request, PCMK__XA_CIB_OP, PCMK__CIB_REQUEST_REPLACE);

    // @TODO Keep for tracing, or drop?
    crm_xml_add(replace_request, PCMK__XA_ORIGINAL_CIB_OP, op);

    pcmk__xe_set_bool_attr(replace_request, F_CIB_GLOBAL_UPDATE, true);

    crm_xml_add(replace_request, PCMK_XA_CRM_FEATURE_SET, CRM_FEATURE_SET);
    digest = calculate_xml_versioned_digest(the_cib, FALSE, TRUE, CRM_FEATURE_SET);
    crm_xml_add(replace_request, PCMK__XA_DIGEST, digest);

    add_message_xml(replace_request, PCMK__XA_CIB_CALLDATA, the_cib);

    if (!all) {
        peer = pcmk__get_node(0, host, NULL, pcmk__node_search_cluster);
    }
    if (!send_cluster_message(peer, crm_msg_cib, replace_request, FALSE)) {
        result = -ENOTCONN;
    }
    free_xml(replace_request);
    free(digest);
    return result;
}

int
cib_process_commit_transaction(const char *op, int options, const char *section,
                               xmlNode *req, xmlNode *input,
                               xmlNode *existing_cib, xmlNode **result_cib,
                               xmlNode **answer)
{
    /* On success, our caller will activate *result_cib locally, trigger a
     * replace notification if appropriate, and sync *result_cib to all nodes.
     * On failure, our caller will free *result_cib.
     */
    int rc = pcmk_rc_ok;
    const char *client_id = crm_element_value(req, PCMK__XA_CIB_CLIENTID);
    const char *origin = crm_element_value(req, PCMK__XA_SRC);
    pcmk__client_t *client = pcmk__find_client_by_id(client_id);

    rc = based_commit_transaction(input, client, origin, result_cib);

    if (rc != pcmk_rc_ok) {
        char *source = based_transaction_source_str(client, origin);

        crm_err("Could not commit transaction for %s: %s",
                source, pcmk_rc_str(rc));
        free(source);
    }
    return pcmk_rc2legacy(rc);
}

int
cib_process_schemas(const char *op, int options, const char *section, xmlNode *req,
                    xmlNode *input, xmlNode *existing_cib, xmlNode **result_cib,
                    xmlNode **answer)
{
    xmlNode *data = NULL;
    const char *after_ver = NULL;
    GList *schemas = NULL;
    GList *already_included = NULL;

    *answer = create_xml_node(NULL, PCMK__XA_SCHEMAS);

    data = get_message_xml(req, PCMK__XA_CIB_CALLDATA);
    if (data == NULL) {
        crm_warn("No data specified in request");
        return -EPROTO;
    }

    after_ver = crm_element_value(data, PCMK_XA_VERSION);
    if (after_ver == NULL) {
        crm_warn("No version specified in request");
        return -EPROTO;
    }

    /* The client requested all schemas after the latest one we know about, which
     * means the client is fully up-to-date.  Return a properly formatted reply
     * with no schemas.
     */
    if (pcmk__str_eq(after_ver, xml_latest_schema(), pcmk__str_none)) {
        return pcmk_ok;
    }

    schemas = pcmk__schema_files_later_than(after_ver);

    for (GList *iter = schemas; iter != NULL; iter = iter->next) {
        pcmk__build_schema_xml_node(*answer, iter->data, &already_included);
    }

    g_list_free_full(schemas, free);
    g_list_free_full(already_included, free);
    return pcmk_ok;
}
