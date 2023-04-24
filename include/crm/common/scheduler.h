/*
 * Copyright 2004-2023 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU Lesser General Public License
 * version 2.1 or later (LGPLv2.1+) WITHOUT ANY WARRANTY.
 */

#ifndef PCMK__CRM_COMMON_SCHEDULER__H
#  define PCMK__CRM_COMMON_SCHEDULER__H

#include <crm/common/actions.h>
#include <crm/common/nodes.h>
#include <crm/common/resources.h>
#include <crm/common/roles.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * \file
 * \brief Scheduler API
 * \ingroup core
 */

//! Possible responses to loss of quorum
enum pe_quorum_policy {
    pcmk_no_quorum_freeze,  //<! Do not recover resources from outside partition

    no_quorum_freeze    = pcmk_no_quorum_freeze,
    no_quorum_stop      = 1,
    no_quorum_ignore    = 2,
    no_quorum_suicide   = 3,
    no_quorum_demote    = 4,
};

#ifdef __cplusplus
}
#endif

#endif // PCMK__CRM_COMMON_SCHEDULER__H
