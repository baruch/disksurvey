OBJS="src/main.o src/disk.o src/sg.o"
LDFLAGS="-lev"
redo-ifchange $OBJS
gcc $LDFLAGS -o $3 $OBJS
