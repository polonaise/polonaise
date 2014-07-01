#!/bin/bash
set -eu
: ${THRIFT_VERSION:=0.9.1}
: ${THRIFT_INSTALL_PREFIX:=/opt/thrift-$THRIFT_VERSION}
: ${CONCURRENCY_LEVEL:=2}
workdir="thrift-$THRIFT_VERSION"

if [[ $# != 1 || $1 != compile ]]; then
	echo "Usage: $0 compile"
	echo "Compiles thrift in directory $workdir"
	echo "Requirements are described here: http://thrift.apache.org/docs/install"
	echo "               For Ubuntu users: http://thrift.apache.org/docs/install/debian"
	echo
	echo "Environmental variables:"
	echo "THRIFT_VERSION = $THRIFT_VERSION"
	echo "THRIFT_INSTALL_PREFIX = $THRIFT_INSTALL_PREFIX"
	echo "CONCURRENCY_LEVEL = $CONCURRENCY_LEVEL"
	exit 1
fi >&2

if [[ -e $workdir ]]; then
	echo "Directory $workdir already exists, exiting!"
	echo "Remove it if you want to do everything once again."
	exit 1
fi >&2
wget ftp://ftp.task.gda.pl/pub/www/apache/dist/thrift/$THRIFT_VERSION/thrift-$THRIFT_VERSION.tar.gz
tar -zxvf thrift-$THRIFT_VERSION.tar.gz
mv thrift-$THRIFT_VERSION.tar.gz $workdir
cd $workdir
./configure --prefix=$THRIFT_INSTALL_PREFIX \
	--without-tests   --without-python  --without-go \
	--without-c_glib  --without-csharp  --without-java \
	--without-erlang  --without-perl    --without-php_extension \
	--without-ruby    --without-haskell --without-php
make -j$CONCURRENCY_LEVEL

echo "=========================================="
echo "Success. Now run:"
echo " [sudo] make -C $workdir install"
echo "=========================================="
