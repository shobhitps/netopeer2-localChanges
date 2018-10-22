/**
 * @file op_copyconfig.c
 * @author Radek Krejci <rkrejci@cesnet.cz>
 * @brief NETCONF <copy-config> operation implementation
 *
 * Copyright (c) 2016 CESNET, z.s.p.o.
 *
 * This source code is licensed under BSD 3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSD-3-Clause
 */

#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include <libyang/libyang.h>
#include <nc_server.h>
#include <sysrepo.h>

#include "common.h"
#include "operations.h"


static char *
find_last_slash(char *string)
{
    char in_quote = '\0';
    char *char_ptr;
    char curr_char;

    for (char_ptr = string + strlen(string); char_ptr != string; --char_ptr) {
        curr_char = *char_ptr;
        if (curr_char == '\'' || curr_char == '"') {
            if (in_quote == '\0')
                in_quote = curr_char;
            else if (in_quote == curr_char)
                in_quote = '\0';
        }
        else if (curr_char == '/') {
            if (in_quote == '\0')
                return char_ptr;
        }
    }

    return NULL;
}

int opcopy_get_subtree(sr_datastore_t source, struct lyd_node **config, struct nc_server_reply **ereply)
{
    struct ly_set *nodeset;
    struct nc_server_error *e;

    e = nc_err(NC_ERR_OP_FAILED, NC_ERR_TYPE_APP);
    nc_err_set_msg(e, "FIXME: FAILED", "en");    // FIXME
    *ereply = nc_server_reply_err(e);
    return -1;
}

static struct lyd_node *opcopy_import_any(struct lyd_node_anydata *any, struct nc_server_reply **ereply)
{
    struct nc_server_error *e;
    struct lyd_node *root = NULL;

    switch (any->value_type) {
    case LYD_ANYDATA_CONSTSTRING:
    case LYD_ANYDATA_STRING:
    case LYD_ANYDATA_SXML:
        root = lyd_parse_mem(np2srv.ly_ctx, any->value.str, LYD_XML, LYD_OPT_CONFIG | LYD_OPT_DESTRUCT | LYD_OPT_STRICT);
        break;
    case LYD_ANYDATA_DATATREE:
        root = any->value.tree;
        any->value.tree = NULL; /* "unlink" data tree from anydata to have full control */
        break;
    case LYD_ANYDATA_XML:
        root = lyd_parse_xml(np2srv.ly_ctx, &any->value.xml, LYD_OPT_CONFIG | LYD_OPT_DESTRUCT | LYD_OPT_STRICT);
        break;
    case LYD_ANYDATA_LYB:
        root = lyd_parse_mem(np2srv.ly_ctx, any->value.mem, LYD_LYB, LYD_OPT_CONFIG | LYD_OPT_DESTRUCT | LYD_OPT_STRICT);
        break;
    case LYD_ANYDATA_JSON:
    case LYD_ANYDATA_JSOND:
    case LYD_ANYDATA_SXMLD:
    case LYD_ANYDATA_LYBD:
        EINT;
        e = nc_err(NC_ERR_OP_FAILED, NC_ERR_TYPE_APP);
        nc_err_set_msg(e, np2log_lasterr(np2srv.ly_ctx), "en");
        *ereply = nc_server_reply_err(e);
    }
    if (!root) {
        if (ly_errno != LY_SUCCESS) {
            e = nc_err(NC_ERR_OP_FAILED, NC_ERR_TYPE_APP);
            nc_err_set_msg(e, np2log_lasterr(np2srv.ly_ctx), "en");
            *ereply = nc_server_reply_err(e);
        } else {
            /* TODO delete-config ??? */
        }
    }

    return root;
}

struct nc_server_reply *
op_copyconfig(struct lyd_node *rpc, struct nc_session *ncs)
{
    struct np2_sessions *sessions;
    sr_datastore_t target_ds = 0, source_ds = 0;
    int target_is_ds = 0, source_is_ds = 0;
    struct ly_set *nodeset;
    struct lyd_node *root = NULL, *iter, *next;
    const char *src_dsname, *tgt_dsname;
    char *str, path[1024];
    sr_val_t value;
    struct nc_server_error *e;
    struct nc_server_reply *ereply = NULL;
    int rc = SR_ERR_OK, path_index = 0, missing_keys = 0, lastkey = 0;
    unsigned int i;
    char quote;
#ifdef NP2SRV_ENABLED_URL_CAPABILITY
    char *target_url = 0;
    const char* urlval;
#endif

    /* get sysrepo connections for this session */
    sessions = (struct np2_sessions *)nc_session_get_data(ncs);

    if (np2srv_sr_check_exec_permission(sessions->srs, "/ietf-netconf:copy-config", &ereply)) {
        goto finish;
    }

    /* get know which datastore is being affected */
    nodeset = lyd_find_path(rpc, "/ietf-netconf:copy-config/target/*");
    tgt_dsname = nodeset->set.d[0]->schema->name;

    if (!strcmp(tgt_dsname, "running")) {
        target_ds = SR_DS_RUNNING;
        target_is_ds = 1;
    } else if (!strcmp(tgt_dsname, "startup")) {
        target_ds = SR_DS_STARTUP;
        target_is_ds = 1;
    } else if (!strcmp(tgt_dsname, "candidate")) {
        target_ds = SR_DS_CANDIDATE;
        target_is_ds = 1;
    } else if (!strcmp(tgt_dsname, "url")) {
#ifdef NP2SRV_ENABLED_URL_CAPABILITY
        urlval = ((struct lyd_node_leaf_list*)nodeset->set.d[0])->value_str;
        if (urlval) {
            target_url = (char *)malloc(strlen(urlval) + 1);
            strcpy(target_url, urlval);
            urlval = 0;
        } else {
            ly_set_free(nodeset);
            e = nc_err(NC_ERR_OP_FAILED, NC_ERR_TYPE_APP);
            nc_err_set_msg(e, "Missing target url", "en");
            ereply = nc_server_reply_err(e);
            goto finish;
        }
#else
        ly_set_free(nodeset);
        e = nc_err(NC_ERR_OP_FAILED, NC_ERR_TYPE_APP);
        nc_err_set_msg(e, "<url> source not supported", "en");
        ereply = nc_server_reply_err(e);
        goto finish;
#endif
    }

    ly_set_free(nodeset);

    /* get source */
    nodeset = lyd_find_path(rpc, "/ietf-netconf:copy-config/source/*");
    src_dsname = nodeset->set.d[0]->schema->name;

    if (!strcmp(src_dsname, "running")) {
        source_ds = SR_DS_RUNNING;
        source_is_ds = 1;
    } else if (!strcmp(src_dsname, "startup")) {
        source_ds = SR_DS_STARTUP;
        source_is_ds = 1;
    } else if (!strcmp(src_dsname, "candidate")) {
        source_ds = SR_DS_CANDIDATE;
        source_is_ds = 1;
    } else if (!strcmp(src_dsname, "config")) {
        root = opcopy_import_any((struct lyd_node_anydata *)nodeset->set.d[0], &ereply);
        if (!root) {
            ly_set_free(nodeset);
            goto finish;
        }
    } else if (!strcmp(src_dsname, "url")) {
#ifdef NP2SRV_ENABLED_URL_CAPABILITY
        urlval = ((struct lyd_node_leaf_list*)nodeset->set.d[0])->value_str;
        if (urlval) {
            if (op_url_import(urlval, &root, &ereply)) {
                ly_set_free(nodeset);
                goto finish;
            }
        } else {
            ly_set_free(nodeset);
            e = nc_err(NC_ERR_OP_FAILED, NC_ERR_TYPE_APP);
            nc_err_set_msg(e, "Missing source url", "en");
            ereply = nc_server_reply_err(e);
            goto finish;
        }
#else
        ly_set_free(nodeset);
        e = nc_err(NC_ERR_OP_FAILED, NC_ERR_TYPE_APP);
        nc_err_set_msg(e, "<url> source not supported", "en");
        ereply = nc_server_reply_err(e);
        goto finish;
#endif
    }

    ly_set_free(nodeset);

    if (source_is_ds && target_is_ds)
    {
        /* datastore-to-datastore copy */
        rc = np2srv_sr_copy_config(sessions->srs, NULL, source_ds, target_ds, &ereply);
        /* commit is done implicitely by sr_copy_config() */
        goto finish;
    }

    if (target_is_ds)
    {
        /* copy url/config (at root) to datastore */
        if (sessions->ds != target_ds) {
            /* update sysrepo session */
            np2srv_sr_session_switch_ds(sessions->srs, target_ds, NULL);
            sessions->ds = target_ds;
        }
        if (sessions->ds != SR_DS_CANDIDATE) {
            /* update data from sysrepo */
            if (np2srv_sr_session_refresh(sessions->srs, &ereply)) {
                goto finish;
            }
        }

        /* perform operation */

        /* remove all the data from the models mentioned in the source config... */
        nodeset = ly_set_new();
        LY_TREE_FOR(root, iter) {
            if (iter->dflt) {
                continue;
            }
            ly_set_add(nodeset, lyd_node_module(iter), 0);
        }
        for (i = 0; i < nodeset->number; i++) {
            snprintf(path, 1024, "/%s:*", ((struct lys_module *)nodeset->set.g[i])->name);
            np2srv_sr_delete_item(sessions->srs, path, 0, NULL);
        }
        ly_set_free(nodeset);

        /* and copy source config's content into sysrepo */
        LY_TREE_DFS_BEGIN(root, next, iter) {
            /* maintain path */
            if (!missing_keys) {
                if (!iter->parent || lyd_node_module(iter) != lyd_node_module(iter->parent)) {
                    /* with prefix */
                    path_index += sprintf(&path[path_index], "/%s:%s", lyd_node_module(iter)->name, iter->schema->name);
                } else {
                    /* without prefix */
                    path_index += sprintf(&path[path_index], "/%s", iter->schema->name);
                }

                /* erase value */
                memset(&value, 0, sizeof value);
            }

            /* specific handling for different types of nodes */
            lastkey = 0;

            /* skip default nodes */
            if (iter->dflt) {
                goto dfs_continue;
            }

            switch(iter->schema->nodetype) {
            case LYS_CONTAINER:
                if (!((struct lys_node_container *)iter->schema)->presence) {
                    /* do nothing */
                    goto dfs_continue;
                }
                /* set value for sysrepo */
                op_set_srval(iter, NULL, 0, &value, &str);

                break;
            case LYS_LEAF:
                if (missing_keys) {
                    /* still processing list keys */
                    missing_keys--;
                    /* add key predicate into the list's path */
                    if (strchr(((struct lyd_node_leaf_list *)iter)->value_str, '\'')) {
                        quote = '"';
                    } else {
                        quote = '\'';
                    }
                    path_index += sprintf(&path[path_index], "[%s=%c%s%c]", iter->schema->name,
                                          quote, ((struct lyd_node_leaf_list *)iter)->value_str, quote);
                    if (!missing_keys) {
                        /* the last key, create the list instance */
                        lastkey = 1;
                        break;
                    }
                    goto dfs_continue;
                }
                /* regular leaf */

                /* set value for sysrepo */
                op_set_srval(iter, NULL, 0, &value, &str);

                break;
            case LYS_LEAFLIST:
                /* set value for sysrepo */
                op_set_srval(iter, NULL, 0, &value, &str);

                break;
            case LYS_LIST:
                /* set value for sysrepo, it will be used as soon as all the keys are processed */
                op_set_srval(iter, NULL, 0, &value, &str);

                /* the creation must be finished later when we get know keys */
                missing_keys = ((struct lys_node_list *)iter->schema)->keys_size;
                goto dfs_continue;
            case LYS_ANYXML:
                /* set value for sysrepo */
                op_set_srval(iter, NULL, 0, &value, &str);

                break;
            default:
                ERR("%s: Invalid node to process", __func__);
                e = nc_err(NC_ERR_OP_FAILED, NC_ERR_TYPE_APP);
                nc_err_set_msg(e, np2log_lasterr(np2srv.ly_ctx), "en");
                ereply = nc_server_reply_err(e);
                goto finish;
            }

            /* create the iter in sysrepo */
            rc = np2srv_sr_set_item(sessions->srs, path, &value, 0, &ereply);
            if (str) {
                free(str);
                str = NULL;
            }
            if (rc) {
                goto finish;
            }

dfs_continue:
            /* modified LY_TREE_DFS_END,
             * select iter for the next run - children first */
            if (iter->schema->nodetype & (LYS_LEAF | LYS_LEAFLIST | LYS_ANYXML)) {
                next = NULL;
            } else {
                next = iter->child;
            }

            if (!next) {
                /* no children, try siblings */
                next = iter->next;

                /* maintain "stack" variables */
                if (!missing_keys && !lastkey) {
                    str = find_last_slash(path);
                    if (str) {
                        *str = '\0';
                        path_index = str - path;
                    } else {
                        path[0] = '\0';
                        path_index = 0;
                    }
                }
            }

            while (!next) {
                /* parent is already processed, go to its sibling */
                iter = iter->parent;
                if (iter == root->parent) {
                    /* we are done, no next element to process */
                    break;
                }
                next = iter->next;

                /* maintain "stack" variables */
                if (!missing_keys) {
                    str = find_last_slash(path);
                    if (str) {
                        *str = '\0';
                        path_index = str - path;
                    } else {
                        path[0] = '\0';
                        path_index = 0;
                    }
                }
            }
        }

        /* commit the result */
        rc = np2srv_sr_commit(sessions->srs, &ereply);

        if (rc) {
            goto finish;
        }

        if (sessions->ds == SR_DS_CANDIDATE) {
            if (np2srv_sr_validate(sessions->srs, &ereply)) {
                /* content is not valid or error, rollback */
                np2srv_sr_discard_changes(sessions->srs, NULL);
                goto finish;
            }
            /* mark candidate as modified */
            sessions->flags |= NP2S_CAND_CHANGED;
        }

        ereply = nc_server_reply_ok();

    } else {
#ifdef NP2SRV_ENABLED_URL_CAPABILITY
        /* copy datastore to url */
        e = nc_err(NC_ERR_OP_FAILED, NC_ERR_TYPE_APP);
        nc_err_set_msg(e, "fixme", "en");
        ereply = nc_server_reply_err(e);
#endif
    }

finish:
    lyd_free_withsiblings(root);
    return ereply;
}
