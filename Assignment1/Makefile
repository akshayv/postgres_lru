CPP=gcc
OPTS=-g -Wall
LIBS=-lresolv -ldl -lm

# Modify PSQLPATH if necesssary
PSQLPATH=/home/cy/postgresql-9.4.0

INCLUDE=-I$(PSQLPATH)/src/include     

freelist-lru.o: freelist-lru.c
	$(CPP) $(OPTS) $(INCLUDE) -c -o freelist-lru.o freelist-lru.c

clean:
	rm -f *.o

lru: copylru pgsql

clock: copyclock pgsql

copylru:
	cp freelist-lru.c $(PSQLPATH)/src/backend/storage/buffer/freelist.c
	cp bufmgr.c $(PSQLPATH)/src/backend/storage/buffer/bufmgr.c

copyclock:
	cp freelist.original.c $(PSQLPATH)/src/backend/storage/buffer/freelist.c
	cp bufmgr.original.c $(PSQLPATH)/src/backend/storage/buffer/bufmgr.c

pgsql:
	cd $(PSQLPATH) && make && make install

