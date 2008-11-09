NGINX_DIR = "/home/ryan/src/nginx-0.6.31"
THIS_DIR  = $(shell pwd)

default: compile

configure_debug: 
	cd $(NGINX_DIR) && ./configure --with-debug --add-module=$(THIS_DIR) --prefix=$(THIS_DIR)/.nginx

configure: 
	cd $(NGINX_DIR) && ./configure --add-module=$(THIS_DIR) --prefix=$(THIS_DIR)/.nginx

compile: .nginx/sbin/nginx

.nginx/sbin/nginx: max_connections_module.c
	cd $(NGINX_DIR) && make && make install

.PHONY: test clean restart

test: test/test.rb test/nginx.conf.erb .nginx/sbin/nginx
	time ruby test/test.rb

clean:
	rm -rf .nginx

restart:
	-killall nginx
	$(NGINX_DIR)/objs/nginx -c $(THIS_DIR)/t/nginx.conf
	@echo *** NGINX restarted
	ps -HC nginx -o pid,cmd
