redo-ifchange protocol.proto
mkdir -p tmp && protoc-c --c_out=tmp protocol.proto
cp tmp/protocol.pb-c.c $3
cp tmp/protocol.pb-c.h .
