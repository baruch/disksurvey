SRCS="jquery-1.6.1.min.js underscore.js backbone.js handlebars.js coffee.js"
#SRCS="jquery-1.6.1.min.js underscore.js backbone.js backbone.localstorage.js handlebars.js coffee.js"
redo-ifchange $SRCS
cat $SRCS
