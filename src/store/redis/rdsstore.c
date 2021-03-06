#include <nchan_module.h>

#include <assert.h>
#include "store.h"

#include "redis_nginx_adapter.h"
#include "redis_lua_commands.h"
#include <util/nchan_reaper.h>
#include <util/nchan_msgid.h>
#include <store/store_common.h>

#define NCHAN_CHANHEAD_EXPIRE_SEC 1
#define REDIS_CHANNEL_EMPTY_BUT_SUBSCRIBED_TTL 300
#define REDIS_LUA_HASH_LENGTH 40

#define REDIS_DEFAULT_PING_INTERVAL_TIME 4*60

#define REDIS_RECONNECT_TIME 5000

static ngx_str_t REDIS_DEFAULT_URL = ngx_string("127.0.0.1:6379");

typedef struct nchan_store_channel_head_s nchan_store_channel_head_t;

#include "../spool.h"

struct nchan_store_channel_head_s {
  ngx_str_t                    id; //channel id
  channel_spooler_t            spooler;
  ngx_uint_t                   generation; //subscriber pool generation.
  chanhead_pubsub_status_t     status;
  ngx_uint_t                   sub_count;
  ngx_int_t                    fetching_message_count;
  ngx_uint_t                   internal_sub_count;
  ngx_event_t                  keepalive_timer;
  nchan_msg_id_t               last_msgid;
  void                        *redis_subscriber_privdata;
  
  nchan_store_channel_head_t  *gc_prev;
  nchan_store_channel_head_t  *gc_next;
  time_t                       gc_time;
  unsigned                     in_gc_queue:1;
  
  unsigned                     meta:1;
  unsigned                     shutting_down:1;
  UT_hash_handle               hh;
};

typedef struct {
  u_char       *password;
  u_char       *host;
  ngx_int_t     port;
  ngx_int_t     db;
} redis_connect_params_t;

typedef struct callback_chain_s callback_chain_t;
struct callback_chain_s {
  callback_pt                  cb;
  void                        *pd;
  callback_chain_t            *next;
};

typedef struct {
  nchan_store_channel_head_t      *subhash;
  redis_connect_params_t          *connect_params;
  ngx_str_t                       *connect_url;
  
  u_char                           subscriber_id[255];
  u_char                           subscriber_channel[255];
  
  redisAsyncContext               *ctx;
  redisAsyncContext               *sub_ctx;
  redisContext                    *sync_ctx;
  
  redis_connection_status_t        status;
  int                              scripts_loaded_count;
  int                              generation;
  ngx_event_t                      reconnect_timer;
  ngx_event_t                      ping_timer;
  time_t                           ping_interval;
  callback_chain_t                *on_connected;
  
  void                            (*set_status)(redis_connection_status_t status, const redisAsyncContext *ac);
  
  nchan_reaper_t                   chanhead_reaper;
  
  unsigned                         shutting_down:1;
} rdstore_data_t;

static rdstore_data_t        rdt;


redis_connection_status_t redis_connection_status(void) {
  return rdt.status;
}

ngx_int_t redis_store_callback_on_connected(callback_pt cb, void *privdata) {
  callback_chain_t  *d;
  
  if(rdt.status == CONNECTED) {
    cb(NGX_OK, NULL, privdata);
  }
  
  d = ngx_alloc(sizeof(*d), ngx_cycle->log);
  
  d->cb = cb;
  d->pd = privdata;
  d->next = rdt.on_connected;
  
  rdt.on_connected = d;
  return NGX_OK;
}

static ngx_int_t redis_ensure_connected();

#define CHANNEL_HASH_FIND(id_buf, p)    HASH_FIND( hh, rdt.subhash, (id_buf)->data, (id_buf)->len, p)
#define CHANNEL_HASH_ADD(chanhead)      HASH_ADD_KEYPTR( hh, rdt.subhash, (chanhead->id).data, (chanhead->id).len, chanhead)
#define CHANNEL_HASH_DEL(chanhead)      HASH_DEL( rdt.subhash, chanhead)

#include <stdbool.h>
#include "cmp.h"

//#define DEBUG_LEVEL NGX_LOG_WARN
#define DEBUG_LEVEL NGX_LOG_DEBUG

#define DBG(fmt, args...) ngx_log_error(DEBUG_LEVEL, ngx_cycle->log, 0, "REDISTORE: " fmt, ##args)
#define ERR(fmt, args...) ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "REDISTORE: " fmt, ##args)

#define redis_command(cb, pd, fmt, args...)                          \
  do {                                                               \
    if(redis_ensure_connected() == NGX_OK) {                         \
      redisAsyncCommand(rdt.ctx, cb, pd, fmt, ##args);               \
    } else {                                                         \
      ERR("Can't run redis command: no connection to redis server.");\
    }                                                                \
  }while(0)                                                          \

#define redis_subscriber_command(cb, pd, fmt, args...)               \
  do {                                                               \
    if(redis_ensure_connected() == NGX_OK) {                         \
      redisAsyncCommand(rdt.sub_ctx, cb, pd, fmt, ##args);           \
    } else {                                                         \
      ERR("Can't run redis command: no connection to redis server.");\
    }                                                                \
  }while(0)                                                          \
  
#define redis_sync_command(fmt, args...)                             \
  do {                                                               \
    if(rdt.sync_ctx == NULL) {                                       \
      redis_nginx_open_sync_context(rdt.connect_params->host, rdt.connect_params->port, rdt.connect_params->db, rdt.connect_params->password, &rdt.sync_ctx);                                                \
  }                                                                  \
    if(rdt.sync_ctx) {                         \
      redisCommand(rdt.sync_ctx, fmt, ##args);                       \
    } else {                                                         \
      ERR("Can't run redis command: no connection to redis server.");\
    }                                                                \
  }while(0)
  

#define CHECK_REPLY_STR(reply) ((reply)->type == REDIS_REPLY_STRING)
#define CHECK_REPLY_STRVAL(reply, v) ( CHECK_REPLY_STR(reply) && ngx_strcmp((reply)->str, v) == 0 )
#define CHECK_REPLY_STRNVAL(reply, v, n) ( CHECK_REPLY_STR(reply) && ngx_strncmp((reply)->str, v, n) == 0 )
#define CHECK_REPLY_INT(reply) ((reply)->type == REDIS_REPLY_INTEGER)
#define CHECK_REPLY_INTVAL(reply, v) ( CHECK_REPLY_INT(reply) && (reply)->integer == v )
#define CHECK_REPLY_ARRAY_MIN_SIZE(reply, size) ( (reply)->type == REDIS_REPLY_ARRAY && (reply)->elements >= (unsigned )size )
#define CHECK_REPLY_NIL(reply) ((reply)->type == REDIS_REPLY_NIL)
#define CHECK_REPLY_INT_OR_STR(reply) ((reply)->type == REDIS_REPLY_INTEGER || (reply)->type == REDIS_REPLY_STRING)
  
static ngx_int_t chanhead_gc_add(nchan_store_channel_head_t *head, const char *reason);
static ngx_int_t chanhead_gc_withdraw(nchan_store_channel_head_t *chanhead);

static ngx_int_t nchan_store_publish_generic(ngx_str_t *, nchan_msg_t *, ngx_int_t, const ngx_str_t *);
static ngx_str_t * nchan_store_content_type_from_message(nchan_msg_t *, ngx_pool_t *);
static ngx_str_t * nchan_store_etag_from_message(nchan_msg_t *, ngx_pool_t *);

static nchan_store_channel_head_t * nchan_store_get_chanhead(ngx_str_t *channel_id);


static ngx_buf_t *set_buf(ngx_buf_t *buf, u_char *start, off_t len){
  ngx_memzero(buf, sizeof(*buf));
  buf->start = start;
  buf->end = start + len;
  buf->pos = buf->start;
  buf->last = buf->end;
  return buf;
}

static ngx_int_t ngx_strmatch(ngx_str_t *str, char *match) {
  return ngx_strncmp(str->data, match, str->len) == 0;
}

static u_char *fwd_buf(ngx_buf_t *buf, size_t sz) {
  u_char *ret = buf->pos;
  buf->pos += sz;
  return ret;
}

static void fwd_buf_to_str(ngx_buf_t *buf, size_t sz, ngx_str_t *str) {
  str->data = fwd_buf(buf, sz);
  str->len = sz;;
}

static bool ngx_buf_reader(cmp_ctx_t *ctx, void *data, size_t limit) {
  ngx_buf_t   *buf=(ngx_buf_t *)ctx->buf;
  if(buf->pos + limit > buf->last){
    return false;
  }
  ngx_memcpy(data, buf->pos, limit);
  buf->pos += limit;
  return true;
}
static size_t ngx_buf_writer(cmp_ctx_t *ctx, const void *data, size_t count) {
  return 0;
}

static redis_connect_params_t *parse_redis_url(ngx_str_t *url) {

  redis_connect_params_t  *params;
  u_char                  *cur_out, *cur, *last, *ret;
  
  cur = url->data;
  last = url->data + url->len;
  
  if((params = ngx_calloc(sizeof(*params) + url->len + 4, ngx_cycle->log)) == NULL) {
    return NULL;
  }
  
  cur_out = (u_char *)&params[1];
  
  //ignore redis://
  if(ngx_strnstr(cur, "redis://", 8) != NULL) {
    cur += 8;
  }
  
  if(cur[0] == ':') {
    cur++;
    if((ret = ngx_strlchr(cur, last, '@')) == NULL) {
      params->password = NULL;
    }
    else {
      params->password = cur_out;
      ngx_memcpy(cur_out, cur, ret - cur);
      cur_out += (ret - cur) + 1;
      cur = ret + 1;
    }
  }
  else {
    params->password = NULL;
  }
  
  ///port:host
  if((ret = ngx_strlchr(cur, last, ':')) == NULL) {
    //just host
    params->port = 6379;
    if((ret = ngx_strlchr(cur, last, '/')) == NULL) {
      ret = last;
    }
    params->host = cur_out;
    ngx_memcpy(cur_out, cur, ret-cur);
    //cur_out += (ret - cur) + 1;
    cur = ret;
  }
  else {
    params->host = cur_out;
    ngx_memcpy(cur_out, cur, ret-cur);
    //cur_out += (ret - cur) + 1;
    cur = ret + 1;
    
    //port
    if((ret = ngx_strlchr(cur, last, '/')) == NULL) {
      ret = last;
    }
    
    params->port = ngx_atoi(cur, ret-cur);
    if(params->port == NGX_ERROR) {
      params->port = 6379;
    }
  }
  
  if(cur[0] == '/') {
    cur++;
    params->db = ngx_atoi(cur, last-cur);
    if(params->db == NGX_ERROR) {
      params->db = 0;
    }
  }
  
  return params;
}

static void redis_store_reap_chanhead(nchan_store_channel_head_t *ch) {
  if(!ch->shutting_down) {
    assert(ch->sub_count == 0 && ch->fetching_message_count == 0);
  }
  
  DBG("UNSUBSCRIBING from channel:pubsub:%V", &ch->id);
  redis_subscriber_command(NULL, NULL, "UNSUBSCRIBE channel:pubsub:%b", STR(&ch->id));
  DBG("chanhead %p (%V) is empty and expired. delete.", ch, &ch->id);
  if(ch->keepalive_timer.timer_set) {
    ngx_del_timer(&ch->keepalive_timer);
  }
  stop_spooler(&ch->spooler, 1);
  CHANNEL_HASH_DEL(ch);
  ngx_free(ch);
}

static ngx_int_t nchan_redis_chanhead_ready_to_reap(nchan_store_channel_head_t *ch, uint8_t force) {
  if(!force) {
    if(ch->status != INACTIVE) {
      return NGX_DECLINED;
    }
    
    if(ch->gc_time - ngx_time() > 0) {
      DBG("not yet time to reap %V, %i sec left", &ch->id, ch->gc_time - ngx_time());
      return NGX_DECLINED;
    }
    
    if (ch->sub_count > 0) { //there are subscribers
      DBG("not ready to reap %V, %i subs left", &ch->id, ch->sub_count);
      return NGX_DECLINED;
    }
    
    if (ch->fetching_message_count > 0) { //there are subscribers
      DBG("not ready to reap %V, fetching %i messages", &ch->id, ch->fetching_message_count);
      return NGX_DECLINED;
    }
    
    //DBG("ok to delete channel %V", &ch->id);
    return NGX_OK;
  }
  else {
    //force delete is always ok
    return NGX_OK;
  }
}

static redisAsyncContext **whichRedisContext(const redisAsyncContext *ac) {
  if(rdt.ctx == ac) {
    return &rdt.ctx;
  }
  else if(rdt.sub_ctx == ac) {
    return &rdt.sub_ctx;
  }
  return NULL;
}

static void rdt_set_status(redis_connection_status_t status, const redisAsyncContext *ac) {
  redis_connection_status_t prev_status = rdt.status;
  rdt.status = status;
  
  if(status == DISCONNECTED) {
    if(!rdt.reconnect_timer.timer_set) {
      ngx_add_timer(&rdt.reconnect_timer, REDIS_RECONNECT_TIME);
    }
    if(rdt.ping_timer.timer_set) {
      ngx_del_timer(&rdt.ping_timer);
    }
    
    if(prev_status == CONNECTED) {
      nchan_store_channel_head_t   *cur, *tmp;
      HASH_ITER(hh, rdt.subhash, cur, tmp) {
        cur->spooler.fn->broadcast_status(&cur->spooler, NGX_HTTP_GONE, &NCHAN_HTTP_STATUS_410);
        chanhead_gc_add(cur, "redis connection gone");
      }
      nchan_reaper_flush(&rdt.chanhead_reaper);
    }
    
    if(ac) {
      redisAsyncContext **pac = whichRedisContext(ac);
      if(pac)
        *pac = NULL;
    }
  }
  else if(status == CONNECTED && prev_status != CONNECTED) {
    callback_chain_t    *cur, *next;
    
    if(!rdt.ping_timer.timer_set && rdt.ping_interval) {
      ngx_add_timer(&rdt.ping_timer, rdt.ping_interval * 1000);
    }
    
    //on_connected callbacks
    cur = rdt.on_connected;
    rdt.on_connected = NULL;
    
    while(cur != NULL) {
      next = cur->next;
      cur->cb(NGX_OK, NULL, cur->pd);
      ngx_free(cur);
      cur = next;
    }
    rdt.generation++;
  }
}

static void redis_reconnect_timer_handler(ngx_event_t *ev) {
  if(!ev->timedout || ngx_exiting || ngx_quit)
    return;
  ev->timedout = 0;
  redis_ensure_connected();
}

static void redisCheckErrorCallback(redisAsyncContext *c, void *r, void *privdata);
static int redisReplyOk(redisAsyncContext *c, void *r);

static void redis_ping_callback(redisAsyncContext *c, void *r, void *privdata) {
  redisReply         *reply = (redisReply *)r;
  if(redisReplyOk(c, r)) {
    if(CHECK_REPLY_INT(reply)) {
      if(reply->integer < 1) {
        ERR("failed to forward ping to sub_ctx");
      }
    }
    else {
      ERR("unexpected reply type for redis_ping_callback");
    }
  }
}

static void redis_ping_timer_handler(ngx_event_t *ev) {
  
  if(!ev->timedout || ngx_exiting || ngx_quit)
    return;
  
  ev->timedout = 0;
  if(rdt.status == CONNECTED && rdt.ctx && rdt.sub_ctx) {
    redis_command(redis_ping_callback, NULL, "PUBLISH %s ping", rdt.subscriber_channel);
    if(rdt.ping_interval) {
      ngx_add_timer(ev, rdt.ping_interval * 1000);
    }
  }
}

static ngx_int_t nchan_store_init_worker(ngx_cycle_t *cycle) {
  
  rdt.subhash = NULL;
  rdt.connect_params = parse_redis_url(rdt.connect_url);
  rdt.status = DISCONNECTED;
  rdt.generation = 0;
  rdt.set_status = rdt_set_status;
  rdt.shutting_down = 0;
  nchan_init_timer(&rdt.reconnect_timer, redis_reconnect_timer_handler, NULL);
  nchan_init_timer(&rdt.ping_timer, redis_ping_timer_handler, NULL);
  
  ngx_memzero(&rdt.subscriber_id, sizeof(rdt.subscriber_id));
  ngx_memzero(&rdt.subscriber_channel, sizeof(rdt.subscriber_channel));
  ngx_snprintf(rdt.subscriber_id, 255, "worker:%i:time:%i", ngx_pid, ngx_time());
  ngx_snprintf(rdt.subscriber_channel, 255, "nchan:%s", rdt.subscriber_id);
  
  redis_nginx_init();
  
  nchan_reaper_start(&rdt.chanhead_reaper, 
                    "redis chanhead", 
                    offsetof(nchan_store_channel_head_t, gc_prev), 
                    offsetof(nchan_store_channel_head_t, gc_next), 
    (ngx_int_t (*)(void *, uint8_t)) nchan_redis_chanhead_ready_to_reap,
        (void (*)(void *)) redis_store_reap_chanhead,
                    4
  );
  
  return redis_ensure_connected();
}

static void redisCheckErrorCallback(redisAsyncContext *c, void *r, void *privdata) {
  redisReplyOk(c, r);
}
static int redisReplyOk(redisAsyncContext *c, void *r) {
  static const ngx_str_t script_error_start= ngx_string("ERR Error running script (call to f_");
  redisReply *reply = (redisReply *)r;
  if(reply == NULL) { //redis disconnected?...
    if(c->err) {
      ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "connection to redis failed while waiting for reply - %s", c->errstr);
    }
    else {
      ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "got a NULL redis reply for unknown reason");
    }
    return 0;
  }
  else if(reply->type == REDIS_REPLY_ERROR) {
    if(ngx_strncmp(reply->str, script_error_start.data, script_error_start.len) == 0 && (unsigned ) reply->len > script_error_start.len + REDIS_LUA_HASH_LENGTH) {
      char *hash = &reply->str[script_error_start.len];
      redis_lua_script_t  *script;
      REDIS_LUA_SCRIPTS_EACH(script) {
        if (ngx_strncmp(script->hash, hash, REDIS_LUA_HASH_LENGTH)==0) {
          ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "REDIS SCRIPT ERROR: %s :%s", script->name, &reply->str[script_error_start.len + REDIS_LUA_HASH_LENGTH + 2]);
          return 0;
        }
      }
      ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "REDIS SCRIPT ERROR: (unknown): %s", reply->str);
    }
    else {
      ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "REDIS_REPLY_ERROR: %s", reply->str);
    }
    return 0;
  }
  else {
    return 1;
  }
}

static void redisEchoCallback(redisAsyncContext *c, void *r, void *privdata) {
  redisReply *reply = r;
  unsigned    i;
  //nchan_channel_t * channel = (nchan_channel_t *)privdata;
  if(c->err) {
    if(rdt.status != DISCONNECTED) {
      ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "connection to redis failed - %s", c->errstr);
      rdt.set_status(DISCONNECTED, c);
    }
    return;
  }
  else if (reply == NULL) {
    ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "REDIS REPLY is NULL");
    return;
  }
  switch(reply->type) {
    case REDIS_REPLY_STATUS:
      ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "REDIS_REPLY_STATUS  %s", reply->str);
      break;
      
    case REDIS_REPLY_ERROR:
      redisCheckErrorCallback(c, r, privdata);
      break;
      
    case REDIS_REPLY_INTEGER:
      ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "REDIS_REPLY_INTEGER: %i", reply->integer);
      break;
      
    case REDIS_REPLY_NIL:
      ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "REDIS_REPLY_NIL: nil");
      break;
      
    case REDIS_REPLY_STRING:
      ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "REDIS_REPLY_STRING: %s", reply->str);
      break;
      
    case REDIS_REPLY_ARRAY:
      ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "REDIS_REPLY_ARRAY: %i", reply->elements);
      for(i=0; i< reply->elements; i++) {
        redisEchoCallback(c, reply->element[i], "  ");
      }
      break;
  }
  //redis_subscriber_command(NULL, NULL, "UNSUBSCRIBE channel:%b:pubsub", str(&(channel->id)));
}

static void redis_load_script_callback(redisAsyncContext *c, void *r, void *privdata) {
  redis_lua_script_t  *script = privdata;

  redisReply *reply = r;
  if (reply == NULL) return;
  switch(reply->type) {
    case REDIS_REPLY_ERROR:
      ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "nchan: Failed loading redis lua script %s :%s", script->name, reply->str);
      break;
    case REDIS_REPLY_STRING:
      if(ngx_strncmp(reply->str, script->hash, REDIS_LUA_HASH_LENGTH)!=0) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "nchan: Redis lua script %s has unexpected hash %s (expected %s)", script->name, reply->str, script->hash);
      }
      else {
        //everything went well
        if(rdt.status == LOADING_SCRIPTS) {
          rdt.scripts_loaded_count++;
          if(rdt.scripts_loaded_count == redis_lua_scripts_count) {
            char        *action;
            ngx_uint_t   loglevel;
            if(rdt.generation == 0) {
              action = "Established";
              loglevel = NGX_LOG_NOTICE;
            }
            else {
              action = "Re-established";
              loglevel = NGX_LOG_WARN;
            }
            
            ngx_log_error(loglevel, ngx_cycle->log, 0, "Nchan: %s connection to redis at %V.", action, rdt.connect_url);
            rdt.set_status(CONNECTED, NULL);
          }
        }
      }
      break;
  }
}

static void redisInitScripts(void){
  redis_lua_script_t  *script;
  if(rdt.ctx == NULL) {
    ERR("unable to init lua scripts: redis connection not initialized.");
  }
  else {
    rdt.set_status(LOADING_SCRIPTS, NULL);
    rdt.scripts_loaded_count = 0;
    REDIS_LUA_SCRIPTS_EACH(script) {
      redisAsyncCommand(rdt.ctx, &redis_load_script_callback, script, "SCRIPT LOAD %s", script->script);
    }
  }
}

static void redis_subscriber_callback(redisAsyncContext *c, void *r, void *privdata);

static void redis_nginx_connect_event_handler(const redisAsyncContext *ac, int status) {
  //ERR("redis_nginx_connect_event_handler %v: %i", rdt.connect_url, status);
  if(status != REDIS_OK) {
    if(rdt.status != DISCONNECTED) {
      char *action = rdt.generation == 0 ? "connect" : "reconnect";
      if(ac->errstr) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "Nchan: Can't %s to redis at %V: %s", action, rdt.connect_url, ac->errstr);
      }
      else {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "Nchan: Can't %s to redis at %V.", action, rdt.connect_url);
      }
    }
    rdt.set_status(DISCONNECTED, ac);
  }
}

static void redis_nginx_disconnect_event_handler(const redisAsyncContext *ac, int status) {
  DBG("connection to redis for %V closed: %s", rdt.connect_url, ac->errstr);
  
  if(rdt.status == CONNECTED && !ngx_exiting && !ngx_quit && !rdt.shutting_down) {
    if(ac->err) {
      ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "Nchan: Lost connection to redis at %V: %s.", rdt.connect_url, ac->errstr);
    }
    else {
      ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "Nchan: Lost connection to redis at %V.", rdt.connect_url);
    }
  }
  rdt.set_status(DISCONNECTED, ac);
  
}

static void redis_server_persistence_info_check(redisAsyncContext *ac);

static void redis_check_if_still_loading_handler(ngx_event_t *ev) {
  DBG("still loading?,,.");
  if(rdt.status != DISCONNECTED && rdt.ctx) {
    redis_server_persistence_info_check(rdt.ctx);
  }
  
  ngx_free(ev);
}

void redis_nginx_info_persistence_callback(redisAsyncContext *ac, void *rep, void *privdata) {
  redisReply             *reply = rep;
  int                     loading = -1;
  //DBG("redis_nginx_info_persistence_callback %p", ac);
  if(ac->err || !redisReplyOk(ac, rep) || reply->type != REDIS_REPLY_STRING) {
    return;
  }
  
  if(ngx_strstrn((u_char *)reply->str, "loading:1", 8)) {
    loading = 1;
  }
  else if(ngx_strstrn((u_char *)reply->str, "loading:0", 8)) {
    loading = 0;
  }
  else {
    ERR("unexpected INFO PERSISTENCE reply");
    return;
  }
  
  if(loading == 1) {
    ngx_log_error(NGX_LOG_WARN, ngx_cycle->log, 0, "Nchan: Redis server at %V is still loading data.", rdt.connect_url);
    ngx_event_t      *evt = ngx_calloc(sizeof(*evt), ngx_cycle->log);
    nchan_init_timer(evt, redis_check_if_still_loading_handler, NULL);
    rdt.set_status(LOADING, ac);
    ngx_add_timer(evt, 1000);
  }
  else if(loading == 0) {
    DBG("everything loaded and good to go");
    redisInitScripts();
    if(rdt.sub_ctx) {
      redisAsyncCommand(rdt.sub_ctx, redis_subscriber_callback, NULL, "SUBSCRIBE %s", rdt.subscriber_channel);
    }
    else {
      ERR("rdt.sub_ctx NULL, can't subscribe");
    }
  }
}

static void redis_server_persistence_info_check(redisAsyncContext *ac) {
  //DBG("redis_server_persistence_info_check %p", ac);
  redisAsyncCommand(ac, redis_nginx_info_persistence_callback, NULL, "INFO persistence", rdt.connect_params->password);
}

void redis_nginx_select_callback(redisAsyncContext *ac, void *rep, void *privdata) {
  redisReply *reply = rep;
  if ((reply == NULL) || (reply->type == REDIS_REPLY_ERROR)) {
    if(rdt.status == CONNECTING) {
      ERR("could not select redis database");
    }
    rdt.set_status(DISCONNECTED, ac);
    redisAsyncFree(ac);
  }
  else if(rdt.ctx && rdt.sub_ctx && rdt.status == CONNECTING && !rdt.ctx->err && !rdt.sub_ctx->err) {
    rdt.set_status(AUTHENTICATING, NULL);
    redis_server_persistence_info_check(ac);
  }
}

void redis_nginx_auth_callback(redisAsyncContext *ac, void *rep, void *privdata) {
  redisReply *reply = rep;
  if ((reply == NULL) || (reply->type == REDIS_REPLY_ERROR)) {
    if(rdt.status == CONNECTING) {
      ERR("AUTH command failed, probably because the password is incorrect");
      rdt.set_status(DISCONNECTED, ac);
    }
  }
}

static int redis_initialize_ctx(redisAsyncContext **ctx) {
  if(*ctx == NULL) {
    redis_nginx_open_context(rdt.connect_params->host, rdt.connect_params->port, rdt.connect_params->db, rdt.connect_params->password, ctx);
    if(*ctx != NULL) {
      if(rdt.connect_params->password) {
        redisAsyncCommand(*ctx, redis_nginx_auth_callback, NULL, "AUTH %s", rdt.connect_params->password);
      }
      redisAsyncCommand(*ctx, redis_nginx_select_callback, NULL, "SELECT %d", rdt.connect_params->db);
      redisAsyncSetConnectCallback(*ctx, redis_nginx_connect_event_handler);
      redisAsyncSetDisconnectCallback(*ctx, redis_nginx_disconnect_event_handler);
      return 1;
    }
    else {
      ERR("can't initialize redis ctx %p", ctx);
      return 0;
    }
  }
  else {
    return 0;
  }
}

static ngx_int_t redis_ensure_connected() {
  int connecting = 0;
  if(redis_initialize_ctx(&rdt.ctx)) {
    connecting = 1;
  }
  
  if(redis_initialize_ctx(&rdt.sub_ctx)) {
    connecting = 1;
  }
    
  if(rdt.ctx && rdt.sub_ctx) {
    if(connecting) {
      rdt.set_status(CONNECTING, NULL);
    }
    return NGX_OK;
  }
  else {
    return NGX_DECLINED;
  }

}

static ngx_int_t msg_from_redis_get_message_reply(nchan_msg_t *msg, ngx_buf_t *buf, redisReply *r, uint16_t offset);

#define SLOW_REDIS_REPLY 100 //ms

static ngx_int_t log_redis_reply(char *name, ngx_msec_t t) {
  ngx_msec_t   dt = ngx_current_msec - t;
  if(dt >= SLOW_REDIS_REPLY) {
    DBG("redis command %s took %i msec", name, dt);
  }
  return NGX_OK;
}

static ngx_int_t redisReply_to_int(redisReply *el, ngx_int_t *integer) {
  if(CHECK_REPLY_INT(el)) {
    *integer=el->integer;
  }
  else if(CHECK_REPLY_STR(el)) {
    *integer=ngx_atoi((u_char *)el->str, el->len);
  }
  else {
    return NGX_ERROR;
  }
  return NGX_OK;
}

typedef struct {
  ngx_msec_t                    t;
  char                         *name;
  ngx_str_t                     channel_id;
  nchan_msg_id_t               *msg_id;
} redis_get_message_from_key_data_t;

static void get_msg_from_msgkey_callback(redisAsyncContext *c, void *r, void *privdata) {
  redis_get_message_from_key_data_t *d = (redis_get_message_from_key_data_t *)privdata;
  redisReply           *reply = r;
  nchan_msg_t           msg;
  ngx_buf_t             msgbuf;
  ngx_str_t            *chid = &d->channel_id;
  DBG("get_msg_from_msgkey_callback");
  log_redis_reply(d->name, d->t);
  
  if(reply) {
    if(chid == NULL) {
      ERR("get_msg_from_msgkey channel id is NULL");
      return;
    }
    if(msg_from_redis_get_message_reply(&msg, &msgbuf, reply, 0) != NGX_OK) {
      ERR("invalid message or message absent after get_msg_from_key");
      return;
    }
    nchan_store_publish_generic(chid, &msg, 0, NULL);
  }
  else {
    //reply is NULL
  }
  ngx_free(d);
}

static bool cmp_err(cmp_ctx_t *cmp) {
  ERR("msgpack parsing error: %s", cmp_strerror(cmp));
  return false;
}

static bool cmp_to_str(cmp_ctx_t *cmp, ngx_str_t *str) {
  ngx_buf_t      *mpbuf =(ngx_buf_t *)cmp->buf;
  uint32_t        sz;
  
  if(cmp_read_str_size(cmp, &sz)) {
    fwd_buf_to_str(mpbuf, sz, str);
    return true;
  }
  else {
    cmp_err(cmp);
    return false;
  }
}

static bool cmp_to_msg(cmp_ctx_t *cmp, nchan_msg_t *msg, ngx_buf_t *buf) {
  ngx_buf_t  *mpb = (ngx_buf_t *)cmp->buf;
  uint32_t    sz;
  uint64_t    msgtag;
  int32_t     ttl;
  //ttl 
  if(!cmp_read_int(cmp, &ttl)) {
    return cmp_err(cmp);
  }
  assert(ttl >= 0);
  if(ttl == 0) {
    ttl++; // less than a second left for this message... give it a second's lease on life
  }
  msg->expires = ngx_time() + ttl;
  
  //msg id
  if(!cmp_read_uinteger(cmp, (uint64_t *)&msg->id.time)) {
    return cmp_err(cmp);
  }
  if(!cmp_read_uinteger(cmp, &msgtag)) {
    return cmp_err(cmp);
  }
  else {
    msg->id.tag.fixed[0] = msgtag;
    msg->id.tagactive = 0;
    msg->id.tagcount = 1;
  }
  
  //msg prev_id
  if(!cmp_read_uinteger(cmp, (uint64_t *)&msg->prev_id.time)) {
    return cmp_err(cmp);
  }
  if(!cmp_read_uinteger(cmp, &msgtag)) {
    return cmp_err(cmp);
  }
  else {
    msg->prev_id.tag.fixed[0] = msgtag;
    msg->prev_id.tagactive = 0;
    msg->prev_id.tagcount = 1;
  }
  
  //message data
  if(!cmp_read_str_size(cmp, &sz)) {
    return cmp_err(cmp);
  }
  set_buf(buf, mpb->pos, sz);
  fwd_buf(mpb, sz);
  buf->memory = 1;
  buf->last_buf = 1;
  buf->last_in_chain = 1;
  msg->buf = buf;

  //content-type
  if(!cmp_read_str_size(cmp, &sz)) {
    return cmp_err(cmp);
  }
  fwd_buf_to_str(mpb, sz, &msg->content_type);
  
  //eventsource_event
  if(!cmp_read_str_size(cmp, &sz)) {
    return cmp_err(cmp);
  }
  fwd_buf_to_str(mpb, sz, &msg->eventsource_event);
  
  return true;
}

static ngx_int_t get_msg_from_msgkey(ngx_str_t *channel_id, nchan_msg_id_t *msgid, ngx_str_t *msg_redis_hash_key) {
  nchan_store_channel_head_t          *head;
  redis_get_message_from_key_data_t   *d;
  DBG("Get message from msgkey %V", msg_redis_hash_key);
  
  head = nchan_store_get_chanhead(channel_id);
  if(head->sub_count == 0) {
    DBG("Nobody wants this message we'll need to grab with an HMGET");
    return NGX_OK;
  }
  
  if((d=ngx_alloc(sizeof(*d) + (u_char)channel_id->len, ngx_cycle->log)) == 0) {
    ERR("nchan: unable to allocate memory for callback data for message hmget");
    return NGX_ERROR;
  }
  d->channel_id.len = channel_id->len;
  d->channel_id.data = (u_char *)&d[1];
  ngx_memcpy(d->channel_id.data, channel_id->data, channel_id->len);
  d->t = ngx_current_msec;
  d->name = "get_message_from_key";
  
  //d->hcln = put_current_subscribers_in_limbo(head);
  //assert(d->hcln != 0);
  
  redis_command(&get_msg_from_msgkey_callback, d, "EVALSHA %s 1 %b", redis_lua_scripts.get_message_from_key.hash, STR(msg_redis_hash_key));

  return NGX_OK;
}

static ngx_int_t redis_subscriber_register(nchan_store_channel_head_t *chanhead, subscriber_t *sub);

static void redis_subscriber_callback(redisAsyncContext *c, void *r, void *privdata) {
  redisReply             *reply = r;
  redisReply             *el = NULL;
  nchan_msg_t             msg;
  ngx_buf_t               buf;
  ngx_str_t               chid = ngx_null_string;
  ngx_str_t               pubsub_channel; 
  ngx_str_t               msg_redis_hash_key = ngx_null_string;
  ngx_uint_t              subscriber_id;
  
  ngx_buf_t               mpbuf;
  cmp_ctx_t               cmp;
  
  ngx_memzero(&buf, sizeof(buf)); 
  
  //ngx_log_error(NGX_LOG_WARN, ngx_cycle->log, 0, "redis_subscriber_callback,  privdata=%p", privdata);
  
  nchan_store_channel_head_t *chanhead = (nchan_store_channel_head_t *)privdata;
  
  msg.expires = 0;
  msg.refcount = 0;
  msg.buf = NULL;

  if(reply == NULL) return;
  if(CHECK_REPLY_ARRAY_MIN_SIZE(reply, 3)
    && CHECK_REPLY_STRVAL(reply->element[0], "message")
    && CHECK_REPLY_STR(reply->element[1])
    && CHECK_REPLY_STR(reply->element[2])) {
    
    pubsub_channel.data = (u_char *)reply->element[1]->str;
    pubsub_channel.len = reply->element[1]->len;
  
    //reply->element[1] is the pubsub channel name
    el = reply->element[2];
    
    if(CHECK_REPLY_STRVAL(el, "ping") && ngx_strmatch(&pubsub_channel, (char *)&rdt.subscriber_channel)) {
      DBG("got pinged");
    }
    else if(CHECK_REPLY_STR(el)) {
      uint32_t    array_sz;
      //maybe a message?
      set_buf(&mpbuf, (u_char *)el->str, el->len);
      cmp_init(&cmp, &mpbuf, ngx_buf_reader, ngx_buf_writer);
      if(cmp_read_array(&cmp, &array_sz)) {
        
        if(array_sz != 0) {
          uint32_t      sz;
          ngx_str_t     msg_type;
          cmp_read_str_size(&cmp ,&sz);
          fwd_buf_to_str(&mpbuf, sz, &msg_type);
          
          if(ngx_strmatch(&msg_type, "msg")) {
            assert(array_sz == 9);
            if(chanhead != NULL && cmp_to_msg(&cmp, &msg, &buf)) {
              //ngx_log_error(NGX_LOG_WARN, ngx_cycle->log, 0, "got msg %V", msgid_to_str(&msg));
              nchan_store_publish_generic(&chanhead->id, &msg, 0, NULL);
            }
            else {
              ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "nchan: thought there'd be a channel id around for msg");
            }
          }
          else if(ngx_strmatch(&msg_type, "ch+msg")) {
            assert(array_sz == 10);
            if(cmp_read_str_size(&cmp, &sz)) {
              fwd_buf_to_str(&mpbuf, sz, &chid);
              if(cmp_to_msg(&cmp, &msg, &buf) == false) {
                ERR("couldn't parse msgpacked message from redis");
                return;
              }
              nchan_store_publish_generic(&chid, &msg, 0, NULL);
            }
            else {
              cmp_err(&cmp);
            }
          }
          else if(ngx_strmatch(&msg_type, "msgkey")) {
            assert(array_sz == 4);
            if(chanhead != NULL) {
              uint64_t              msgtag;
              nchan_msg_id_t        msgid;
              
              if(!cmp_read_uinteger(&cmp, (uint64_t *)&msgid.time)) {
                cmp_err(&cmp);
                return;
              }
              
              if(!cmp_read_uinteger(&cmp, &msgtag)) {
                cmp_err(&cmp);
                return;
              }
              else {
                msgid.tag.fixed[0] = msgtag;
                msgid.tagactive = 0;
                msgid.tagcount = 1;
              }
              
              if(cmp_to_str(&cmp, &msg_redis_hash_key)) {
                get_msg_from_msgkey(&chanhead->id, &msgid, &msg_redis_hash_key);
              }
            }
            else {
              ERR("nchan: thought there'd be a channel id around for msgkey");
            }
          }
          else if(ngx_strmatch(&msg_type, "ch+msgkey")) {
            uint64_t              msgtag;
            nchan_msg_id_t        msgid;
            assert(array_sz == 5);
            if(! cmp_to_str(&cmp, &chid)) {
              return;
            }
            if(!cmp_read_uinteger(&cmp, (uint64_t *)&msgid.time)) {
              cmp_err(&cmp);
              return;
            }
            if(!cmp_read_uinteger(&cmp, &msgtag)) {
              cmp_err(&cmp);
              return;
            }
            else {
              msgid.tag.fixed[0] = msgtag;
              msgid.tagactive = 0;
              msgid.tagcount = 1;
            }
            
            if(cmp_to_str(&cmp, &msg_redis_hash_key)) {
              get_msg_from_msgkey(&chid, &msgid, &msg_redis_hash_key);
            }
          }
          else if(ngx_strmatch(&msg_type, "alert") && array_sz > 1) {
            ngx_str_t    alerttype;
            
            if(!cmp_to_str(&cmp, &alerttype)) {
              return;
            }
            
            if(ngx_strmatch(&alerttype, "delete channel") && array_sz > 2) {
              if(cmp_to_str(&cmp, &chid)) {
                nchan_store_publish_generic(&chid, NULL, NGX_HTTP_GONE, &NCHAN_HTTP_STATUS_410);
              }
              else {
                ERR("nchan: unexpected \"delete channel\" msgpack message from redis");
              }
            }
            else if(ngx_strmatch(&alerttype, "unsub one") && array_sz > 3) {
              if(cmp_to_str(&cmp, &chid)) {
                cmp_to_str(&cmp, &chid);
                cmp_read_uinteger(&cmp, (uint64_t *)&subscriber_id);
                //TODO
              }
              ERR("unsub one not yet implemented");
              assert(0);
            }
            else if(ngx_strmatch(&alerttype, "unsub all") && array_sz > 1) {
              if(cmp_to_str(&cmp, &chid)) {
                nchan_store_publish_generic(&chid, NULL, NGX_HTTP_CONFLICT, &NCHAN_HTTP_STATUS_409);
              }
            }
            else if(ngx_strmatch(&alerttype, "unsub all except")) {
              if(cmp_to_str(&cmp, &chid)) {
                cmp_read_uinteger(&cmp, (uint64_t *)&subscriber_id);
                //TODO
              }
              ERR("unsub all except not yet  implemented");
              assert(0);
            }
            else {
              ERR("nchan: unexpected msgpack alert from redis");
              assert(0);
            }
          }
          else {
            ERR("nchan: unexpected msgpack message from redis");
            assert(0);
          }
        }
        else {
          ERR("nchan: unexpected msgpack object from redis");
          assert(0);
        }
      }
      else {
        ERR("nchan: invalid msgpack message from redis: %s", cmp_strerror(&cmp));
        assert(0);
      }
    }
    else { //not a string
      redisEchoCallback(c, el, NULL);
    }
  }

  else if(CHECK_REPLY_ARRAY_MIN_SIZE(reply, 3)
    && CHECK_REPLY_STRVAL(reply->element[0], "subscribe")
    && CHECK_REPLY_STR(reply->element[1])
    && CHECK_REPLY_INT(reply->element[2])) {

    if(chanhead != NULL){
      switch(chanhead->status) {
        case NOTREADY:
          chanhead->status = READY;
          chanhead->spooler.fn->handle_channel_status_change(&chanhead->spooler);
          
          //ngx_log_error(NGX_LOG_WARN, ngx_cycle->log, 0, "REDIS: PUB/SUB subscribed to %s, chanhead %p now READY.", reply->element[1]->str, chanhead);
          break;
        case READY:
          ERR("REDIS: PUB/SUB already subscribed to %s, chanhead %p (id %V) already READY.", reply->element[1]->str, chanhead, &chanhead->id);
          break;
        case INACTIVE:
          ERR("REDIS: PUB/SUB already unsubscribed from %s, chanhead %p (id %V) INACTIVE.", reply->element[1]->str, chanhead, &chanhead->id);
          break;
        default:
          ERR("REDIS: PUB/SUB really unexpected chanhead status %i", chanhead->status);
          assert(0);
          //not sposed to happen
      }
    }
    
    DBG("REDIS: PUB/SUB subscribed to %s (%i total)", reply->element[1]->str, reply->element[2]->integer);
  }
  else if(CHECK_REPLY_ARRAY_MIN_SIZE(reply, 3)
    && CHECK_REPLY_STRVAL(reply->element[0], "unsubscribe")
    && CHECK_REPLY_STR(reply->element[1])
    && CHECK_REPLY_INT(reply->element[2])) {

    DBG("REDIS: PUB/SUB unsubscribed from %s (%i total)", reply->element[1]->str, reply->element[2]->integer);
  }
  
  else {
    ngx_log_error(NGX_LOG_WARN, ngx_cycle->log, 0, "no message, something else");
    redisEchoCallback(c,r,privdata);
  }
}

static ngx_int_t redis_subscriber_register(nchan_store_channel_head_t *chanhead, subscriber_t *sub);
static ngx_int_t redis_subscriber_unregister(ngx_str_t *channel_id, subscriber_t *sub, uint8_t shutting_down);
static void spooler_add_handler(channel_spooler_t *spl, subscriber_t *sub, void *privdata) {
  nchan_store_channel_head_t *head = (nchan_store_channel_head_t *)privdata;
  head->sub_count++;
  if(sub->type == INTERNAL) {
    head->internal_sub_count++;
  }
  redis_subscriber_register(head, sub);
}

static void spooler_dequeue_handler(channel_spooler_t *spl, subscriber_t *sub, void *privdata) {
  //need individual subscriber
  //TODO
  nchan_store_channel_head_t *head = (nchan_store_channel_head_t *)privdata;
  
  head->sub_count--;
  if(sub->type == INTERNAL) {
    head->internal_sub_count--;
  }
  
  if(rdt.status == CONNECTED) {
    redis_subscriber_unregister(&head->id, sub, head->shutting_down);
  }
  
  if(head->sub_count == 0 && head->fetching_message_count == 0) {
    chanhead_gc_add(head, "sub count == 0 and fetching_message_count == 0 after spooler dequeue");
  }
  
}

static void spooler_bulk_post_subscribe_handler(channel_spooler_t *spl, int n, void *d) {
  //nothing. 
}

void spooler_get_message_start_handler(channel_spooler_t *spl, void *pd) {
  ((nchan_store_channel_head_t *)pd)->fetching_message_count++;
}

void spooler_get_message_finish_handler(channel_spooler_t *spl, void *pd) {
  ((nchan_store_channel_head_t *)pd)->fetching_message_count--;
  assert(((nchan_store_channel_head_t *)pd)->fetching_message_count >= 0);
}

static ngx_int_t start_chanhead_spooler(nchan_store_channel_head_t *head) {
  static channel_spooler_handlers_t handlers = {
    spooler_add_handler,
    spooler_dequeue_handler,
    NULL,
    spooler_bulk_post_subscribe_handler,
    spooler_get_message_start_handler,
    spooler_get_message_finish_handler
  };
  start_spooler(&head->spooler, &head->id, &head->status, &nchan_store_redis, FETCH, &handlers, head);
  return NGX_OK;
}

static void redis_subscriber_register_callback(redisAsyncContext *c, void *vr, void *privdata);

typedef struct {
  nchan_store_channel_head_t *chanhead;
  unsigned             generation;
  subscriber_t        *sub;
} redis_subscriber_register_t;

static ngx_int_t redis_subscriber_register(nchan_store_channel_head_t *chanhead, subscriber_t *sub) {
  redis_subscriber_register_t *sdata=NULL;
  
  if((sdata = ngx_alloc(sizeof(*sdata), ngx_cycle->log)) == NULL) {
    ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "No memory for sdata. Part IV, subparagraph 12 of the Cryptic Error Series.");
    return NGX_ERROR;
  }
  sdata->chanhead = chanhead;
  sdata->generation = chanhead->generation;
  sdata->sub = sub;
  
  sub->fn->reserve(sub);
  
  //input: keys: [], values: [channel_id, subscriber_id, active_ttl]
  //  'subscriber_id' can be '-' for new id, or an existing id
  //  'active_ttl' is channel ttl with non-zero subscribers. -1 to persist, >0 ttl in sec
  //output: subscriber_id, num_current_subscribers, next_keepalive_time
  redis_command(&redis_subscriber_register_callback, sdata, "EVALSHA %s 0 %b - %i", redis_lua_scripts.subscriber_register.hash, STR(&chanhead->id), REDIS_CHANNEL_EMPTY_BUT_SUBSCRIBED_TTL);
  
  return NGX_OK;
}

static void redis_subscriber_register_callback(redisAsyncContext *c, void *vr, void *privdata) {
  redis_subscriber_register_t *sdata= (redis_subscriber_register_t *) privdata;
  redisReply                  *reply = (redisReply *)vr;
  int                          keepalive_ttl;
  sdata->sub->fn->release(sdata->sub, 0);
  
  if (reply == NULL || reply->type == REDIS_REPLY_ERROR) {
    redisEchoCallback(c,reply,privdata);
    return;
  }
  
  if ( !CHECK_REPLY_ARRAY_MIN_SIZE(reply, 3) || !CHECK_REPLY_INT(reply->element[1]) || !CHECK_REPLY_INT(reply->element[2])) {
    //no good
    redisEchoCallback(c,reply,privdata);
    return;
  }
  if(sdata->generation == sdata->chanhead->generation) {
    //is the subscriber     
    //TODO: set subscriber id
    //sdata->sub->id = reply->element[1]->integer;
  }
  keepalive_ttl = reply->element[2]->integer;
  if(keepalive_ttl > 0) {
    if(!sdata->chanhead->keepalive_timer.timer_set) {
      ngx_add_timer(&sdata->chanhead->keepalive_timer, keepalive_ttl * 1000);
    }
  }
  ngx_free(sdata);
}


static ngx_int_t redis_subscriber_unregister(ngx_str_t *channel_id, subscriber_t *sub, uint8_t shutting_down) {
  nchan_loc_conf_t  *cf = sub->cf;
  //input: keys: [], values: [channel_id, subscriber_id, empty_ttl]
  // 'subscriber_id' is an existing id
  // 'empty_ttl' is channel ttl when without subscribers. 0 to delete immediately, -1 to persist, >0 ttl in sec
  //output: subscriber_id, num_current_subscribers
  
  if(!shutting_down) {
    redis_command(&redisCheckErrorCallback, NULL, "EVALSHA %s 0 %b %i %i", redis_lua_scripts.subscriber_unregister.hash, STR(channel_id), 0/*TODO: sub->id*/, cf->channel_timeout);
  }
  else {
    redis_sync_command("EVALSHA %s 0 %b %i %i", redis_lua_scripts.subscriber_unregister.hash, STR(channel_id), 0/*TODO: sub->id*/, cf->channel_timeout);
  }
  return NGX_OK;
}

static void redisChannelKeepaliveCallback(redisAsyncContext *c, void *vr, void *privdata) {
  nchan_store_channel_head_t  *head = (nchan_store_channel_head_t *)privdata;
  redisReply                  *reply = (redisReply *)vr;
  
  if(redisReplyOk(c, vr)) {
    assert(CHECK_REPLY_INT(reply));
    
    //reply->integer == -1 means "let it disappear" (see channel_keepalive.lua)
    
    if(reply->integer != -1 && !head->keepalive_timer.timer_set) {
      ngx_add_timer(&head->keepalive_timer, reply->integer * 1000);
    }
  }
  
}

static void redis_channel_keepalive_timer_handler(ngx_event_t *ev) {
  nchan_store_channel_head_t   *head = ev->data;
  if(ev->timedout) {
    ev->timedout=0;
    redis_command(&redisChannelKeepaliveCallback, head, "EVALSHA %s 0 %b %i", redis_lua_scripts.channel_keepalive.hash, STR(&head->id), REDIS_CHANNEL_EMPTY_BUT_SUBSCRIBED_TTL);
  }
}

static nchan_store_channel_head_t *chanhead_redis_create(ngx_str_t *channel_id) {
  nchan_store_channel_head_t *head;
  
  head=ngx_calloc(sizeof(*head) + sizeof(u_char)*(channel_id->len), ngx_cycle->log);
  if(head==NULL) {
    ngx_log_error(NGX_LOG_WARN, ngx_cycle->log, 0, "can't allocate memory for (new) channel subscriber head");
    return NULL;
  }
  head->id.len = channel_id->len;
  head->id.data = (u_char *)&head[1];
  ngx_memcpy(head->id.data, channel_id->data, channel_id->len);
  head->sub_count=0;
  head->fetching_message_count=0;
  head->redis_subscriber_privdata = NULL;
  head->status = NOTREADY;
  head->generation = 0;
  head->last_msgid.time=0;
  head->last_msgid.tag.fixed[0]=0;
  head->last_msgid.tagcount = 1;
  head->last_msgid.tagactive = 0;
  head->shutting_down = 0;
  
  head->in_gc_queue = 0;

  if(head->id.len >= 5 && ngx_strncmp(head->id.data, "meta/", 5) == 0) {
    head->meta = 1;
  }
  else {
    head->meta = 0;
  }
  
  head->spooler.running=0;
  start_chanhead_spooler(head);
  if(head->meta) {
    head->spooler.publish_events = 0;
  }

  ngx_memzero(&head->keepalive_timer, sizeof(head->keepalive_timer));
  nchan_init_timer(&head->keepalive_timer, redis_channel_keepalive_timer_handler, head);
  
  DBG("SUBSCRIBING to channel:pubsub:%V", channel_id);
  redis_subscriber_command(redis_subscriber_callback, head, "SUBSCRIBE channel:pubsub:%b", STR(channel_id));
  CHANNEL_HASH_ADD(head);
  
  return head;
}

static nchan_store_channel_head_t * nchan_store_get_chanhead(ngx_str_t *channel_id) {
  nchan_store_channel_head_t     *head;
  
  CHANNEL_HASH_FIND(channel_id, head);
  if(head==NULL) {
    head = chanhead_redis_create(channel_id);
  }
  if(head == NULL) {
    ERR("can't create chanhead for redis store");
    return NULL;
  }
  
  if (head->status == INACTIVE) { //recycled chanhead
    chanhead_gc_withdraw(head);
    head->status = READY;
  }

  if(!head->spooler.running) {
    DBG("Spooler for channel %p %V wasn't running. start it.", head, &head->id);
    start_chanhead_spooler(head);
  }
  
  return head;
}

static ngx_int_t chanhead_gc_add(nchan_store_channel_head_t *head, const char *reason) {
  
  DBG("Chanhead gc add %p %V: %s", head, &head->id, reason);
  
  if(head->in_gc_queue != 1) {
    assert(head->status != INACTIVE);
    head->status = INACTIVE;
    head->gc_time = ngx_time() + NCHAN_CHANHEAD_EXPIRE_SEC;
    head->in_gc_queue = 1;
    nchan_reaper_add(&rdt.chanhead_reaper, head);
    DBG("gc_add chanhead %V", &head->id);
  }
  else {
    ERR("gc_add chanhead %V: already added", &head->id);
  }

  return NGX_OK;
}


static ngx_int_t chanhead_gc_withdraw(nchan_store_channel_head_t *chanhead) {
  //remove from cleanup list if we're there
  DBG("gc_withdraw chanhead %V", &chanhead->id);
  if(chanhead->in_gc_queue == 1) {
    assert(chanhead->status == INACTIVE);
    nchan_reaper_withdraw(&rdt.chanhead_reaper, chanhead);
    chanhead->in_gc_queue = 0;
  }
  else {
    DBG("gc_withdraw chanhead %p (%V), but already not in gc reaper", chanhead, &chanhead->id);
  }
  return NGX_OK;
}

static ngx_int_t nchan_store_publish_generic(ngx_str_t *channel_id, nchan_msg_t *msg, ngx_int_t status_code, const ngx_str_t *status_line){
  nchan_store_channel_head_t        *head;
  ngx_int_t                   ret;
  //redis_channel_head_cleanup_t *hcln;
  
  head = nchan_store_get_chanhead(channel_id);
  
  if(head->sub_count > 0) {
    if(msg) {
      assert(msg->id.tagcount == 1);
      head->last_msgid.time = msg->id.time;
      head->last_msgid.tag.fixed[0] = msg->id.tag.fixed[0];
      head->last_msgid.tagcount = 1;
      head->last_msgid.tagactive = 0;
      
      head->spooler.fn->respond_message(&head->spooler, msg);
    }
    else {
      head->spooler.fn->broadcast_status(&head->spooler, status_code, status_line);
    }
    ret= NGX_OK;
  }
  else {
    ret= NCHAN_MESSAGE_QUEUED;
  }
  return ret;
}

static ngx_int_t redis_array_to_channel(redisReply *r, nchan_channel_t *ch) {
  if ( CHECK_REPLY_ARRAY_MIN_SIZE(r, 4)
    && CHECK_REPLY_INT(r->element[0])
    && CHECK_REPLY_INT(r->element[1])
    && CHECK_REPLY_INT(r->element[2]) ) {
    
    //channel info
    ch->expires = ngx_time() + r->element[0]->integer;
    ch->last_seen = r->element[1]->integer;
    ch->subscribers = r->element[2]->integer;
    ch->messages = r->element[3]->integer;
    
    //no id?..
    ch->id.len=0;
    ch->id.data=NULL;
    
    //last message id
    if( CHECK_REPLY_ARRAY_MIN_SIZE(r, 5)
      && CHECK_REPLY_STR(r->element[4])) {
      
      ngx_str_t      msgid;
      msgid.data = (u_char *)r->element[4]->str;
      msgid.len = r->element[4]->len;
      
      if(msgid.len > 0 && nchan_parse_compound_msgid(&ch->last_published_msg_id, &msgid, 1) != NGX_OK) {
        ERR("failed to parse last-msgid %V from redis", &msgid);
      }
      else {
        nchan_msg_id_t  zeroid = NCHAN_OLDEST_MSGID;
        ch->last_published_msg_id = zeroid;
      }
    }
    
    //queued messages
    if( CHECK_REPLY_ARRAY_MIN_SIZE(r, 6)
      && CHECK_REPLY_INT(r->element[5])) {
      
      ch->messages = r->element[5]->integer;
    }
    
    return NGX_OK;
  }
  else if(CHECK_REPLY_NIL(r)) {
    return NGX_DECLINED;
  }
  else {
    return NGX_ERROR;
  }
}

typedef struct {
  ngx_msec_t           t;
  char                *name;
  ngx_str_t           *channel_id;
  callback_pt          callback;
  void                *privdata;
} redis_channel_callback_data_t;

static void redisChannelInfoCallback(redisAsyncContext *c, void *r, void *privdata) {
  redisReply *reply=r;
  redis_channel_callback_data_t *d=(redis_channel_callback_data_t *)privdata;
  nchan_channel_t channel;
  ngx_memzero(&channel, sizeof(channel)); // for ddebugging. this should be removed later.
  
  log_redis_reply(d->name, d->t);
  
  if(reply) {
    switch(redis_array_to_channel(reply, &channel)) {
      case NGX_OK:
        d->callback(NGX_OK, &channel, d->privdata);
        break;
      case NGX_DECLINED: //not found
        d->callback(NGX_OK, NULL, d->privdata);
        break;
      case NGX_ERROR:
      default:
        d->callback(NGX_ERROR, NULL, d->privdata);
        redisEchoCallback(c, r, privdata);
    }
  }
  ngx_free(d);
}

static ngx_int_t nchan_store_delete_channel(ngx_str_t *channel_id, callback_pt callback, void *privdata) {
  redis_channel_callback_data_t *d;
  if((d=ngx_alloc(sizeof(*d), ngx_cycle->log))==NULL) {
    ngx_log_error(NGX_LOG_WARN, ngx_cycle->log, 0, "Failed to allocate memory for some callback data");
    return NGX_ERROR;
  }
  d->t = ngx_current_msec;
  d->name = "delete";
  d->channel_id = channel_id;
  d->callback = callback;
  d->privdata = privdata;
  
  redis_command(&redisChannelInfoCallback, d, "EVALSHA %s 0 %b", redis_lua_scripts.delete.hash, STR(channel_id));

  return NGX_OK;
}

static ngx_int_t nchan_store_find_channel(ngx_str_t *channel_id, nchan_loc_conf_t *cf, callback_pt callback, void *privdata) {
  redis_channel_callback_data_t *d;
  if((d=ngx_alloc(sizeof(*d), ngx_cycle->log))==NULL) {
    ngx_log_error(NGX_LOG_WARN, ngx_cycle->log, 0, "Failed to allocate memory for some callback data");
    return NGX_ERROR;
  }
  d->t = ngx_current_msec;
  d->name = "find_channel";
  d->channel_id = channel_id;
  d->callback = callback;
  d->privdata = privdata;
  
  redis_command(&redisChannelInfoCallback, d, "EVALSHA %s 0 %b", redis_lua_scripts.find_channel.hash, STR(channel_id));
  
  return NGX_OK;
}

typedef struct {
  nchan_msg_t   msg;
  ngx_buf_t     buf;
} getmessage_blob_t;

static ngx_int_t msg_from_redis_get_message_reply(nchan_msg_t *msg, ngx_buf_t *buf, redisReply *r, uint16_t offset) {
  
  redisReply         **els = r->element;
  size_t               content_type_len = 0, es_event_len = 0;
  ngx_int_t            time_int, ttl;
  
  if(CHECK_REPLY_ARRAY_MIN_SIZE(r, offset + 7)
   && CHECK_REPLY_INT(els[offset])            //msg TTL
   && CHECK_REPLY_INT_OR_STR(els[offset+1])   //id - time
   && CHECK_REPLY_INT_OR_STR(els[offset+2])   //id - tag
   && CHECK_REPLY_INT_OR_STR(els[offset+3])   //prev_id - time
   && CHECK_REPLY_INT_OR_STR(els[offset+4])   //prev_id - tag
   && CHECK_REPLY_STR(els[offset+5])   //message
   && CHECK_REPLY_STR(els[offset+6])   //content-type
   && CHECK_REPLY_STR(els[offset+7])){ //eventsource event
     
    content_type_len=els[offset+6]->len;
    es_event_len = els[offset+7]->len;
    
    ngx_memzero(msg, sizeof(*msg));
    ngx_memzero(buf, sizeof(*buf));
    //set up message buffer;
    msg->buf = buf;
    
    buf->start = buf->pos = (u_char *)els[offset+5]->str;
    buf->end = buf->last = buf->start + els[offset+5]->len;
    buf->memory = 1;
    buf->last_buf = 1;
    buf->last_in_chain = 1;
    
    if(redisReply_to_int(els[offset], &ttl) != NGX_OK) {
      ERR("invalid ttl integer value is msg response from redis");
      return NGX_ERROR;
    }
    assert(ttl > 0);
    msg->expires = ngx_time() + ttl;
    
    if(content_type_len > 0) {
      msg->content_type.len=content_type_len;
      msg->content_type.data=(u_char *)els[offset+6]->str;
    }
    
    if(es_event_len > 0) {
      msg->eventsource_event.len=es_event_len;
      msg->eventsource_event.data=(u_char *)els[offset+7]->str;
    }
    
    if(redisReply_to_int(els[offset+1], &time_int) == NGX_OK) {
      msg->id.time = time_int;
    }
    else {
      msg->id.time = 0;
      ERR("invalid msg time from redis");
    }
    
    redisReply_to_int(els[offset+2], (ngx_int_t *)&msg->id.tag.fixed[0]); // tag is a uint, meh.
    msg->id.tagcount = 1;
    msg->id.tagactive = 0;
    
    redisReply_to_int(els[offset+3], &time_int);
    msg->prev_id.time = time_int;
    redisReply_to_int(els[offset+4], (ngx_int_t *)&msg->prev_id.tag.fixed[0]);
    msg->prev_id.tagcount = 1;
    msg->prev_id.tagactive = 0;
    
    return NGX_OK;
  }
  else {
    ERR("nchan: invalid message redis reply");
    return NGX_ERROR;
  }
}

typedef struct {
  ngx_msec_t              t;
  char                   *name;
  ngx_str_t              *channel_id;
  nchan_msg_id_t         *msg_id;
  callback_pt             callback;
  void                   *privdata;
} redis_get_message_data_t;

static void redis_get_message_callback(redisAsyncContext *c, void *r, void *privdata) {
  redisReply                *reply= r;
  redis_get_message_data_t  *d= (redis_get_message_data_t *)privdata;
  nchan_msg_t                msg;
  ngx_buf_t                  msgbuf;
  
  if(d == NULL) {
    ERR("redis_get_mesage_callback has NULL userdata");
    return;
  }
  
  log_redis_reply(d->name, d->t);
  
  //output: result_code, msg_time, msg_tag, message, content_type,  channel-subscriber-count
  // result_code can be: 200 - ok, 403 - channel not found, 404 - not found, 410 - gone, 418 - not yet available
  
  if (!reply || !CHECK_REPLY_ARRAY_MIN_SIZE(reply, 1) || !CHECK_REPLY_INT(reply->element[0]) ) {
    //no good
    ngx_free(d);
    return;
  }
  
  switch(reply->element[0]->integer) {
    case 200: //ok
      if(msg_from_redis_get_message_reply(&msg, &msgbuf, reply, 1) == NGX_OK) {
        d->callback(MSG_FOUND, &msg, d->privdata);
      }
      break;
    case 403: //channel not found
    case 404: //not found
      d->callback(MSG_NOTFOUND, NULL, d->privdata);
      break;
    case 410: //gone
      d->callback(MSG_EXPIRED, NULL, d->privdata);
      break;
    case 418: //not yet available
      d->callback(MSG_EXPECTED, NULL, d->privdata);
      break;
  }
  
  ngx_free(d);
}

static ngx_int_t nchan_store_async_get_message(ngx_str_t *channel_id, nchan_msg_id_t *msg_id, callback_pt callback, void *privdata) {
  redis_get_message_data_t           *d=NULL;
  if(callback==NULL) {
    ngx_log_error(NGX_LOG_WARN, ngx_cycle->log, 0, "no callback given for async get_message. someone's using the API wrong!");
    return NGX_ERROR;
  }
  if((d=ngx_alloc(sizeof(*d), ngx_cycle->log))==NULL) {
    ngx_log_error(NGX_LOG_WARN, ngx_cycle->log, 0, "Failed to allocate memory for some callback data");
    return NGX_ERROR;
  }
  d->channel_id = channel_id;
  d->msg_id = msg_id;
  d->callback = callback;
  d->privdata = privdata;
  
  d->t = ngx_current_msec;
  d->name = "get_message";
  
  //input:  keys: [], values: [channel_id, msg_time, msg_tag, no_msgid_order, create_channel_ttl, subscriber_channel]
  //subscriber channel is not given, because we don't care to subscribe
  assert(msg_id->tagcount == 1);
  redis_command(&redis_get_message_callback, (void *)d, "EVALSHA %s 0 %b %i %i %s", redis_lua_scripts.get_message.hash, STR(channel_id), msg_id->time, msg_id->tag.fixed[0], "FILO", 0);
  return NGX_OK; //async only now!
}

//initialization
static ngx_int_t nchan_store_init_module(ngx_cycle_t *cycle) {
  ngx_core_conf_t                *ccf = (ngx_core_conf_t *) ngx_get_conf(cycle->conf_ctx, ngx_core_module);
  nchan_worker_processes = ccf->worker_processes;
  return NGX_OK;
}

static ngx_int_t nchan_store_init_postconfig(ngx_conf_t *cf) {
  nchan_main_conf_t     *conf = ngx_http_conf_get_module_main_conf(cf, ngx_nchan_module);
  
  if(conf->redis_url.len == 0) {
    ngx_memcpy(&conf->redis_url, &REDIS_DEFAULT_URL, sizeof(REDIS_DEFAULT_URL));
  }
  
  if(conf->redis_ping_interval==NGX_CONF_UNSET) {
    conf->redis_ping_interval=REDIS_DEFAULT_PING_INTERVAL_TIME;
  }
  rdt.connect_url = &conf->redis_url;
  rdt.ping_interval = conf->redis_ping_interval;
  
  return NGX_OK;
}

static void nchan_store_create_main_conf(ngx_conf_t *cf, nchan_main_conf_t *mcf) {
  mcf->redis_ping_interval=NGX_CONF_UNSET;
}

void redis_store_prepare_to_exit_worker() {
  nchan_store_channel_head_t *cur, *tmp;
  HASH_ITER(hh, rdt.subhash, cur, tmp) {
    cur->shutting_down = 1;
  }
}

static void nchan_store_exit_worker(ngx_cycle_t *cycle) {
  nchan_store_channel_head_t *cur, *tmp;
  
  rdt.shutting_down = 1;
  
  callback_chain_t *ccur, *cnext;  
  for(ccur = rdt.on_connected; ccur != NULL; ccur = cnext) {
    cnext = ccur->next;
    ngx_free(ccur);
  }
  rdt.on_connected = NULL;
  
  HASH_ITER(hh, rdt.subhash, cur, tmp) {
    cur->shutting_down = 1;
    if(cur->in_gc_queue != 1) {
      chanhead_gc_add(cur, "exit worker");
    }
  }
  
  nchan_exit_notice_about_remaining_things("redis channel", "", rdt.chanhead_reaper.count);
  nchan_reaper_stop(&rdt.chanhead_reaper);
  
  if(rdt.ctx)
    redis_nginx_force_close_context(&rdt.ctx);
  if(rdt.sub_ctx)
    redis_nginx_force_close_context(&rdt.sub_ctx);
  if(rdt.sync_ctx)
    redisFree(rdt.sync_ctx);
  
  ngx_free(rdt.connect_params);
}

static void nchan_store_exit_master(ngx_cycle_t *cycle) {
  //destroy channel tree in shared memory
  //nchan_walk_rbtree(nchan_movezig_channel_locked, nchan_shm_zone);
}

typedef struct {
  ngx_str_t                   *channel_id;
  subscriber_t                *sub;
  unsigned                     allocd:1;
} redis_subscribe_data_t;

static ngx_int_t nchan_store_subscribe_continued(redis_subscribe_data_t *d);

static ngx_int_t subscribe_existing_channel_callback(ngx_int_t status, void *ch, void *d) {
  nchan_channel_t              *channel = (nchan_channel_t *)ch;
  redis_subscribe_data_t       *data = (redis_subscribe_data_t *)d;
  
  if(channel == NULL) {
    data->sub->fn->respond_status(data->sub, NGX_HTTP_FORBIDDEN, NULL);
    data->sub->fn->release(data->sub, 0);
  }
  else {
    nchan_store_subscribe_continued(d);
  }
  assert(data->allocd);
  ngx_free(data);
  
  return NGX_OK;
}

static ngx_int_t nchan_store_subscribe(ngx_str_t *channel_id, subscriber_t *sub) {
  redis_subscribe_data_t        d_data;
  redis_subscribe_data_t       *d = NULL;

  assert(sub->last_msgid.tagcount == 1);
  
  if(!sub->cf->subscribe_only_existing_channel) {
    d_data.allocd = 0;
    d_data.channel_id = channel_id;
    d_data.sub = sub;
    nchan_store_subscribe_continued(&d_data);
  }
  else {
    if((d=ngx_alloc(sizeof(*d) + sizeof(ngx_str_t) + channel_id->len, ngx_cycle->log))==NULL) {
      ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "can't allocate redis get_message callback data");
      return NGX_ERROR;
    }
    d->allocd = 1;
    d->channel_id=(ngx_str_t *)&d[1];
    d->channel_id->len = channel_id->len;
    d->channel_id->data = (u_char *)&(d->channel_id)[1];
    ngx_memcpy(d->channel_id->data, channel_id->data, channel_id->len);
    d->sub = sub;
    nchan_store_find_channel(channel_id, sub->cf, subscribe_existing_channel_callback, d);
  }

  return NGX_OK;
}

static ngx_int_t nchan_store_subscribe_continued(redis_subscribe_data_t *d) {
  //nchan_loc_conf_t             *cf = d->sub->cf;
  nchan_store_channel_head_t   *ch;
  //ngx_int_t                     create_channel_ttl = cf->subscribe_only_existing_channel==1 ? 0 : cf->channel_timeout;
  
  ch = nchan_store_get_chanhead(d->channel_id);
  
  assert(ch != NULL);
  
  ch->spooler.fn->add(&ch->spooler, d->sub);
  //redisAsyncCommand(rds_ctx(), &redis_getmessage_callback, (void *)d, "EVALSHA %s 0 %b %i %i %s %i", redis_lua_scripts.get_message.hash, STR(d->channel_id), d->msg_id->time, d->msg_id->tag[0], "FILO", create_channel_ttl);
  return NGX_OK;
}

static ngx_str_t * nchan_store_etag_from_message(nchan_msg_t *msg, ngx_pool_t *pool){
  ngx_str_t       *etag = NULL;
  if(pool!=NULL && (etag = ngx_palloc(pool, sizeof(*etag) + NGX_INT_T_LEN))==NULL) {
    ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "nchan: unable to allocate memory for Etag header in pool");
    return NULL;
  }
  else if(pool==NULL && (etag = ngx_alloc(sizeof(*etag) + NGX_INT_T_LEN, ngx_cycle->log))==NULL) {
    ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "nchan: unable to allocate memory for Etag header");
    return NULL;
  }
  etag->data = (u_char *)(etag+1);
  etag->len = ngx_sprintf(etag->data,"%i", msg->id.tag.fixed[0])- etag->data;
  return etag;
}

static ngx_str_t * nchan_store_content_type_from_message(nchan_msg_t *msg, ngx_pool_t *pool){
  ngx_str_t        *content_type = NULL;
  if(pool != NULL && (content_type = ngx_palloc(pool, sizeof(*content_type) + msg->content_type.len))==NULL) {
    ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "nchan: unable to allocate memory for Content Type header in pool");
    return NULL;
  }
  else if(pool == NULL && (content_type = ngx_alloc(sizeof(*content_type) + msg->content_type.len, ngx_cycle->log))==NULL) {
    ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "nchan: unable to allocate memory for Content Type header");
    return NULL;
  }
  content_type->data = (u_char *)(content_type+1);
  content_type->len = msg->content_type.len;
  ngx_memcpy(content_type->data, msg->content_type.data, content_type->len);
  return content_type;
}

typedef struct {
  ngx_msec_t            t;
  char                 *name;
  ngx_str_t            *channel_id;
  time_t                msg_time;
  nchan_msg_t          *msg;
  ngx_int_t             msglen;
  callback_pt           callback;
  void                 *privdata;
} redis_publish_callback_data_t;

static void redisPublishCallback(redisAsyncContext *, void *, void *);

static ngx_int_t nchan_store_publish_message(ngx_str_t *channel_id, nchan_msg_t *msg, nchan_loc_conf_t *cf, callback_pt callback, void *privdata) {
  redis_publish_callback_data_t  *d=NULL;
  u_char                         *msgstart;
  size_t                          msglen;
  ngx_int_t                       mmapped=0;
  ngx_buf_t                      *buf;
  
  assert(callback != NULL);

  if((d=ngx_calloc(sizeof(*d), ngx_cycle->log))==NULL) { //todo: allocate in request pool?...
    ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "can't allocate redis publish callback data");
    return callback(NGX_ERROR, NULL, privdata);
  }

  if(msg->id.time==0) {
    msg->id.time = ngx_time();
  }
  
  d->channel_id=channel_id;
  d->callback=callback;
  d->privdata=privdata;
  d->msg_time=msg->id.time;
  
  
  assert(msg->id.tagcount == 1);
  //nchan_store_publish_generic(channel_id, msg, 0, NULL);
  
  //input:  keys: [], values: [channel_id, time, message, content_type, msg_ttl, max_messages]
  //output: message_tag, channel_hash
  buf = msg->buf;
  if(ngx_buf_in_memory(buf)) {
    msgstart = buf->pos;
    msglen = buf->last - msgstart;
  }
  else { //in a file
    ngx_fd_t fd = buf->file->fd == NGX_INVALID_FILE ? nchan_fdcache_get(&buf->file->name) : buf->file->fd;
    
    msglen = buf->file_last - buf->file_pos;
    msgstart = mmap(NULL, msglen, PROT_READ, MAP_SHARED, fd, 0);
    if (msgstart != MAP_FAILED) {
      mmapped = 1;
    }
    else {
      msgstart = NULL;
      msglen = 0;
      ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, ngx_errno, "Redis store: Couldn't mmap file %V", &buf->file->name);
    }
  }
  d->msglen = msglen;
  
  d->t = ngx_current_msec;
  d->name = "publish";
  
  redis_command(&redisPublishCallback, (void *)d, "EVALSHA %s 0 %b %i %b %b %b %i %i", redis_lua_scripts.publish.hash, STR(channel_id), msg->id.time, msgstart, msglen, STR(&(msg->content_type)), STR(&(msg->eventsource_event)), cf->buffer_timeout, cf->max_messages);
  if(mmapped && munmap(msgstart, msglen) == -1) {
    ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "munmap was a problem");
  }
  return NGX_OK;
}

static void redisPublishCallback(redisAsyncContext *c, void *r, void *privdata) {
  redis_publish_callback_data_t *d=(redis_publish_callback_data_t *)privdata;
  redisReply                    *reply=r;
  redisReply                    *cur;
  //nchan_msg_id_t                 msg_id;
  nchan_channel_t                ch;
  ngx_memzero(&ch, sizeof(ch)); //for debugging basically. should be removed in the future and zeroed as-needed

  if(reply && CHECK_REPLY_ARRAY_MIN_SIZE(reply, 2)) {
    ch.last_published_msg_id.time=d->msg_time;
    ch.last_published_msg_id.tag.fixed[0]=reply->element[0]->integer;
    ch.last_published_msg_id.tagcount = 1;
    ch.last_published_msg_id.tagactive = 0;

    cur=reply->element[1];
    switch(redis_array_to_channel(cur, &ch)) {
      case NGX_OK:
        d->callback(ch.subscribers > 0 ? NCHAN_MESSAGE_RECEIVED : NCHAN_MESSAGE_QUEUED, &ch, d->privdata);
        break;
      case NGX_DECLINED: //not found
        d->callback(NGX_OK, NULL, d->privdata);
        break;
      case NGX_ERROR:
      default:
        redisEchoCallback(c, r, privdata);
        d->callback(NGX_HTTP_INTERNAL_SERVER_ERROR, NULL, d->privdata);
    }
  }
  else {
    redisEchoCallback(c, r, privdata);
    d->callback(NGX_HTTP_INTERNAL_SERVER_ERROR, NULL, d->privdata);
  }
  ngx_free(d);
}

ngx_int_t nchan_store_redis_fakesub_add(ngx_str_t *channel_id, ngx_int_t count, uint8_t shutting_down) {
  if(!shutting_down) {
    redis_command(&redisCheckErrorCallback, NULL, "EVALSHA %s 0 %b %i", redis_lua_scripts.add_fakesub.hash, STR(channel_id), count);
  }
  else {
    redis_sync_command("EVALSHA %s 0 %b %i", redis_lua_scripts.add_fakesub.hash, STR(channel_id), count);
  }
  return NGX_OK;
}

nchan_store_t  nchan_store_redis = {
    //init
    &nchan_store_init_module,
    &nchan_store_init_worker,
    &nchan_store_init_postconfig,
    &nchan_store_create_main_conf,
    
    //shutdown
    &nchan_store_exit_worker,
    &nchan_store_exit_master,
    
    //async-friendly functions with callbacks
    &nchan_store_async_get_message, //+callback
    &nchan_store_subscribe, //+callback
    &nchan_store_publish_message, //+callback
    
    &nchan_store_delete_channel, //+callback
    &nchan_store_find_channel, //+callback
    
    //message stuff
    &nchan_store_etag_from_message,
    &nchan_store_content_type_from_message,
    
};

