NGINX_DIR = "/home/ryan/src/nginx-0.6.31"
THIS_DIR  = $(shell pwd)

default: compile

configure: .nginx/sbin/nginx
	cd $(NGINX_DIR) && ./configure --with-debug --add-module=$(THIS_DIR) --prefix=$(THIS_DIR)/.nginx

compile: max_connections_module.c
	cd $(NGINX_DIR) && make && make install

restart:
	-killall nginx
	$(NGINX_DIR)/objs/nginx -c $(THIS_DIR)/t/nginx.conf
	@echo *** NGINX restarted
	ps -HC nginx -o pid,cmd
