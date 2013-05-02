redo-ifchange disk_mgr.o ../src/protocol.pb-c.o
gcc  -o $3 disk_mgr.o ../src/protocol.pb-c.o -lcheck -lprotobuf-c
