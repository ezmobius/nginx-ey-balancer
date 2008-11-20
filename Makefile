NGINX_DIR = "/home/ryan/src/nginx-0.6.32-maxconn-patch" # change me
THIS_DIR  = $(shell pwd)

default: compile

configure_debug: clean
	cd $(NGINX_DIR) && ./configure --with-http_ssl_module --with-debug --add-module=$(THIS_DIR) --prefix=$(THIS_DIR)/.nginx

configure: clean
	cd $(NGINX_DIR) && ./configure --with-http_ssl_module --add-module=$(THIS_DIR) --prefix=$(THIS_DIR)/.nginx

compile: .nginx/sbin/nginx

.nginx/sbin/nginx: max_connections_module.c
	cd $(NGINX_DIR) && make && make install

.PHONY: test clean restart

test/tmp:
	mkdir test/tmp

FAIL=ruby -e 'puts "\033[1;31m FAIL\033[m"'
PASS=ruby -e 'puts "\033[1;32m PASS\033[m"'

test: .nginx/sbin/nginx test/tmp
	@for i in test/test_*; do \
	  echo -n "$$i: ";	\
		ruby $$i && $(PASS) || $(FAIL); \
	done 

clean:
	-rm -rf .nginx
	-rm -f test/tmp/*

restart:
	-killall nginx
	$(NGINX_DIR)/objs/nginx -c $(THIS_DIR)/t/nginx.conf
	@echo *** NGINX restarted
	ps -HC nginx -o pid,cmd
