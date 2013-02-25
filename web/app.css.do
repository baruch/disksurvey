SRCS="disksurvey.css backgrid.css backgrid-paginator.css"
redo-ifchange $SRCS
cat $SRCS > $3
