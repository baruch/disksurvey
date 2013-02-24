redo-ifchange app.css app.js index.html
sed -e '1istatic char app_css[] = "HTTP/1.1 200 OK\\r\\nContent-Type: text/css\\r\\n\\r\\n"' -e '$a;' -e 's/\\/\\\\/g;s/"/\\"/g;s/  /\\t/g;s/^/"/;s/$/\\n"/' app.css
sed -e '1istatic char app_js[] = "HTTP/1.1 200 OK\\r\\nContent-Type: text/javascript\\r\\n\\r\\n"' -e '$a;' -e 's/\\/\\\\/g;s/"/\\"/g;s/  /\\t/g;s/^/"/;s/$/\\n"/' app.js
sed -e '1istatic char index_html[] = "HTTP/1.1 200 OK\\r\\nContent-Type: text/html\\r\\n\\r\\n"' -e '$a;' -e 's/\\/\\\\/g;s/"/\\"/g;s/  /\\t/g;s/^/"/;s/$/\\n"/' index.html
