/* max connections module for nginx
** october 2008, ryan dahl (ry@ndahl.us)
*/


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <ngx_http_upstream.h>
#include <assert.h>

#define DEFAULT_MAX_CONNECTIONS 2

typedef struct {
  ngx_uint_t max_connections;
  ngx_queue_t                        waiting_requests;
  ngx_array_t                       *peers; /* backend servers */
} max_connections_srv_conf_t;

typedef struct {
  ngx_queue_t queue;
  ngx_http_request_t *r;
} max_connections_waiting_t;

typedef struct {
  struct sockaddr *sockaddr;
  socklen_t socklen;
  ngx_str_t *name;

  ngx_uint_t weight;
  ngx_uint_t  max_fails;
  time_t fail_timeout;

  time_t accessed;
  ngx_uint_t down:1;

  ngx_uint_t connections;
} max_connections_peer_t;



/* forward declarations */
static char * max_connections_command (ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static void * max_connections_create_conf(ngx_conf_t *cf);

static ngx_command_t  max_connections_commands[] =
{ { ngx_string("max_connections")
  , NGX_HTTP_UPS_CONF|NGX_CONF_TAKE1
  , max_connections_command
  , 0
  , 0
  , NULL
  }
, ngx_null_command
};

static ngx_http_module_t max_connections_module_ctx =
/* preconfiguration              */ { NULL 
/* postconfiguration             */ , NULL 
/* create main configuration     */ , NULL 
/* init main configuration       */ , NULL 
/* create server configuration   */ , max_connections_create_conf 
/* merge server configuration    */ , NULL 
/* create location configuration */ , NULL 
/* merge location configuration  */ , NULL 
                                    };


ngx_module_t max_connections_module =
                        { NGX_MODULE_V1
/* module context    */ , &max_connections_module_ctx
/* module directives */ , max_connections_commands
/* module type       */ , NGX_HTTP_MODULE
/* init master       */ , NULL
/* init module       */ , NULL
/* init process      */ , NULL
/* init thread       */ , NULL
/* exit thread       */ , NULL
/* exit process      */ , NULL
/* exit master       */ , NULL
                        , NGX_MODULE_V1_PADDING
                        };

static void
max_connections_dispatch_new_connection (max_connections_srv_conf_t *maxconn_cf)
{
  if(ngx_queue_empty(&maxconn_cf->waiting_requests)) 
    return;
  ngx_queue_t *last = ngx_queue_last(&maxconn_cf->waiting_requests);
  ngx_queue_remove(last);
  max_connections_waiting_t *waiting = 
    ngx_queue_data(last, max_connections_waiting_t, queue);
  ngx_http_request_t *r = waiting->r;

  ngx_pfree(r->pool, waiting); // TODO check return value
  
  ngx_http_upstream_connect(r, r->upstream);
}

static max_connections_peer_t*
max_connections_find_open_upstream (max_connections_srv_conf_t *maxconn_cf)
{
  ngx_uint_t i, min_connections = 0, min_upstream_index = 0;
  
  max_connections_peer_t *peers = maxconn_cf->peers->elts;
  for(i = 0; i < maxconn_cf->peers->nelts; i++) {
    if(peers[i].connections <= min_connections) {
      min_connections = peers[i].connections;
      min_upstream_index = i;
    }
  }

  assert(min_connections <= maxconn_cf->max_connections);

  if(min_connections == maxconn_cf->max_connections) 
    /* no open slots */
    return NULL;

  return &peers[min_upstream_index];
}


static void
max_connections_peer_free (ngx_peer_connection_t *pc, void *data, ngx_uint_t state)
{
  max_connections_srv_conf_t *maxconn_cf = data;
  max_connections_peer_t *peer = pc->data;

  ngx_log_debug1( NGX_LOG_DEBUG_HTTP
                , pc->log
                , 0
                , "FREE upstream request max_connections peer try %ui"
                , pc->tries
                );

  assert(peer->connections > 0);

  //if(state == 0) { // Connection sucessful
    peer->connections--;

    if(!ngx_queue_empty(&maxconn_cf->waiting_requests))
      max_connections_dispatch_new_connection(maxconn_cf);
  //}

  /* TODO try a differnt backend if error */
  pc->tries = 0;
}


static ngx_int_t
max_connections_peer_get (ngx_peer_connection_t *pc, void *data)
{
  max_connections_srv_conf_t *maxconn_cf = data;
  ngx_log_debug1( NGX_LOG_DEBUG_HTTP
                , pc->log
                , 0
                , "get upstream request max_connections peer try %ui"
                , pc->tries
                );

  pc->cached = 0;
  pc->connection = NULL;

  max_connections_peer_t *peer = 
    max_connections_find_open_upstream(maxconn_cf);

  assert(peer == NULL && "should always be an availible peer in max_connections_peer_get()");
  assert(peer->connections < maxconn_cf->max_connections);

  peer->connections++;

  pc->sockaddr = peer->sockaddr;
  pc->socklen  = peer->socklen;
  pc->name     = peer->name;
  pc->data     = peer;  // store this for later 

  return NGX_OK;
}

static ngx_int_t
max_connections_peer_init (ngx_http_request_t *r, ngx_http_upstream_srv_conf_t *uscf)
{
  ngx_log_debug0( NGX_LOG_DEBUG_HTTP
                , r->connection->log
                , 0
                , "init max connections peer"
                );

  max_connections_srv_conf_t *maxconn_cf = 
    ngx_http_conf_upstream_srv_conf(uscf, max_connections_module);

  r->upstream->peer.free = max_connections_peer_free;
  r->upstream->peer.get  = max_connections_peer_get;
  r->upstream->peer.tries = 1;
  r->upstream->peer.data = maxconn_cf;

  if(max_connections_find_open_upstream(maxconn_cf) == NULL) {
    /* insert request into queue */
    max_connections_waiting_t *waiting =
      ngx_palloc(r->pool, sizeof(max_connections_waiting_t));
    if(waiting == NULL) return NGX_ERROR;
    waiting->r = r; 
    ngx_queue_insert_head(&maxconn_cf->waiting_requests, &waiting->queue);
    return NGX_BUSY;
  }

  return NGX_OK;
}


static ngx_int_t
max_connections_init(ngx_conf_t *cf, ngx_http_upstream_srv_conf_t *uscf)
{
  ngx_log_debug0(NGX_LOG_DEBUG_HTTP, cf->log, 0, "init max_connections");

  max_connections_srv_conf_t *maxconn_cf = 
    ngx_http_conf_upstream_srv_conf(uscf, max_connections_module);

/* allocate all the max_connections_peer_t. put them in maxconn_cf->peers */
  if(uscf->servers == NULL) return NGX_ERROR;
  ngx_http_upstream_server_t *server = uscf->servers->elts;
  ngx_uint_t i, j;
  ngx_uint_t number_peers = 0;

  for (i = 0; i < uscf->servers->nelts; i++) 
      number_peers += server[i].naddrs;

  ngx_array_t *peers_array = 
    ngx_array_create(cf->pool, number_peers, sizeof(max_connections_peer_t));

  if (peers_array == NULL) return NGX_ERROR;
  max_connections_peer_t *peers = peers_array->elts;

  /* one hostname can have multiple IP addresses in DNS */
  ngx_uint_t n;
  for (n = 0, i = 0; i < uscf->servers->nelts; i++) {
    for (j = 0; j < server[i].naddrs; j++, n++) {
      peers[n].sockaddr     = server[i].addrs[j].sockaddr;
      peers[n].socklen      = server[i].addrs[j].socklen;
      peers[n].name         = &(server[i].addrs[j].name);
      peers[n].max_fails    = server[i].max_fails;
      peers[n].fail_timeout = server[i].fail_timeout;
      peers[n].down         = server[i].down;
      peers[n].weight       = server[i].down ? 0 : server[i].weight;
      peers[n].connections  = 0;
    }
  }
  maxconn_cf->peers = peers_array;

  uscf->peer.init = max_connections_peer_init;

  ngx_queue_init(&maxconn_cf->waiting_requests);

  return NGX_OK;
}

static char *
max_connections_command (ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
  ngx_http_upstream_srv_conf_t *uscf = 
    ngx_http_conf_get_module_srv_conf(cf, ngx_http_upstream_module);

  max_connections_srv_conf_t *maxconn_cf = 
    ngx_http_conf_upstream_srv_conf(uscf, max_connections_module);

  uscf->peer.init_upstream = max_connections_init;

  /* read options */
  ngx_str_t *value = cf->args->elts;
  ngx_int_t max_connections = ngx_atoi(value[1].data, value[1].len);

  if (max_connections == NGX_ERROR || max_connections == 0) {
    ngx_conf_log_error( NGX_LOG_EMERG
                      , cf
                      , 0
                      , "invalid value \"%V\" in \"%V\" directive"
                      , &value[1]
                      , &cmd->name
                      );
    return NGX_CONF_ERROR;
  }
  maxconn_cf->max_connections = (ngx_uint_t)max_connections;
  return NGX_CONF_OK;
}

static void *
max_connections_create_conf(ngx_conf_t *cf)
{
    max_connections_srv_conf_t  *conf = 
      ngx_pcalloc(cf->pool, sizeof(max_connections_srv_conf_t));

    if (conf == NULL) return NGX_CONF_ERROR;

    conf->max_connections = DEFAULT_MAX_CONNECTIONS;
    return conf;
}
