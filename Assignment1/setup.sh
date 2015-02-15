#!/usr/bin/env bash

chmod u+x ~/assign1/*.sh

# install pgbench program
cd ~/postgresql-9.4.0/contrib/pgbench
make && make install 

# install test_bufmgr extension
if [ ! -d ~/postgresql-9.4.0/contrib/test_bufmgr ]; then
	cp -r ~/assign1/test_bufmgr ~/postgresql-9.4.0/contrib
	cd ~/postgresql-9.4.0/contrib/test_bufmgr
	make && make install 
fi


