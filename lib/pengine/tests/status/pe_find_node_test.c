/*
 * Copyright 2022 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU Lesser General Public License
 * version 2.1 or later (LGPLv2.1+) WITHOUT ANY WARRANTY.
 */

#include <crm_internal.h>

#include <crm/common/unittest_internal.h>
#include <crm/pengine/internal.h>

static void
empty_list(void **state) {
    assert_null(pe_find_node(NULL, NULL));
    assert_null(pe_find_node(NULL, "cluster1"));
}

static void
non_null_list(void **state) {
    GList *nodes = NULL;

    pe_node_t *a = calloc(1, sizeof(pe_node_t));
    pe_node_t *b = calloc(1, sizeof(pe_node_t));

    a->details = calloc(1, sizeof(struct pe_node_shared_s));
    a->details->uname = "cluster1";
    b->details = calloc(1, sizeof(struct pe_node_shared_s));
    b->details->uname = "cluster2";

    nodes = g_list_append(nodes, a);
    nodes = g_list_append(nodes, b);

    assert_ptr_equal(a, pe_find_node(nodes, "cluster1"));
    assert_null(pe_find_node(nodes, "cluster10"));
    assert_null(pe_find_node(nodes, "nodecluster1"));
    assert_ptr_equal(b, pe_find_node(nodes, "CLUSTER2"));
    assert_null(pe_find_node(nodes, "xyz"));

    free(a->details);
    free(a);
    free(b->details);
    free(b);
    g_list_free(nodes);
}

int main(int argc, char **argv) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(empty_list),
        cmocka_unit_test(non_null_list),
    };

    cmocka_set_message_output(CM_OUTPUT_TAP);
    return cmocka_run_group_tests(tests, NULL, NULL);
}