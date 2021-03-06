/*
 * Cache engine functions.
 *
 * Copyright (C) 2017, [Jiang Wenyuan](https://github.com/jiangwenyuan), < koubunen AT gmail DOT com >
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include <types/applet.h>
#include <types/cli.h>
#include <types/global.h>
#include <types/cache.h>

#include <proto/filters.h>
#include <proto/log.h>
#include <proto/proto_http.h>
#include <proto/sample.h>
#include <proto/raw_sock.h>
#include <proto/stream_interface.h>
#include <proto/acl.h>
#include <proto/cache.h>

#include <import/xxhash.h>

#ifdef USE_OPENSSL
#include <proto/ssl_sock.h>
#include <types/ssl_sock.h>
#endif

/*
 * Cache the keys which calculated in request for response use
 */
struct cache_rule_stash *cache_stash_rule(struct cache_ctx *ctx,
        struct cache_rule *rule, char *key, uint64_t hash) {

    struct cache_rule_stash *stash = pool_alloc2(global.cache.pool.stash);

    if(stash) {
        stash->rule = rule;
        stash->key  = key;
        stash->hash = hash;
        if(ctx->stash) {
            stash->next = ctx->stash;
        } else {
            stash->next = NULL;
        }
        ctx->stash = stash;
    }
    return stash;
}

int cache_test_rule(struct cache_rule *rule, struct stream *s, int res) {
    int ret;

    /* no acl defined */
    if(!rule->cond) {
        return 1;
    }

    if(res) {
        ret = acl_exec_cond(rule->cond, s->be, s->sess, s, SMP_OPT_DIR_RES|SMP_OPT_FINAL);
    } else {
        ret = acl_exec_cond(rule->cond, s->be, s->sess, s, SMP_OPT_DIR_REQ|SMP_OPT_FINAL);
    }
    ret = acl_pass(ret);
    if(rule->cond->pol == ACL_COND_UNLESS) {
        ret = !ret;
    }

    if(ret) {
        return 1;
    }
    return 0;
}

static char *_cache_key_append(char *dst, int *dst_len, int *dst_size,
        char *src, int src_len) {

    int left     = *dst_size - *dst_len;
    int need     = src_len + 1;
    int old_size = *dst_size;

    if(left < need) {
        *dst_size += ((need - left) / CACHE_DEFAULT_KEY_SIZE + 1)  * CACHE_DEFAULT_KEY_SIZE;
    }

    if(old_size != *dst_size) {
        char *new_dst = realloc(dst, *dst_size);
        if(!new_dst) {
            free(dst);
            return NULL;
        }
        dst = new_dst;
    }

    memcpy(dst + *dst_len, src, src_len);
    *dst_len += src_len;
    dst[*dst_len] = '\0';
    return dst;
}

static int _cache_find_param_value_by_name(char *query_beg, char *query_end,
        char *name, char **value, int *value_len) {

    char equal   = '=';
    char and     = '&';
    char *ptr    = query_beg;
    int name_len = strlen(name);

    while(ptr + name_len + 1 < query_end) {
        if(!memcmp(ptr, name, name_len) && *(ptr + name_len) == equal) {
            if(ptr == query_beg || *(ptr - 1) == and) {
                ptr    = ptr + name_len + 1;
                *value = ptr;
                while(ptr < query_end && *ptr != and) {
                    (*value_len)++;
                    ptr++;
                }
                return 1;
            }
        }
        ptr++;
    }
    return 0;
}

/*
 * create a new cache_data and insert it to cache->data list
 */
struct cache_data *cache_data_new() {

    struct cache_data *data = pool_alloc2(global.cache.pool.data);

    if(data) {
        data->clients  = 0;
        data->invalid  = 0;
        data->element  = NULL;

        if(cache->data_head == NULL) {
            cache->data_head = data;
            cache->data_tail = data;
            data->next       = data;
        } else {
            if(cache->data_head == cache->data_tail) {
                cache->data_head->next = data;
                data->next             = cache->data_head;
                cache->data_tail       = data;
            } else {
                data->next             = cache->data_head;
                cache->data_tail->next = data;
                cache->data_tail       = data;
            }
        }
    }
    return data;
}

/*
 * Append partial http response data
 */
static struct cache_element *cache_data_append(struct cache_element *tail,
        struct http_msg *msg, long msg_len) {

    struct cache_element *element = pool_alloc2(global.cache.pool.element);

    if(element) {
        char *data = msg->chn->buf->data;
        char *p    = msg->chn->buf->p;
        int size   = msg->chn->buf->size;

        element->msg = pool_alloc2(global.cache.pool.chunk);

        if(p - data + msg_len > size) {
            int right = data + size - p;
            int left  = msg_len - right;
            memcpy(element->msg, p, right);
            memcpy(element->msg + right, data, left);
        } else {
            memcpy(element->msg, p, msg_len);
        }
        element->msg_len = msg_len;
        element->next    = NULL;
        if(tail == NULL) {
            tail = element;
        } else {
            tail->next = element;
        }
        global.cache.stats->used_mem += msg_len;
    }
    return element;
}


static int _cache_data_invalid(struct cache_data *data) {
    if(data->invalid) {
        if(!data->clients) {
            return 1;
        }
    }
    return 0;
}

/*
 * free invalid cache_data
 */
static void _cache_data_cleanup() {
    struct cache_data *data = NULL;

    if(cache->data_head) {
        if(cache->data_head == cache->data_tail) {
            if(_cache_data_invalid(cache->data_head)) {
                data             = cache->data_head;
                cache->data_head = NULL;
                cache->data_tail = NULL;
            }
        } else {
            if(_cache_data_invalid(cache->data_head)) {
                data                   = cache->data_head;
                cache->data_tail->next = cache->data_head->next;
                cache->data_head       = cache->data_head->next;
            } else {
                cache->data_tail       = cache->data_head;
                cache->data_head       = cache->data_head->next;
            }
        }
    }

    if(data) {
        struct cache_element *element = data->element;
        while(element) {
            struct cache_element *tmp = element;
            element                   = element->next;

            global.cache.stats->used_mem -= tmp->msg_len;
            pool_free2(global.cache.pool.chunk, tmp->msg);
            pool_free2(global.cache.pool.element, tmp);
        }
        pool_free2(global.cache.pool.data, data);
    }
}

void cache_housekeeping() {
    if(global.cache.status == CACHE_STATUS_ON) {
        cache_dict_rehash();
        cache_dict_cleanup();
        _cache_data_cleanup();
    }
}

void cache_init() {
    if(global.cache.status == CACHE_STATUS_ON) {
        global.cache.pool.stash   = create_pool("cp.stash", sizeof(struct cache_rule_stash), MEM_F_SHARED);
        global.cache.pool.ctx     = create_pool("cp.ctx", sizeof(struct cache_ctx), MEM_F_SHARED);
        global.cache.pool.data    = create_pool("cp.data", sizeof(struct cache_data), MEM_F_SHARED);
        global.cache.pool.element = create_pool("cp.element", sizeof(struct cache_element), MEM_F_SHARED);
        global.cache.pool.chunk   = create_pool("cp.chunk", global.tune.bufsize, MEM_F_SHARED);
        global.cache.pool.entry   = create_pool("cp.entry", sizeof(struct cache_entry), MEM_F_SHARED);
        global.cache.stats        = malloc(sizeof(struct cache_stats));
        if(!global.cache.stats) {
            goto err;
        }
        global.cache.stats->used_mem = 0;
        global.cache.stats->requests = 0;
        global.cache.stats->hits     = 0;

        cache = malloc(sizeof(struct cache));
        if(!cache) {
            goto err;
        }
        cache->dict[0].entry = NULL;
        cache->dict[0].used  = 0;
        cache->dict[1].entry = NULL;
        cache->dict[1].used  = 0;
        cache->data_head     = NULL;
        cache->data_tail     = NULL;
        cache->rehash_idx    = -1;
        cache->cleanup_idx   = 0;

        if(!cache_dict_init()) {
            goto err;
        }
        cache_debug("[CACHE] on, data_size=%llu\n", global.cache.data_size);
    }
    return;
err:
    Alert("Out of memory when initializing cache.\n");
    exit(1);
}

int cache_full() {
    return global.cache.data_size <= global.cache.stats->used_mem;
}

char *cache_build_key(struct cache_key **pck, struct stream *s,
        struct http_msg *msg) {

    struct http_txn *txn = s->txn;

    int https, host_len, path_len, query_len;
    char *host, *path_beg, *url_end, *query_beg, *cookie_beg, *cookie_end;
    struct hdr_ctx ctx;

    struct cache_key *ck = NULL;
    int key_len          = 0;
    int key_size         = CACHE_DEFAULT_KEY_SIZE;
    char *key            = malloc(key_size);
    if(!key) {
        return NULL;
    }

    https = 0;
#ifdef USE_OPENSSL
    if(s->sess->listener->xprt == &ssl_sock) {
        https = 1;
    }
#endif

    host     = NULL;
    host_len = 0;
    ctx.idx  = 0;
    if(http_find_header2("Host", 4, msg->chn->buf->p, &txn->hdr_idx, &ctx)) {
        host     = ctx.line + ctx.val;
        host_len = ctx.vlen;
    }

    path_beg = http_get_path(txn);
    url_end  = NULL;
    path_len = 0;
    if(path_beg) {
        char *ptr = path_beg;
        url_end   = msg->chn->buf->p + msg->sl.rq.u + msg->sl.rq.u_l;
        while(ptr < url_end && *ptr != '?') {
            ptr++;
        }
        path_len = ptr - path_beg;
    }

    query_beg = NULL;
    query_len = 0;
    if(path_beg) {
        query_beg = memchr(path_beg, '?', url_end - path_beg);
        query_beg = query_beg ? query_beg + 1 : NULL;
        query_len = url_end - query_beg;
    }

    ctx.idx    = 0;
    cookie_beg = NULL;
    cookie_end = NULL;
    if(http_find_header2("Cookie", 6, msg->chn->buf->p, &txn->hdr_idx, &ctx)) {
        cookie_beg = ctx.line + ctx.val;
        cookie_end = cookie_beg + ctx.vlen;
    }

    cache_debug("[CACHE] Calculate key: ");
    while((ck = *pck++)) {
        switch(ck->type) {
            case CK_METHOD:
                cache_debug("method.");
                key = _cache_key_append(key, &key_len, &key_size, http_known_methods[txn->meth].name, strlen(http_known_methods[txn->meth].name));
                if(!key) return NULL;
                break;
            case CK_SCHEME:
                cache_debug("scheme.");
                key = _cache_key_append(key, &key_len, &key_size, https ? "HTTPS": "HTTP", strlen(https ? "HTTPS": "HTTP"));
                if(!key) return NULL;
                break;
            case CK_HOST:
                cache_debug("host.");
                if(host) {
                    key = _cache_key_append(key, &key_len, &key_size, host, host_len);
                    if(!key) return NULL;
                }
                break;
            case CK_PATH:
                cache_debug("path.");
                if(path_beg) {
                    key = _cache_key_append(key, &key_len, &key_size, path_beg, path_len);
                    if(!key) return NULL;
                }
                break;
            case CK_QUERY:
                cache_debug("query.");
                if(query_beg) {
                    key = _cache_key_append(key, &key_len, &key_size, query_beg, query_len);
                    if(!key) return NULL;
                }
                break;
            case CK_PARAM:
                cache_debug("param_%s.", ck->data);
                if(query_beg) {
                    char *v = NULL;
                    int v_l = 0;
                    if(_cache_find_param_value_by_name(query_beg, url_end, ck->data, &v, &v_l)) {
                        key = _cache_key_append(key, &key_len, &key_size, v, v_l);
                        if(!key) return NULL;
                    }

                }
                break;
            case CK_HEADER:
                ctx.idx = 0;
                cache_debug("header_%s.", ck->data);
                if(http_find_header2(ck->data, strlen(ck->data), msg->chn->buf->p, &txn->hdr_idx, &ctx)) {
                    key = _cache_key_append(key, &key_len, &key_size, ctx.line + ctx.val, ctx.vlen);
                    if(!key) return NULL;
                }
                break;
            case CK_COOKIE:
                cache_debug("header_%s.", ck->data);
                if(cookie_beg) {
                    char *v = NULL;
                    int v_l = 0;
                    if(extract_cookie_value(cookie_beg, cookie_end, ck->data, strlen(ck->data), 1, &v, &v_l)) {
                        key = _cache_key_append(key, &key_len, &key_size, v, v_l);
                        if(!key) return NULL;
                    }
                }
                break;
            case CK_BODY:
                cache_debug("body.");
                if(txn->meth == HTTP_METH_POST || txn->meth == HTTP_METH_PUT) {
                    if((s->be->options & PR_O_WREQ_BODY) && msg->body_len > 0 ) {
                        key = _cache_key_append(key, &key_len, &key_size, msg->chn->buf->p + msg->sov, msg->body_len);
                        if(!key) return NULL;
                    }
                }
                break;
            default:
                break;
        }
    }
    cache_debug("\n");
    return key;
}

uint64_t cache_hash_key(const char *key) {
    return XXH64(key, strlen(key), 0);
}

/*
 * Check if valid cache exists
 */
struct cache_data *cache_exists(const char *key, uint64_t hash) {
    struct cache_entry *entry = NULL;

    if(!key) return NULL;

    entry = cache_dict_get(hash, key);
    if(entry && entry->state == CACHE_ENTRY_STATE_VALID) {
        return entry->data;
    }

    return NULL;
}

/*
 * Start to create cache,
 * if cache does not exist, add a new cache_entry
 * if cache exists but expired, add a new cache_data to the entry
 * otherwise, set the corresponding state: bypass, wait
 */
void cache_create(struct cache_ctx *ctx, char *key, uint64_t hash) {
    struct cache_entry *entry = NULL;

    /* Check if cache is full */
    if(cache_full()) {
        ctx->state = CACHE_CTX_STATE_FULL;
        return;
    }

    entry = cache_dict_get(hash, key);
    if(entry) {
        if(entry->state == CACHE_ENTRY_STATE_CREATING) {
            ctx->state = CACHE_CTX_STATE_WAIT;
        } else if(entry->state == CACHE_ENTRY_STATE_VALID) {
            ctx->state = CACHE_CTX_STATE_HIT;
        } else if(entry->state == CACHE_ENTRY_STATE_EXPIRED || entry->state == CACHE_ENTRY_STATE_INVALID) {
            entry->state = CACHE_ENTRY_STATE_CREATING;
            entry->data = cache_data_new();
            if(!entry->data) {
                ctx->state = CACHE_CTX_STATE_BYPASS;
                return;
            }
            ctx->state = CACHE_CTX_STATE_CREATE;
        } else {
            ctx->state = CACHE_CTX_STATE_BYPASS;
        }
    } else {
        entry = cache_dict_set(hash, key);
        if(entry) {
            ctx->state = CACHE_CTX_STATE_CREATE;
        } else {
            ctx->state = CACHE_CTX_STATE_BYPASS;
            return;
        }
    }
    ctx->entry   = entry;
    ctx->data    = entry->data;
    ctx->element = entry->data->element;
}

/*
 * Add partial http data to cache_data
 */
int cache_update(struct cache_ctx *ctx, struct http_msg *msg, long msg_len) {
    struct cache_element *element = cache_data_append(ctx->element, msg, msg_len);

    if(element) {
        if(!ctx->element) {
            ctx->data->element = element;
        }
        ctx->element = element;
        return 1;
    } else {
        return 0;
    }
}

/*
 * cache done
 */
void cache_finish(struct cache_ctx *ctx) {
    ctx->state = CACHE_CTX_STATE_DONE;
    ctx->entry->state = CACHE_ENTRY_STATE_VALID;
    if(ctx->rule->ttl == 0) {
        ctx->entry->expire = 0;
    } else {
        ctx->entry->expire = _get_current_timestamp() + ctx->rule->ttl;
    }
}

void cache_abort(struct cache_ctx *ctx) {
    ctx->entry->state = CACHE_ENTRY_STATE_INVALID;
}

/*
 * Create cache applet to handle the request
 */
void cache_hit(struct stream *s, struct stream_interface *si, struct channel *req,
        struct channel *res, struct cache_data *data) {

    struct appctx *appctx = NULL;

    /*
     * set backend to cache_applet
     */
    s->target = &cache_applet.obj_type;
    if(unlikely(!stream_int_register_handler(si, objt_applet(s->target)))) {
        /* return to regular process on error */
        s->target = NULL;
    } else {
        appctx = si_appctx(si);
        memset(&appctx->ctx.cache, 0, sizeof(appctx->ctx.cache));
        appctx->ctx.cache.data    = data;
        appctx->ctx.cache.element = data->element;

        req->analysers &= ~AN_REQ_FLT_HTTP_HDRS;
        req->analysers &= ~AN_REQ_FLT_XFER_DATA;

        req->analysers |= AN_REQ_FLT_END;
        req->analyse_exp = TICK_ETERNITY;

        res->flags |= CF_NEVER_WAIT;
    }
}

/*
 * The cache applet acts like the backend to send cached http data
 */
static void cache_io_handler(struct appctx *appctx) {
    struct stream_interface *si   = appctx->owner;
    struct channel *res           = si_ic(si);
    struct stream *s              = si_strm(si);
    struct cache_element *element = NULL;
    int ret;

    if(appctx->ctx.cache.element) {
        if(appctx->ctx.cache.element == appctx->ctx.cache.data->element) {
            s->res.analysers = 0;
            s->res.analysers |= (AN_RES_WAIT_HTTP | AN_RES_HTTP_PROCESS_BE | AN_RES_HTTP_XFER_BODY);
            appctx->ctx.cache.data->clients++;
        }
        element = appctx->ctx.cache.element;
        ret = bi_putblk(res, element->msg, element->msg_len);
        if(ret >= 0) {
            appctx->ctx.cache.element = element->next;
        } else if(ret == -2) {
            appctx->ctx.cache.data->clients--;
            si_shutr(si);
            res->flags |= CF_READ_NULL;
        }
    } else {
        bo_skip(si_oc(si), si_ob(si)->o);
        si_shutr(si);
        res->flags |= CF_READ_NULL;
        appctx->ctx.cache.data->clients--;
    }
}

void cache_debug(const char *fmt, ...) {
    if((global.mode & MODE_DEBUG)) {
        va_list args;
        va_start(args, fmt);
        vfprintf(stderr, fmt, args);
        va_end(args);
    }
}

struct applet cache_applet = {
    .obj_type = OBJ_TYPE_APPLET,
    .name = "<CACHE>",
    .fct = cache_io_handler,
    .release = NULL,
};

__attribute__((constructor)) static void __cache_init(void) { }

