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

typedef struct {
  ngx_uint_t max_connections;
  ngx_queue_t waiting_requests;
  ngx_array_t *backends; /* backend servers */
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
  ngx_uint_t connections;
  ngx_event_t disconnect_event;
  max_connections_srv_conf_t *maxconn_cf;
} max_connections_backend_t;

typedef struct {
  max_connections_srv_conf_t *maxconn_cf;
  max_connections_backend_t  *backend; /* the backend the peer was sent to */
  ngx_queue_t queue; /* queue information */
  ngx_http_request_t *r; /* the request associated with the peer */
} max_connections_peer_data_t;

static ngx_uint_t max_connections_rr_index;

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


static ngx_uint_t
max_connections_queue_size (max_connections_srv_conf_t *maxconn_cf)
{
    ngx_queue_t *node;
    ngx_uint_t queue_size = 0;
    for( node = maxconn_cf->waiting_requests.next
       ; node && node != &maxconn_cf->waiting_requests 
       ; node = node->next
       ) queue_size += 1;
    return queue_size;
}

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
  if( ngx_queue_empty(&maxconn_cf->waiting_requests)
   //|| max_connections_upstreams_all_occupied(maxconn_cf)
    ) return;

  ngx_queue_t *last = ngx_queue_last(&maxconn_cf->waiting_requests);
  ngx_queue_remove(last);
  max_connections_peer_data_t *peer_data = 
    ngx_queue_data(last, max_connections_peer_data_t, queue);

  ngx_http_request_t *r = peer_data->r;
  if(r == NULL) {
    max_connections_dispatch (maxconn_cf);
    return;
  }

  if(r->connection->error) {
    ngx_log_debug0( NGX_LOG_DEBUG_HTTP
                  , r->connection->log
                  , 0
                  , "max_connections: request already closed!"
                  );
    ngx_http_finalize_request(r, NGX_HTTP_CLIENT_CLOSED_REQUEST);  // XXX rc correct?   
    max_connections_dispatch (maxconn_cf);
  } else {
    ngx_log_debug0( NGX_LOG_DEBUG_HTTP
                  , r->connection->log
                  , 0
                  , "max_connections: dispatch"
                  );
    ngx_http_upstream_connect(r, r->upstream);
  }
}

/* This is function selects an open backend. It is not called directly
 * rather <max_connections_upstreams_all_occupied> and
 * <max_connections_find_rotate_upstream> are used. It simply iterates
 * through the backends looking for the one with the least connections. The
 * global (process-wide) variable <max_connections_rr_index> is used so that
 * the iteration always starts at a different place.
 */
static max_connections_backend_t*
max_connections_find_open_upstream (max_connections_srv_conf_t *maxconn_cf, ngx_int_t rotate)
{
  ngx_uint_t n
           , index
           , min_backend_index = 99999
           , min_backend_connections = 999999 /* set to a high number */
           ;
  ngx_uint_t nbackends = maxconn_cf->backends->nelts;
  max_connections_backend_t *backends = maxconn_cf->backends->elts;
  time_t now = ngx_time();
  for(n = 0; n < nbackends; n++) {
    /* we're just iterating through the backends. the strange index
     * is so that we always start a new place in the array and not 
     * at the beginning. this insures that backends are used evenly.
     */
    index = (n + max_connections_rr_index) % nbackends;
    max_connections_backend_t *backend = &backends[index];

    if(now - backend->accessed > backend->fail_timeout) {
      backend->fails = 0;
    }

    if( backend->fails >= backend->max_fails
     || backend->down
      ) continue;

    if(backend->connections < min_backend_connections) {
      min_backend_connections = backend->connections;
      min_backend_index = index;
    }
  }
  assert(min_backend_connections <= maxconn_cf->max_connections && "the minimum connections that we have found should be less than the global setting!");
  assert(min_backend_index < nbackends);

  max_connections_backend_t *choosen = &backends[min_backend_index];
  assert(choosen->connections == min_backend_connections);
  assert(!choosen->down);
  assert(choosen->fails < choosen->max_fails);

  if(rotate)
    /* increase the index that we start our iteration at */
    max_connections_rr_index = (1 + max_connections_rr_index) % nbackends;

  if(choosen->connections == maxconn_cf->max_connections) 
    return NULL; /* no open slots */
  assert(choosen->connections < maxconn_cf->max_connections);

  return choosen;
}

/* Returns true if there are no slots to send a request to. */
#define max_connections_upstreams_all_occupied(maxconn_cf) \
  (max_connections_find_open_upstream (maxconn_cf, 0) == NULL)

/* finds a backend slot and inceases <max_connections_rr_index>.
 * Returns NULL if none are found. */
#define max_connections_find_rotate_upstream(maxconn_cf) \
  max_connections_find_open_upstream (maxconn_cf, 1)

/* The peer free function which is part of all NGINX upstream modules
 */
static void
max_connections_peer_free (ngx_peer_connection_t *pc, void *data, ngx_uint_t state)
{
  max_connections_peer_data_t *peer_data = data;
  /* this is the backend that the peer was connecting to */
  max_connections_backend_t *backend = peer_data->backend;

  if(backend == NULL) {
    ngx_log_debug0( NGX_LOG_DEBUG_HTTP
                  , pc->log
                  , 0
                  , "max_connections_peer_free backend == NULL"
                  );
    pc->tries = 0;
    return;
  }

  if(pc)
    pc->tries--;

  /* previous connection successful */
  if(state == 0) {
  } else {
    /* previous connection failed (state & NGX_PEER_FAILED) or either the
     * connection failed, or it succeeded but the application returned an
     * error (state & NGX_PEER_NEXT) 
     */ 
    ngx_log_debug1( NGX_LOG_DEBUG_HTTP
                  , pc->log
                  , 0
                  , "max_connections %V failed "
                  , backend->name
                  );
    peer_data->backend->accessed = ngx_time();
    peer_data->backend->fails++;
  }

  ngx_log_debug2( NGX_LOG_DEBUG_HTTP
                , peer_data->r->connection->log
                , 0
                , "max_connections recv client from %V (%ui connections)"
                , backend->name
                , backend->connections
                );
  pc->tries = 0;
}

static void
max_connections_disconnect_from_upstream(ngx_event_t *ev)
{
  max_connections_backend_t *backend = ev->data;
  assert(backend->connections > 0);
  backend->connections--; /* free the slot */

  if(!ngx_queue_empty(&backend->maxconn_cf->waiting_requests))
    max_connections_dispatch(backend->maxconn_cf);
}

static void
max_connections_cleanup(void *data)
{
  max_connections_peer_data_t *peer_data = data;
  max_connections_backend_t *backend = peer_data->backend;
  ngx_http_request_t *r = peer_data->r; 

  if(peer_data->queue.next != NULL && peer_data->queue.prev != NULL) {
    ngx_queue_remove(&peer_data->queue);
    ngx_log_debug1( NGX_LOG_DEBUG_HTTP
                  , r->connection->log
                  , 0
                  , "max_connections del queue (new size %ui)"
                  , max_connections_queue_size(peer_data->maxconn_cf)
                  );
  }

  if(backend) {
    if(r->connection->error) {
      if (!backend->disconnect_event.timer_set) {
        /* allow some time for the backend connection to reset? */
        ngx_add_timer((&backend->disconnect_event), 500);
      }
    } else {
      ngx_post_event((&backend->disconnect_event), &ngx_posted_events);
    }
  }
}

static ngx_int_t
max_connections_peer_get (ngx_peer_connection_t *pc, void *data)
{
  max_connections_peer_data_t *peer_data = data;
  max_connections_srv_conf_t *maxconn_cf = peer_data->maxconn_cf;

  /* XXX do i need this?
  pc->cached = 0;
  pc->connection = NULL;
  */

  max_connections_backend_t *backend = 
    max_connections_find_rotate_upstream(maxconn_cf);
  assert(backend != NULL && "should always be an availible backend in max_connections_peer_get()");
  assert(backend->connections < maxconn_cf->max_connections);

  peer_data->backend = backend;

  pc->sockaddr = backend->sockaddr;
  pc->socklen  = backend->socklen;
  pc->name     = backend->name;

  ngx_log_debug2( NGX_LOG_DEBUG_HTTP
                , pc->log
                , 0
                , "max_connections sending client to %V (%ui connections)"
                , pc->name
                , backend->connections
                );
  backend->connections++; /* keep track of how many slots are occupied */
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

  r->upstream->peer.free  = max_connections_peer_free;
  r->upstream->peer.get   = max_connections_peer_get;
  r->upstream->peer.tries = 1; /* FIXME - set to maxconn_cf->backends->nelts ?*/
  r->upstream->peer.data  = peer_data;

  ngx_http_cleanup_t *cleanup = ngx_http_cleanup_add(r, 0);
  if (cleanup == NULL) return NGX_ERROR;
  cleanup->handler = max_connections_cleanup;
  cleanup->data = peer_data; 


  if(max_connections_upstreams_all_occupied(maxconn_cf)) {
    ngx_queue_insert_head(&maxconn_cf->waiting_requests, &peer_data->queue);
    ngx_log_debug1( NGX_LOG_DEBUG_HTTP
                  , r->connection->log
                  , 0
                  , "max_connections add queue (new size %ui)"
                  , max_connections_queue_size(maxconn_cf)
                  );
    return NGX_BUSY;
  }

  return NGX_OK;
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

      backend->disconnect_event.handler = max_connections_disconnect_from_upstream;
      backend->disconnect_event.log = cf->log;
      backend->disconnect_event.data = backend;
      backend->disconnect_event.prev = NULL;
      backend->disconnect_event.next = NULL;
      backend->maxconn_cf = maxconn_cf;
    }
  }
  maxconn_cf->backends = backends;

  uscf->peer.init = max_connections_peer_init;

  ngx_queue_init(&maxconn_cf->waiting_requests);
  assert(ngx_queue_empty(&maxconn_cf->waiting_requests));


  return NGX_OK;
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
    return conf;
}
