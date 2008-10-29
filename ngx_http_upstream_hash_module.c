/* max connections module for nginx
** october 2008, ryan dahl (ry@ndahl.us)
*/


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#define DEFAULT_MAX_CONNECTIONS 2

#define ngx_bitvector_index(index) index / (8 * sizeof(uintptr_t))
#define ngx_bitvector_bit(index) (uintptr_t) 1 << index % (8 * sizeof(uintptr_t))

typedef struct {
    ngx_uint_t max_connections;
    ngx_uint_t                         single;       /* unsigned:1 */

    ngx_queue_t                        request_queue;
    ngx_array_t                        peers; /* backend servers */

    ngx_http_upstream_init_pt          original_init_upstream;
    ngx_http_upstream_init_peer_pt     original_init_peer;

} max_connections_srv_conf_t;


typedef struct {
    struct sockaddr                *sockaddr;
    socklen_t                       socklen;
    ngx_str_t                       name;
} max_connections_peer_t;

typedef struct {
    ngx_uint_t                        number;
    max_connections_peer_t     peer[1];
} max_connections_peers_t;

typedef struct {
    unsigned long long                request_id;
    max_connections_peers_t   *peers;
    uintptr_t                         tried[1];
} max_connections_peer_data_t;


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


static ngx_module_t max_connections_module =
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



static ngx_int_t
max_connections_peer_get (ngx_peer_connection_t *pc, void *data)
{
    max_connections_peer_data_t  *uhpd = data;
    max_connections_peer_t       *peer;
    ngx_uint_t                           peer_index;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, pc->log, 0,
                   "get upstream request max_connections peer try %ui", pc->tries);

    pc->cached = 0;
    pc->connection = NULL;

    peer_index = uhpd->max_connections % uhpd->peers->number;

    peer = &uhpd->peers->peer[peer_index];

    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, pc->log, 0,
                   "chose peer %ui w/ max_connections %ui", peer_index, uhpd->max_connections);

    pc->sockaddr = peer->sockaddr;
    pc->socklen = peer->socklen;
    pc->name = &peer->name;

    return NGX_OK;
}

static void
max_connections_peer_free (ngx_peer_connection_t *pc, void *data,
    ngx_uint_t state)
{
    max_connections_peer_data_t  *uhpd = data;
    ngx_uint_t                           current;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, pc->log, 0, "free upstream max_connections peer try %ui", pc->tries);

    if (state & NGX_PEER_FAILED
            && --pc->tries)
    {
        current = uhpd->max_connections % uhpd->peers->number;

        uhpd->tried[ngx_bitvector_index(current)] |= ngx_bitvector_bit(current);

        do {
            uhpd->max_connections = ngx_max_connections_key((u_char *)&uhpd->max_connections, sizeof(ngx_uint_t));
            current = uhpd->max_connections % uhpd->peers->number;
        } while ((uhpd->tried[ngx_bitvector_index(current)] & ngx_bitvector_bit(current)) && --pc->tries);
    }
}

static ngx_int_t
max_connections_peer_init (ngx_http_request_t *r, ngx_http_upstream_srv_conf_t *uscf)
{
    max_connections_peer_data_t     *uhpd;

    ngx_str_t val;

    if (ngx_http_script_run(r, &val, us->lengths, 0, us->values) == NULL) {
        return NGX_ERROR;
    }

    uhpd = ngx_pcalloc(r->pool, sizeof(max_connections_peer_data_t)
            + sizeof(uintptr_t) * ((max_connections_peers_t *)us->peer.data)->number / (8 * sizeof(uintptr_t)));
    if (uhpd == NULL) {
        return NGX_ERROR;
    }

    r->upstream->peer.data = uhpd;

    uhpd->peers = us->peer.data;

    r->upstream->peer.free = max_connections_peer_free;
    r->upstream->peer.get  = max_connections_peer_get;
    r->upstream->peer.tries = 1;

    uhpd->max_connections = us->max_connections_function(val.data, val.len);

    return NGX_OK;
}


static ngx_int_t
max_connections_init(ngx_conf_t *cf, ngx_http_upstream_srv_conf_t *uscf)
{
  ngx_log_debug0(NGX_LOG_DEBUG_HTTP, cf->log, 0, "init max_connections");

  max_connections_srv_conf_t *maxconn_cf = 
    ngx_http_conf_upstream_srv_conf(uscf, max_connections_module);

  /* allocate all the max_connections_peer_t 
   * put them in maxconn_cf->peers 
   */
  if(!uscf->servers)
    return NGX_ERROR;
  ngx_http_upstream_server_t *server = uscf->servers->elts;
  ngx_uint i, j, number_peers = 0;

  for (i = 0; i < uscf->servers->nelts; i++) 
      number_peers += server[i].naddrs;

  ngx_array_t *peers = 
    ngx_array_create(uscf->pool, number_peers, sizeof(max_connections_peer_t));

  if (peers == NULL)
      return NGX_ERROR;

  /* one hostname can have multiple IP addresses in DNS */
  ngx_uint n;
  for (n = 0, i = 0; i < us->servers->nelts; i++) {
    for (j = 0; j < server[i].naddrs; j++, n++) {
      peers->elts[n].sockaddr = server[i].addrs[j].sockaddr;
      peers->elts[n].socklen  = server[i].addrs[j].socklen;
      peers->elts[n].name     = server[i].addrs[j].name;
    }
  }
  maxconn_cf->peers = peers;

  maxconn_cf->original_init_peer = uscf->peer.init;

  uscf->peer.init = max_connections_peer_init;
  ngx_queue_init(&max_conn->request_queue);

  return NGX_OK;
}


static char *
max_connections_command (ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
  ngx_http_upstream_srv_conf_t *uscf = 
    ngx_http_conf_get_module_srv_conf(cf, ngx_http_upstream_module);

  max_connections_srv_conf_t *maxconn_cf = 
    ngx_http_conf_upstream_srv_conf(uscf, max_connections_module);

  /* allow maxconn to be stacked */
  /*
  maxconn_cf->original_init_upstream = uscf->peer.init_upstream 
                                       ? uscf->peer.init_upstream 
                                       : ngx_http_upstream_init_round_robin;
  */

  uscf->peer.init_upstream = max_connections_init;

  /* read options */
  ngx_str_t *value = cf->args->elts;
  ngx_uint_t max_connections = ngx_atoi(value[1].data, value[1].len);

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
  maxconn_cf->max_connections = max_connections;
  return NGX_CONF_OK;
}

static void *
max_connections_create_conf(ngx_conf_t *cf)
{
    max_connections_srv_conf_t  *conf = 
      ngx_pcalloc(cf->pool, sizeof(max_connections_srv_conf_t));

    if (conf == NULL) return NGX_CONF_ERROR;

    /*
     * set by ngx_pcalloc():
     *
     *     conf->original_init_upstream = NULL;
     *     conf->original_init_peer = NULL;
     */

    conf->max_connections = DEFAULT_MAX_CONNECTIONS;
    return conf;
}
