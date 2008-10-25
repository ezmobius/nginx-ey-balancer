NGINX_DIR = "/home/ryan/src/nginx-0.6.31"
HASH_DIR  = $(shell pwd)

default: nginx_compile

nginx_configure:
	cd $(NGINX_DIR) && ./configure --with-debug --add-module=$(HASH_DIR) --prefix=$(HASH_DIR)/.nginx

nginx_compile: ngx_http_upstream_hash_module.c
	cd $(NGINX_DIR) && make && make install

nginx_restart:
	killall nginx
	$(NGINX_DIR)/objs/nginx -c $(HASH_DIR)/nginx/conf/nginx.conf
	@echo *** NGINX restarted
	ps -HC nginx -o pid,cmd
