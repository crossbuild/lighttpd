#!/bin/sh

test x$srcdir = x && srcdir=.

. $srcdir/testbase.sh

prepare_test

cat > $TMPFILE <<EOF
Host: good name
GET / HTTP/1.0
Host: a-b.de

Status: 200
EOF

run_test
