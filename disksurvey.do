OBJS="src/main.o src/disk.o src/sg.o src/web.o src/disk_mgr.o"
LDFLAGS="-L . -lebb -lev"
redo-ifchange $OBJS libebb.a
gcc -o $3 $OBJS $LDFLAGS 
