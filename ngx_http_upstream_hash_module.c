/* 
** GOAL
A module which allows for only a maxium number of connections to any of the
upstream servers. For example

  upstream mongrels {
    127.0.0.1:8000;
    127.0.0.1:8001;
    maxconn 2;
  }

will only allow 2 requests at a time to be sent to each Mongrel. That means if
there are more than 4 requests at a time, some will be queued inside this module
waiting for dispatch to a mongrel.

Why is this necessary? Many backends are programmed in crappy languages which
simply cannot handle the multiplexing. Mongrel is a perfect example - it is a
threaded HTTP server. That is, each request is done in a new Ruby thread.
Rails, the popular framework used with Mongrel, on the other hand has a large
mutex lock for processing each request. This means that Mongrel will accept
many connections but they wait in-line while Rails chugs along on a single
request. Having many open threads, many open sockets, and Rails chewing in the
background has proven to be a bad combination - it works - but experience shows
that a proxy like HAproxy with a maxconn=1 or maxconn=2 (i.e. the connections
queued in the proxy and not in the backends) performs much better.

** HOW TO ACHIVE THIS

Nginx provides a very user friendly interface to add custom load balancers. 
Unfortunately this interface assumes that every request is immediately sent to
a backend or is an error. To see why this is true let's look at what takes
place when a request goes through the server and hits a proxy_pass call. We
start at the proxy handler:

     ngx_http_proxy_handler(request) 

calls

     ngx_http_read_client_request_body(request, ngx_http_upstream_init);

which calls

     ngx_http_upstream_init(request)

which calls 

    if (uscf->peer.init(r, uscf) != NGX_OK) {
        ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
        return;
    }

    ngx_http_upstream_connect(request, request->upstream);


Where uscf is of type ngx_http_upstream_srv_conf_t and equal to
request->upstream->conf->upstream.  Note that uscf->peer.init() is the peer
initialization function (http://emiller.info/nginx-modules-guide.html#lb-peer)

NOTE that the user of the load balancer module API does not have control of
whether ngx_http_upstream_connect is called. Let's continue down the call path
ngx_http_upstream_connect calls 

    ngx_event_connect_peer(request->upstream->peer)  

And that in turn calls

    request->upstream->peer->get( request->upstream->peer 
                                , request->upstream->peer->data
                                )

This get() function is the "load balancer function" provided by the load
balancer module. It says which upstream server the request should be sent
to.
http://emiller.info/nginx-modules-guide.html#lb-function

After calling the load balancer callback ngx_event_connect_peer() proceeds
to the actual socket connection -- something we want to avoid if the all the
backends are currently at maxconn.

*/


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#define ngx_bitvector_index(index) index / (8 * sizeof(uintptr_t))
#define ngx_bitvector_bit(index) (uintptr_t) 1 << index % (8 * sizeof(uintptr_t))


typedef struct {
    struct sockaddr                *sockaddr;
    socklen_t                       socklen;
    ngx_str_t                       name;
} ngx_http_upstream_hash_peer_t;

typedef struct {
    ngx_uint_t                        number;
    ngx_http_upstream_hash_peer_t     peer[1];
} ngx_http_upstream_hash_peers_t;

typedef struct {
    unsigned long long                request_id;
    ngx_http_upstream_hash_peers_t   *peers;
    uintptr_t                         tried[1];
} ngx_http_upstream_hash_peer_data_t;


static ngx_int_t ngx_http_upstream_init_hash_peer(ngx_http_request_t *r,
    ngx_http_upstream_srv_conf_t *us);
static ngx_int_t ngx_http_upstream_get_hash_peer(ngx_peer_connection_t *pc,
    void *data);
static void ngx_http_upstream_free_hash_peer(ngx_peer_connection_t *pc,
    void *data, ngx_uint_t state);
static char *ngx_http_upstream_hash(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_http_upstream_hash_again(ngx_conf_t *cf, ngx_command_t *cmd, 
    void *conf);
static char *ngx_http_upstream_hash_method(ngx_conf_t *cf, ngx_command_t *cmd, 
    void *conf);
static ngx_int_t ngx_http_upstream_init_hash(ngx_conf_t *cf, 
    ngx_http_upstream_srv_conf_t *us);
static ngx_uint_t ngx_http_upstream_hash_crc32(u_char *data, size_t len);


static ngx_command_t  ngx_http_upstream_hash_commands[] = {
    { ngx_string("hash"),
      NGX_HTTP_UPS_CONF|NGX_CONF_TAKE1,
      ngx_http_upstream_hash,
      0,
      0,
      NULL },

    { ngx_string("hash_method"),
      NGX_HTTP_UPS_CONF|NGX_CONF_TAKE1,
      ngx_http_upstream_hash_method,
      0,
      0,
      NULL },

    { ngx_string("hash_again"),
      NGX_HTTP_UPS_CONF|NGX_CONF_TAKE1,
      ngx_http_upstream_hash_again,
      0,
      0,
      NULL },

      ngx_null_command
};


static ngx_http_module_t  ngx_http_upstream_hash_module_ctx = {
    NULL,                                  /* preconfiguration */
    NULL,                                  /* postconfiguration */

    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    NULL,                                  /* create location configuration */
    NULL                                   /* merge location configuration */
};


ngx_module_t  ngx_http_upstream_hash_module = {
    NGX_MODULE_V1,
    &ngx_http_upstream_hash_module_ctx,    /* module context */
    ngx_http_upstream_hash_commands,       /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


static ngx_int_t
ngx_http_upstream_init_hash(ngx_conf_t *cf, ngx_http_upstream_srv_conf_t *us)
{
    ngx_uint_t                       i, j, n;
    ngx_http_upstream_server_t      *server;
    ngx_http_upstream_hash_peers_t  *peers;

    us->peer.init = ngx_http_upstream_init_hash_peer;

    if (!us->servers) {
        return NGX_ERROR;
    }

    server = us->servers->elts;

    for (n = 0, i = 0; i < us->servers->nelts; i++) {
        n += server[i].naddrs;
    }

    peers = ngx_pcalloc(cf->pool, sizeof(ngx_http_upstream_hash_peers_t)
            + sizeof(ngx_http_upstream_hash_peer_t) * (n - 1));

    if (peers == NULL) {
        return NGX_ERROR;
    }

    peers->number = n;

    /* one hostname can have multiple IP addresses in DNS */
    for (n = 0, i = 0; i < us->servers->nelts; i++) {
        for (j = 0; j < server[i].naddrs; j++, n++) {
            peers->peer[n].sockaddr = server[i].addrs[j].sockaddr;
            peers->peer[n].socklen = server[i].addrs[j].socklen;
            peers->peer[n].name = server[i].addrs[j].name;
        }
    }

    us->peer.data = peers;

    return NGX_OK;
}


static ngx_int_t
ngx_http_upstream_init_hash_peer(ngx_http_request_t *r,
    ngx_http_upstream_srv_conf_t *us)
{
    ngx_http_upstream_hash_peer_data_t     *uhpd;
    
    ngx_str_t val;

    if (ngx_http_script_run(r, &val, us->lengths, 0, us->values) == NULL) {
        return NGX_ERROR;
    }

    uhpd = ngx_pcalloc(r->pool, sizeof(ngx_http_upstream_hash_peer_data_t)
            + sizeof(uintptr_t) * ((ngx_http_upstream_hash_peers_t *)us->peer.data)->number / (8 * sizeof(uintptr_t)));
    if (uhpd == NULL) {
        return NGX_ERROR;
    }

    r->upstream->peer.data = uhpd;

    uhpd->peers = us->peer.data;

    r->upstream->peer.free = ngx_http_upstream_free_hash_peer;
    r->upstream->peer.get = ngx_http_upstream_get_hash_peer;
    r->upstream->peer.tries = us->retries + 1;

    uhpd->hash = us->hash_function(val.data, val.len);

    return NGX_OK;
}


static ngx_int_t
ngx_http_upstream_get_hash_peer(ngx_peer_connection_t *pc, void *data)
{
    ngx_http_upstream_hash_peer_data_t  *uhpd = data;
    ngx_http_upstream_hash_peer_t       *peer;
    ngx_uint_t                           peer_index;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, pc->log, 0,
                   "get upstream request hash peer try %ui", pc->tries);

    pc->cached = 0;
    pc->connection = NULL;

    peer_index = uhpd->hash % uhpd->peers->number;

    peer = &uhpd->peers->peer[peer_index];

    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, pc->log, 0,
                   "chose peer %ui w/ hash %ui", peer_index, uhpd->hash);

    pc->sockaddr = peer->sockaddr;
    pc->socklen = peer->socklen;
    pc->name = &peer->name;

    return NGX_OK;
}

static void
ngx_http_upstream_free_hash_peer(ngx_peer_connection_t *pc, void *data,
    ngx_uint_t state)
{
    ngx_http_upstream_hash_peer_data_t  *uhpd = data;
    ngx_uint_t                           current;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, pc->log, 0, "free upstream hash peer try %ui", pc->tries);

    if (state & NGX_PEER_FAILED
            && --pc->tries)
    {
        current = uhpd->hash % uhpd->peers->number;

        uhpd->tried[ngx_bitvector_index(current)] |= ngx_bitvector_bit(current);

        do {
            uhpd->hash = ngx_hash_key((u_char *)&uhpd->hash, sizeof(ngx_uint_t));
            current = uhpd->hash % uhpd->peers->number;
        } while ((uhpd->tried[ngx_bitvector_index(current)] & ngx_bitvector_bit(current)) && --pc->tries);
    }
}

static ngx_uint_t
ngx_http_upstream_hash_crc32(u_char *data, size_t len)
{
    return ngx_crc32_short(data, len) & 0x7fff;
}

static char *
ngx_http_upstream_hash(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_upstream_srv_conf_t  *uscf;
    ngx_http_script_compile_t      sc;
    ngx_str_t                     *value;
    ngx_array_t                   *vars_lengths, *vars_values;

    value = cf->args->elts;

    ngx_memzero(&sc, sizeof(ngx_http_script_compile_t));

    vars_lengths = NULL;
    vars_values = NULL;

    sc.cf = cf;
    sc.source = &value[1];
    sc.lengths = &vars_lengths;
    sc.values = &vars_values;
    sc.complete_lengths = 1;
    sc.complete_values = 1;

    if (ngx_http_script_compile(&sc) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    uscf = ngx_http_conf_get_module_srv_conf(cf, ngx_http_upstream_module);

    uscf->peer.init_upstream = ngx_http_upstream_init_hash;

    uscf->flags = NGX_HTTP_UPSTREAM_CREATE;

    uscf->values = vars_values->elts;
    uscf->lengths = vars_lengths->elts;

    if (uscf->hash_function == NULL) {
        uscf->hash_function = ngx_hash_key;
    }

    return NGX_CONF_OK;
}

static char *
ngx_http_upstream_hash_method(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_upstream_srv_conf_t  *uscf;
    ngx_str_t *value;

    uscf = ngx_http_conf_get_module_srv_conf(cf, ngx_http_upstream_module);

    value = cf->args->elts;

    if (ngx_strcmp(value[1].data, "crc32") == 0) {
        uscf->hash_function = ngx_http_upstream_hash_crc32;
    } else if (ngx_strcmp(value[1].data, "simple") == 0) {
        uscf->hash_function = ngx_hash_key;
    } else {
        return "invalid hash method";
    }

    return NGX_CONF_OK;
}

static char *
ngx_http_upstream_hash_again(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_upstream_srv_conf_t  *uscf;
    ngx_int_t n;

    ngx_str_t *value;

    uscf = ngx_http_conf_get_module_srv_conf(cf, ngx_http_upstream_module);

    value = cf->args->elts;

    n = ngx_atoi(value[1].data, value[1].len);

    if (n == NGX_ERROR || n < 0) {
        return "invalid number";
    }

    uscf->retries = n;

    return NGX_CONF_OK;
}
