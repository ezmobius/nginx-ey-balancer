/* max connections module for nginx
 * Copyright 2008 Engine Yard, Inc. All rights reserved. 
 * Author: Ryan Dahl (ry@ndahl.us)
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>
#include <ngx_http.h>
#include <ngx_http_upstream.h>
#include <assert.h>

/* 0.5 seconds until a backend SLOT is reset after client half-close */
#define CLIENT_CLOSURE_SLEEP ((ngx_msec_t)500)  

typedef struct {
  ngx_uint_t max_connections;
  ngx_queue_t waiting_requests;
  ngx_array_t *backends; /* backend servers */
  ngx_event_t queue_check_event;
  ngx_msec_t queue_timeout;
} max_connections_srv_conf_t;

typedef struct {
  struct sockaddr *sockaddr;
  socklen_t socklen;
  ngx_str_t *name;

  ngx_uint_t weight; /* unused */
  ngx_uint_t  max_fails;
  time_t fail_timeout;

  time_t accessed;
  ngx_uint_t down:1;

  ngx_uint_t fails;
  ngx_uint_t client_closures;
  ngx_uint_t connections;
  ngx_event_t disconnect_event;
  max_connections_srv_conf_t *maxconn_cf;
} max_connections_backend_t;

typedef struct {
  max_connections_srv_conf_t *maxconn_cf;
  max_connections_backend_t  *backend; /* the backend the peer was sent to */
  ngx_queue_t queue; /* queue information */
  ngx_http_request_t *r; /* the request associated with the peer */
  ngx_msec_t accessed;
} max_connections_peer_data_t;

static ngx_uint_t max_connections_rr_index;

/* forward declarations */
static char * max_connections_command (ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static char * max_connections_queue_timeout_command (ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static void * max_connections_create_conf(ngx_conf_t *cf);

#define RAMP(x) (x > 0 ? x : 0)

static ngx_command_t  max_connections_commands[] =
{ { ngx_string("max_connections")
  , NGX_HTTP_UPS_CONF|NGX_CONF_TAKE1
  , max_connections_command
  , 0
  , 0
  , NULL
  }
, { ngx_string("max_connections_queue_timeout")
  , NGX_HTTP_UPS_CONF|NGX_CONF_TAKE1
  , max_connections_queue_timeout_command
  , 0
  , offsetof(max_connections_srv_conf_t, queue_timeout)
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

static ngx_uint_t
queue_size (max_connections_srv_conf_t *maxconn_cf)
{
  ngx_queue_t *node;
  ngx_uint_t queue_size = 0;
  /* TODO O(n) could be O(1) */
  for( node = maxconn_cf->waiting_requests.next
     ; node && node != &maxconn_cf->waiting_requests 
     ; node = node->next
     ) queue_size += 1;
  return queue_size;
}

static int
queue_remove (max_connections_peer_data_t *peer_data)
{
  if(peer_data->queue.next != NULL) {
    ngx_queue_remove(&peer_data->queue);
    peer_data->queue.prev = peer_data->queue.next = NULL; 
    ngx_log_debug1( NGX_LOG_DEBUG_HTTP
                  , peer_data->r->connection->log
                  , 0
                  , "max_connections del queue (new size %ui)"
                  , queue_size(peer_data->maxconn_cf)
                  );
    return 1;
  }
  return 0;
}

static max_connections_peer_data_t *
queue_oldest (max_connections_srv_conf_t *maxconn_cf)
{
  if(ngx_queue_empty(&maxconn_cf->waiting_requests)) 
    return NULL;

  ngx_queue_t *last = ngx_queue_last(&maxconn_cf->waiting_requests);

  max_connections_peer_data_t *peer_data = 
    ngx_queue_data(last, max_connections_peer_data_t, queue);
  return peer_data;
}

/* removes the first item from the queue - returns request */
static max_connections_peer_data_t *
queue_shift (max_connections_srv_conf_t *maxconn_cf)
{
  if(ngx_queue_empty(&maxconn_cf->waiting_requests)) 
    return NULL;

  max_connections_peer_data_t *peer_data = queue_oldest (maxconn_cf);

  int r = queue_remove (peer_data);
  assert(r == 1);

  if(ngx_queue_empty(&maxconn_cf->waiting_requests)) {
    /* delete the timer if the queue is empty now */
    if(maxconn_cf->queue_check_event.timer_set) {
      ngx_del_timer( (&maxconn_cf->queue_check_event) );
    }
  } else { 
    /* make sure that the check queue timer is set when we have things in
     * the queue */
    max_connections_peer_data_t *oldest = queue_oldest (maxconn_cf);
    /*  ------|-----------|-------------|------------------------ */
    /*       accessed    now           accessed + TIMEOUT         */
    ngx_add_timer( (&maxconn_cf->queue_check_event)
                 , RAMP(300 + oldest->accessed + maxconn_cf->queue_timeout - ngx_current_msec)
                 ); 
  }

  return peer_data;
}

/* This is function selects an open backend. It simply iterates through the
 * backends looking for the one with the least connections. */
static max_connections_backend_t*
max_connections_find_open_upstream (max_connections_srv_conf_t *maxconn_cf)
{
#define MAXCONN_BIGNUM 999999
  ngx_uint_t c
           , index
           , min_backend_index = MAXCONN_BIGNUM
           , min_backend_connections = MAXCONN_BIGNUM 
           ;
  ngx_uint_t nbackends = maxconn_cf->backends->nelts;
  max_connections_backend_t *backends = maxconn_cf->backends->elts;
  time_t now = ngx_time();

  for( c = 0, index = ngx_random() % nbackends
     ; c < nbackends
     ; c++, index = (index + 1) % nbackends
     )
  {
    max_connections_backend_t *backend = &backends[index];

    if(now - backend->accessed > backend->fail_timeout) {
      backend->fails = 0;
    }

    if(backend->fails >= backend->max_fails || backend->down) 
      continue;

    if(backend->connections < min_backend_connections) {
      min_backend_connections = backend->connections;
      min_backend_index = index;
    }
  }

  if(min_backend_connections == MAXCONN_BIGNUM)
    return NULL;

  assert(min_backend_connections <= maxconn_cf->max_connections && "the minimum connections that we have found should be less than the global setting!");
  assert(min_backend_index < nbackends);

  max_connections_backend_t *choosen = &backends[min_backend_index];

  assert(choosen->connections == min_backend_connections);
  assert(!choosen->down);
  assert(choosen->fails < choosen->max_fails);

  if(choosen->connections == maxconn_cf->max_connections) 
    return NULL; /* no open slots */

  assert(choosen->connections < maxconn_cf->max_connections);

  return choosen;
}

/* Returns true if there are no slots to send a request to. */
#define max_connections_upstreams_all_occupied(maxconn_cf) \
  (max_connections_find_open_upstream (maxconn_cf) == NULL)

/* This function takes the oldest request on the queue
 * (maxconn_cf->waiting_requests) and dispatches it to the backends.  This
 * calls ngx_http_upstream_connect() which will in turn call the peer get
 * callback, max_connections_peer_get(). max_connections_peer_get() will do
 * the actual selection of backend. Here we're just giving the request the
 * go-ahead to proceed.
 */
static void
max_connections_dispatch (max_connections_srv_conf_t *maxconn_cf)
{
  if(ngx_queue_empty(&maxconn_cf->waiting_requests)) return;
  if(max_connections_upstreams_all_occupied(maxconn_cf)) return;

  max_connections_peer_data_t *peer_data = queue_shift(maxconn_cf);
  ngx_http_request_t *r = peer_data->r;

  assert(!r->connection->destroyed);
  assert(!r->connection->error);
  assert(peer_data->backend == NULL);

  ngx_log_debug2( NGX_LOG_DEBUG_HTTP
                , r->connection->log
                , 0
                , "max_connections dispatch (queue timeout: %ui, maxconn: %ui)"
                , maxconn_cf->queue_timeout
                , maxconn_cf->max_connections
                );
  ngx_http_upstream_connect(r, r->upstream);

  /* can we dispatch again? */
  max_connections_dispatch(maxconn_cf);
}

static void
recover_from_client_closure(ngx_event_t *ev)
{
  max_connections_backend_t *backend = ev->data;

  assert(backend->connections > 0);
  assert(backend->client_closures > 0);

  backend->connections -= backend->client_closures; 
  backend->client_closures = 0;

  max_connections_dispatch(backend->maxconn_cf);
}

static void
queue_check_event(ngx_event_t *ev)
{
  max_connections_srv_conf_t *maxconn_cf = ev->data;

  max_connections_peer_data_t *oldest; 

  while ( (oldest = queue_oldest(maxconn_cf))
       && ngx_current_msec - oldest->accessed > maxconn_cf->queue_timeout
        ) 
  {
    max_connections_peer_data_t *peer_data = queue_shift(maxconn_cf);
    assert(peer_data == oldest);
    ngx_log_debug0( NGX_LOG_DEBUG_HTTP
                  , peer_data->r->connection->log
                  , 0
                  , "max_connections expire"
                  );
    ngx_http_finalize_request(peer_data->r, NGX_HTTP_INTERNAL_SERVER_ERROR);
  }

  /* try to dispatch some requets */
  max_connections_dispatch(maxconn_cf);
}


/* The peer free function which is part of all NGINX upstream modules */
static void
max_connections_peer_free (ngx_peer_connection_t *pc, void *data, ngx_uint_t state)
{
  max_connections_peer_data_t *peer_data = data;
  max_connections_backend_t *backend = peer_data->backend;
  max_connections_srv_conf_t *maxconn_cf = peer_data->maxconn_cf;

  /* This happens when a client closes their connection before the request
   * is completed */
  if(peer_data->r->connection->error) {

    /* If the connection is in the queue, remove it. */
    queue_remove(peer_data);
    
    /* If the connection is connected to a backend */
    if(backend != NULL) {
      if(!backend->disconnect_event.timer_set) {
        assert(backend->client_closures == 0);
        ngx_add_timer( (&backend->disconnect_event)
                     , CLIENT_CLOSURE_SLEEP
                     );
      }
      backend->client_closures++;
      assert(backend->client_closures <= maxconn_cf->max_connections);
      peer_data->backend = NULL;
    }

    pc->tries = 0;
    goto dispatch;
  }

  if(pc) 
    pc->tries--;

  if(backend) {
    assert(backend->connections > 0);
    backend->connections--; /* free the slot */
    ngx_log_debug2( NGX_LOG_DEBUG_HTTP
                  , peer_data->r->connection->log
                  , 0
                  , "max_connections recv client from %V (now %ui connections)"
                  , backend->name
                  , backend->connections
                  );
  }

  /* previous connection failed (state & NGX_PEER_FAILED) or either the
   * connection failed, or it succeeded but the application returned an
   * error (state & NGX_PEER_NEXT) 
   */ 
  if(state & NGX_PEER_FAILED) {
    ngx_log_debug1( NGX_LOG_DEBUG_HTTP
                  , pc->log
                  , 0
                  , "max_connections %V failed "
                  , backend->name
                  );
    peer_data->backend->accessed = ngx_time();
    peer_data->backend->fails++;
  } else if (state & NGX_PEER_NEXT) {
    assert(0 && "TODO just get the next host");
  }

  peer_data->backend = NULL;

  if(state != 0) {
    return;
  }


dispatch:
  max_connections_dispatch(maxconn_cf);
}

static ngx_int_t
max_connections_peer_get (ngx_peer_connection_t *pc, void *data)
{
  max_connections_peer_data_t *peer_data = data;
  max_connections_srv_conf_t *maxconn_cf = peer_data->maxconn_cf;

  assert(peer_data->queue.next == NULL && "should not be in the queue");
  assert(peer_data->queue.prev == NULL && "should not be in the queue");

  max_connections_backend_t *backend = 
    max_connections_find_open_upstream(maxconn_cf);
  assert(backend != NULL && "should always be an availible backend in max_connections_peer_get()");
  assert(backend->connections < maxconn_cf->max_connections);
  assert(peer_data->backend == NULL);

  peer_data->backend = backend;
  backend->connections++; /* keep track of how many slots are occupied */

  pc->sockaddr = backend->sockaddr;
  pc->socklen  = backend->socklen;
  pc->name     = backend->name;

  ngx_log_debug2( NGX_LOG_DEBUG_HTTP
                , pc->log
                , 0
                , "max_connections sending client to %V (now %ui connections)"
                , pc->name
                , backend->connections
                );
  return NGX_OK;
}

static ngx_int_t
max_connections_peer_init (ngx_http_request_t *r, ngx_http_upstream_srv_conf_t *uscf)
{
  max_connections_srv_conf_t *maxconn_cf = 
    ngx_http_conf_upstream_srv_conf(uscf, max_connections_module);

  max_connections_peer_data_t *peer_data = 
    ngx_palloc(r->pool, sizeof(max_connections_peer_data_t));
  if(peer_data == NULL) return NGX_ERROR;

  peer_data->backend = NULL;
  peer_data->maxconn_cf = maxconn_cf;
  peer_data->r = r;
  peer_data->accessed = ngx_current_msec; 

  r->upstream->peer.free  = max_connections_peer_free;
  r->upstream->peer.get   = max_connections_peer_get;
  r->upstream->peer.tries = maxconn_cf->backends->nelts;
  r->upstream->peer.data  = peer_data;

  ngx_queue_insert_head(&maxconn_cf->waiting_requests, &peer_data->queue);
  ngx_log_debug1( NGX_LOG_DEBUG_HTTP
                , r->connection->log
                , 0
                , "max_connections add queue (new size %ui)"
                , queue_size(maxconn_cf)
                );

  max_connections_dispatch(peer_data->maxconn_cf);
  return NGX_BUSY;
}

static ngx_int_t
max_connections_init(ngx_conf_t *cf, ngx_http_upstream_srv_conf_t *uscf)
{
  ngx_log_debug0( NGX_LOG_DEBUG_HTTP
                , cf->log
                , 0
                , "max_connections_init"
                );

  max_connections_srv_conf_t *maxconn_cf = 
    ngx_http_conf_upstream_srv_conf(uscf, max_connections_module);

  /* allocate all the max_connections_backend_t. put them in
   * maxconn_cf->backends */
  if(uscf->servers == NULL) return NGX_ERROR;
  ngx_http_upstream_server_t *server = uscf->servers->elts;
  ngx_uint_t i, j;
  ngx_uint_t number_backends = 0;

  for (i = 0; i < uscf->servers->nelts; i++) 
      number_backends += server[i].naddrs;

  ngx_array_t *backends = 
    ngx_array_create(cf->pool, number_backends, sizeof(max_connections_backend_t));
  if (backends == NULL) return NGX_ERROR;

  /* one hostname can have multiple IP addresses in DNS */
  ngx_uint_t n;
  for (n = 0, i = 0; i < uscf->servers->nelts; i++) {
    for (j = 0; j < server[i].naddrs; j++, n++) {
      max_connections_backend_t *backend = ngx_array_push(backends);
      backend->sockaddr     = server[i].addrs[j].sockaddr;
      backend->socklen      = server[i].addrs[j].socklen;
      backend->name         = &(server[i].addrs[j].name);
      backend->max_fails    = server[i].max_fails;
      backend->fail_timeout = server[i].fail_timeout;
      backend->down         = server[i].down;
      backend->weight       = server[i].down ? 0 : server[i].weight;
      backend->connections  = 0;

      backend->disconnect_event.handler = recover_from_client_closure;
      backend->disconnect_event.log = cf->log;
      backend->disconnect_event.data = backend;
      backend->maxconn_cf = maxconn_cf;
    }
  }
  maxconn_cf->backends = backends;

  uscf->peer.init = max_connections_peer_init;

  ngx_queue_init(&maxconn_cf->waiting_requests);
  assert(ngx_queue_empty(&maxconn_cf->waiting_requests));

  maxconn_cf->queue_check_event.handler = queue_check_event;
  maxconn_cf->queue_check_event.log = cf->log;
  maxconn_cf->queue_check_event.data = maxconn_cf;

  return NGX_OK;
}

static char *
max_connections_queue_timeout_command (ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
  ngx_http_upstream_srv_conf_t *uscf = 
    ngx_http_conf_get_module_srv_conf(cf, ngx_http_upstream_module);

  max_connections_srv_conf_t *maxconn_cf = 
    ngx_http_conf_upstream_srv_conf(uscf, max_connections_module);

  ngx_str_t        *value; 

  value = cf->args->elts;    

  ngx_msec_t ms = ngx_parse_time(&value[1], 0); 
  if (ms == (ngx_msec_t) NGX_ERROR) {
      return "invalid value";
  }

  if (ms == (ngx_msec_t) NGX_PARSE_LARGE_TIME) {
      return "value must be less than 597 hours";
  }

  maxconn_cf->queue_timeout = ms;

  return NGX_CONF_OK;
}

static char *
max_connections_command (ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
  ngx_http_upstream_srv_conf_t *uscf = 
    ngx_http_conf_get_module_srv_conf(cf, ngx_http_upstream_module);
  /* 1. set the initialization function */
  uscf->peer.init_upstream = max_connections_init;

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

  max_connections_srv_conf_t *maxconn_cf = 
    ngx_http_conf_upstream_srv_conf(uscf, max_connections_module);
  /* 2. set the number of max_connections */
  maxconn_cf->max_connections = (ngx_uint_t)max_connections;

  return NGX_CONF_OK;
}

static void *
max_connections_create_conf(ngx_conf_t *cf)
{
    max_connections_srv_conf_t  *conf = 
      ngx_pcalloc(cf->pool, sizeof(max_connections_srv_conf_t));

    if (conf == NULL) return NGX_CONF_ERROR;
    max_connections_rr_index = 0;
    conf->max_connections = 1;
    conf->queue_timeout = 1000;  /* default queue timeout 5 seconds */
    return conf;
}

