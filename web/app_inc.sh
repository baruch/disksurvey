#!/bin/bash

sed -e '1istatic char app_css[] = ' -e '$a;' -e 's/\\/\\\\/g;s/"/\\"/g;s/  /\\t/g;s/^/"/;s/$/\\n"/' "$1"
sed -e '1istatic char app_js[] = ' -e '$a;' -e 's/\\/\\\\/g;s/"/\\"/g;s/  /\\t/g;s/^/"/;s/$/\\n"/' "$2"
sed -e '1istatic char index_html[] = ' -e '$a;' -e 's/\\/\\\\/g;s/"/\\"/g;s/  /\\t/g;s/^/"/;s/$/\\n"/' "$3"
