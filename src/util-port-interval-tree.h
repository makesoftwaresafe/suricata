/* Copyright (C) 2024 Open Information Security Foundation
 *
 * You can copy, redistribute or modify this Program under the terms of
 * the GNU General Public License version 2 as published by the Free
 * Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

/**
 * \file
 *
 * \author Shivani Bhardwaj <shivani@oisf.net>
 */

#ifndef SURICATA_UTIL_PORT_INTERVAL_TREE_H
#define SURICATA_UTIL_PORT_INTERVAL_TREE_H

#include "detect-engine-port.h"

typedef struct SCPortIntervalTree_ {
    struct PI tree;
    SCPortIntervalNode *head;
} SCPortIntervalTree;

SCPortIntervalTree *SCPortIntervalTreeInit(void);
void SCPortIntervalTreeFree(DetectEngineCtx *, SCPortIntervalTree *);
int SCPortIntervalInsert(DetectEngineCtx *, SCPortIntervalTree *, const DetectPort *);
void SCPortIntervalFindOverlappingRanges(
        DetectEngineCtx *, const uint16_t, const uint16_t, const struct PI *, DetectPort **);

#endif /* SURICATA_UTIL_PORT_INTERVAL_TREE_H */
