/*
 * Cache stats functions.
 *
 * Copyright (C) [Jiang Wenyuan](https://github.com/jiangwenyuan), < koubunen AT gmail DOT com >
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <types/global.h>
#include <types/cache.h>

#include <proto/stream_interface.h>
#include <proto/proxy.h>
#include <proto/cache.h>

void cache_stats_update_used_mem(int i) {
    nuster_shctx_lock(global.cache.stats);
    global.cache.stats->used_mem += i;
    nuster_shctx_unlock(global.cache.stats);
}

int cache_stats_init() {
    global.cache.stats = nuster_memory_alloc(global.cache.memory, sizeof(struct cache_stats));
    if(!global.cache.stats) {
        return 0;
    }
    global.cache.stats->used_mem = 0;
    global.cache.stats->requests = 0;
    global.cache.stats->hits     = 0;
    return 1;
}

int cache_stats_full() {
    int i;
    nuster_shctx_lock(global.cache.stats);
    i =  global.cache.data_size <= global.cache.stats->used_mem;
    nuster_shctx_unlock(global.cache.stats);
    return i;
}

/*
 * return 1 if the request is done, otherwise 0
 */
int cache_stats(struct stream *s, struct channel *req, struct proxy *px) {
    struct stream_interface *si = &s->si[1];
    struct http_txn *txn        = s->txn;
    struct http_msg *msg        = &txn->req;
    struct appctx *appctx       = NULL;

    if(global.cache.status != CACHE_STATUS_ON) {
        return 0;
    }

    /* GET stats uri */
    if(txn->meth == HTTP_METH_GET && cache_check_uri(msg)) {
        s->target = &cache_stats_applet.obj_type;
        if(unlikely(!stream_int_register_handler(si, objt_applet(s->target)))) {
            return 1;
        } else {
            appctx      = si_appctx(si);
            appctx->st0 = NUSTER_CACHE_STATS_HEAD;

            req->analysers &= (AN_REQ_HTTP_BODY | AN_REQ_FLT_HTTP_HDRS | AN_REQ_FLT_END);
            req->analysers &= ~AN_REQ_FLT_XFER_DATA;
            req->analysers |= AN_REQ_HTTP_XFER_BODY;
        }
    }
    return 0;
}

int cache_stats_head(struct appctx *appctx, struct stream *s, struct stream_interface *si, struct channel *res) {
    chunk_printf(&trash,
            "HTTP/1.1 200 OK\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: close\r\n"
            "Content-Type: text/plain\r\n"
            "\r\n");

    chunk_appendf(&trash, "**GLOBAL**\n");
    chunk_appendf(&trash, "global.cache.data.size: %"PRIu64"\n", global.cache.data_size);
    chunk_appendf(&trash, "global.cache.dict.size: %"PRIu64"\n", global.cache.dict_size);
    chunk_appendf(&trash, "global.cache.uri: %s\n", global.cache.uri);
    chunk_appendf(&trash, "global.cache.purge_method: %.*s\n", (int)strlen(global.cache.purge_method) - 1, global.cache.purge_method);

    s->txn->status     = 200;

    if (bi_putchk(res, &trash) == -1) {
        si_applet_cant_put(si);
        return 0;
    }

    return 1;
}

int cache_stats_data(struct appctx *appctx, struct stream *s, struct stream_interface *si, struct channel *res) {
    struct proxy *p;

    p = proxy;
    while(p) {
        struct cache_rule *rule = NULL;

        if(p->cap & PR_CAP_BE) {
            chunk_appendf(&trash, "\n**PROXY %s**\n", p->id);

            if (!LIST_ISEMPTY(&p->cache_rules)) {
                list_for_each_entry(rule, &p->cache_rules, list) {
                    chunk_appendf(&trash, "%s.rule.%s: state=%s ttl=%"PRIu32"\n",
                            p->id, rule->name, *rule->state == CACHE_RULE_ENABLED ? "on" : "off", *rule->ttl);
                }
            }
        }
        p = p->next;
    }

    if (bi_putchk(res, &trash) == -1) {
        si_applet_cant_put(si);
        return 0;
    }

    return 1;
}

static void cache_stats_handler(struct appctx *appctx) {
    struct stream_interface *si = appctx->owner;
    struct channel *res         = si_ic(si);
    struct stream *s            = si_strm(si);

    if(appctx->st0 == NUSTER_CACHE_STATS_HEAD) {
        if(cache_stats_head(appctx, s, si, res)) {
            appctx->st0 = NUSTER_CACHE_STATS_DATA;
        }
    }
    if(appctx->st0 == NUSTER_CACHE_STATS_DATA) {
        if(cache_stats_data(appctx, s, si, res)) {
            appctx->st0 = NUSTER_CACHE_STATS_DONE;
        }
    }
    if(appctx->st0 == NUSTER_CACHE_STATS_DONE) {
        bo_skip(si_oc(si), si_ob(si)->o);
        si_shutr(si);
        res->flags |= CF_READ_NULL;
    }

}

struct applet cache_stats_applet = {
    .obj_type = OBJ_TYPE_APPLET,
    .name = "<CACHE-STATS>",
    .fct = cache_stats_handler,
    .release = NULL,
};
