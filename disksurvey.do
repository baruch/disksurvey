OBJS="src/main.o src/disk.o src/sg.o src/web_app.o src/disk_mgr.o src/latency.o"
LDFLAGS="-L ../libscsicmd -lscsicmd -L . -lebb -lev"
redo-ifchange $OBJS libebb.a
gcc -o $3 $OBJS $LDFLAGS 
