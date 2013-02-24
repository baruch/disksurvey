SRC="coffee.coffee"
redo-ifchange $SRC
coffee -p $SRC > $3
