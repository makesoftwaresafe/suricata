/* Copyright (C) 2007-2020 Open Information Security Foundation
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
 * \author Victor Julien <victor@inliniac.net>
 *
 * Implements the config keyword
 */

#include "suricata-common.h"
#include "threads.h"
#include "decode.h"

#include "detect.h"
#include "detect-parse.h"

#include "detect-engine.h"
#include "detect-engine-mpm.h"
#include "detect-engine-state.h"

#include "feature.h"

#include "flow.h"
#include "flow-var.h"
#include "flow-util.h"

#include "util-debug.h"
#include "util-spm-bm.h"
#include "util-unittest.h"
#include "util-unittest-helper.h"

#include "app-layer.h"
#include "app-layer-parser.h"
#include "app-layer-htp.h"

#include "stream-tcp.h"

#include "detect-config.h"

#include "output.h"

/**
 * \brief Regex for parsing our flow options
 */
#define PARSE_REGEX  "^\\s*([A-z_]+)\\s*\\s*([A-z_]+)\\s*(?:,\\s*([A-z_]+)\\s+([A-z_]+))?\\s*(?:,\\s*([A-z_]+)\\s+([A-z_]+))?$"

static DetectParseRegex parse_regex;

static int DetectConfigPostMatch (DetectEngineThreadCtx *det_ctx, Packet *p,
        const Signature *s, const SigMatchCtx *ctx);
static int DetectConfigSetup (DetectEngineCtx *, Signature *, const char *);
static void DetectConfigFree(DetectEngineCtx *, void *);
#ifdef UNITTESTS
static void DetectConfigRegisterTests(void);
#endif

/**
 * \brief Registration function for keyword: filestore
 */
void DetectConfigRegister(void)
{
    sigmatch_table[DETECT_CONFIG].name = "config";
    sigmatch_table[DETECT_CONFIG].Match = DetectConfigPostMatch;
    sigmatch_table[DETECT_CONFIG].Setup = DetectConfigSetup;
    sigmatch_table[DETECT_CONFIG].Free  = DetectConfigFree;
#ifdef UNITTESTS
    sigmatch_table[DETECT_CONFIG].RegisterTests = DetectConfigRegisterTests;
#endif
    DetectSetupParseRegexes(PARSE_REGEX, &parse_regex);
}

static void ConfigApplyTx(Flow *f,
        const uint64_t tx_id, const DetectConfigData *config)
{
    if (f->alstate == NULL) {
        return;
    }
    void *tx = AppLayerParserGetTx(f->proto, f->alproto, f->alstate, tx_id);
    if (tx) {
        AppLayerTxData *txd = AppLayerParserGetTxData(f->proto, f->alproto, tx);
        SCLogDebug("tx %p txd %p: log_flags %x", tx, txd, txd->config.log_flags);
        txd->config.log_flags |= BIT_U8(config->type);

        const bool unidir =
                (txd->flags & (APP_LAYER_TX_SKIP_INSPECT_TS | APP_LAYER_TX_SKIP_INSPECT_TC)) != 0;
        if (unidir) {
            SCLogDebug("handle unidir tx");
            AppLayerTxConfig req;
            memset(&req, 0, sizeof(req));
            req.log_flags = BIT_U8(config->type);
            AppLayerParserApplyTxConfig(
                    f->proto, f->alproto, f->alstate, tx, CONFIG_ACTION_SET, req);
        }
    } else {
        SCLogDebug("no tx");
    }
}

/**
 *  \brief apply the post match filestore with options
 */
static int ConfigApply(DetectEngineThreadCtx *det_ctx,
        Packet *p, const DetectConfigData *config)
{
    bool this_tx = false;
    bool this_flow = false;

    switch (config->scope) {
        case CONFIG_SCOPE_TX:
            this_tx = true;
            break;
        case CONFIG_SCOPE_FLOW:
            this_flow = true;
            break;
    }

    if (this_tx) {
        SCLogDebug("tx logic here: tx_id %"PRIu64, det_ctx->tx_id);
        ConfigApplyTx(p->flow, det_ctx->tx_id, config);
    } else if (this_flow) {
        SCLogDebug("flow logic here");
    }

    SCReturnInt(0);
}

static int DetectConfigPostMatch(DetectEngineThreadCtx *det_ctx,
        Packet *p, const Signature *s, const SigMatchCtx *ctx)
{
    SCEnter();
    const DetectConfigData *config = (const DetectConfigData *)ctx;
    ConfigApply(det_ctx, p, config);
    SCReturnInt(1);
}

/**
 * \brief this function is used to parse filestore options
 * \brief into the current signature
 *
 * \param de_ctx pointer to the Detection Engine Context
 * \param s pointer to the Current Signature
 * \param str pointer to the user provided "filestore" option
 *
 * \retval 0 on Success
 * \retval -1 on Failure
 */
static int DetectConfigSetup (DetectEngineCtx *de_ctx, Signature *s, const char *str)
{
    SCEnter();

    DetectConfigData *fd = NULL;
    int res = 0;
    size_t pcre2len;
#if 0
    /* filestore and bypass keywords can't work together */
    if (s->flags & SIG_FLAG_BYPASS) {
        SCLogError(
                   "filestore can't work with bypass keyword");
        return -1;
    }
#endif
    pcre2_match_data *match = NULL;

    if (str == NULL || strlen(str) == 0) {
        SCLogError("config keywords need arguments");
        goto error;
    }
    char subsys[32];
    char state[32];
    char type[32];
    char typeval[32];
    char scope[32];
    char scopeval[32];
    SCLogDebug("str %s", str);

    int ret = DetectParsePcreExec(&parse_regex, &match, str, 0, 0);
    if (ret != 7) {
        SCLogError("config is rather picky at this time");
        goto error;
    }
    pcre2len = sizeof(subsys);
    res = pcre2_substring_copy_bynumber(match, 1, (PCRE2_UCHAR8 *)subsys, &pcre2len);
    if (res < 0) {
        SCLogError("pcre2_substring_copy_bynumber failed");
        goto error;
    }

    if (strcmp(subsys, "logging") != 0) {
        SCLogError("only 'logging' supported at this time");
        goto error;
    }
    SCLogDebug("subsys %s", subsys);

    pcre2len = sizeof(state);
    res = pcre2_substring_copy_bynumber(match, 2, (PCRE2_UCHAR8 *)state, &pcre2len);
    if (res < 0) {
        SCLogError("pcre2_substring_copy_bynumber failed");
        goto error;
    }

    if (strcmp(state, "disable") != 0) {
        SCLogError("only 'disable' supported at this time");
        goto error;
    }
    SCLogDebug("state %s", state);

    pcre2len = sizeof(type);
    res = pcre2_substring_copy_bynumber(match, 3, (PCRE2_UCHAR8 *)type, &pcre2len);
    if (res < 0) {
        SCLogError("pcre2_substring_copy_bynumber failed");
        goto error;
    }

    if (strcmp(type, "type") != 0) {
        SCLogError("only 'type' supported at this time");
        goto error;
    }
    SCLogDebug("type %s", type);

    pcre2len = sizeof(typeval);
    res = pcre2_substring_copy_bynumber(match, 4, (PCRE2_UCHAR8 *)typeval, &pcre2len);
    if (res < 0) {
        SCLogError("pcre2_substring_copy_bynumber failed");
        goto error;
    }

    if (!(strcmp(typeval, "tx") == 0 ||strcmp(typeval, "flow") == 0)) {
        SCLogError("only 'tx' and 'flow' supported at this time");
        goto error;
    }
    SCLogDebug("typeval %s", typeval);

    pcre2len = sizeof(scope);
    res = pcre2_substring_copy_bynumber(match, 5, (PCRE2_UCHAR8 *)scope, &pcre2len);
    if (res < 0) {
        SCLogError("pcre2_substring_copy_bynumber failed");
        goto error;
    }

    if (strcmp(scope, "scope") != 0) {
        SCLogError("only 'scope' supported at this time");
        goto error;
    }
    SCLogDebug("scope %s", scope);

    pcre2len = sizeof(scopeval);
    res = pcre2_substring_copy_bynumber(match, 6, (PCRE2_UCHAR8 *)scopeval, &pcre2len);
    if (res < 0) {
        SCLogError("pcre2_substring_copy_bynumber failed");
        goto error;
    }

    if (!(strcmp(scopeval, "tx") == 0 ||strcmp(scopeval, "flow") == 0)) {
        SCLogError("only 'tx' and 'flow' supported at this time");
        goto error;
    }
    SCLogDebug("scopeval %s", scopeval);

    fd = SCCalloc(1, sizeof(DetectConfigData));
    if (unlikely(fd == NULL))
        goto error;

    if (strcmp(typeval, "tx") == 0) {
        fd->type = CONFIG_TYPE_TX;
    }
    if (strcmp(scopeval, "tx") == 0) {
        fd->scope = CONFIG_SCOPE_TX;
    }

    if (fd->scope == CONFIG_SCOPE_TX) {
        s->flags |= SIG_FLAG_APPLAYER;
    }

    if (SCSigMatchAppendSMToList(
                de_ctx, s, DETECT_CONFIG, (SigMatchCtx *)fd, DETECT_SM_LIST_POSTMATCH) == NULL) {
        goto error;
    }

    pcre2_match_data_free(match);
    return 0;

error:
    if (match) {
        pcre2_match_data_free(match);
    }
    return -1;
}

static void DetectConfigFree(DetectEngineCtx *de_ctx, void *ptr)
{
    if (ptr != NULL) {
        SCFree(ptr);
    }
}

#ifdef UNITTESTS
/*
 * The purpose of this test is to confirm that
 * filestore and bypass keywords can't
 * can't work together
 */
static int DetectConfigTest01(void)
{
    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    FAIL_IF(de_ctx == NULL);
    de_ctx->flags |= DE_QUIET;
    Signature *s = DetectEngineAppendSig(de_ctx,
            "config dns any any -> any any ("
            "dns.query; content:\"common.domain.com\"; "
            "config:logging disable, type tx, scope tx; "
            "sid:1;)");
    FAIL_IF_NULL(s);
    DetectEngineCtxFree(de_ctx);
    PASS;
}

void DetectConfigRegisterTests(void)
{
    UtRegisterTest("DetectConfigTest01", DetectConfigTest01);
}
#endif /* UNITTESTS */
