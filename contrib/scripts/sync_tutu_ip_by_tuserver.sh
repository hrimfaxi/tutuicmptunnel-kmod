#!/bin/bash

export TUTU_UID=${TUTU_UID:-102}
export HOST=${HOST:-rack.h}
export COMMENT=${COMMENT:-x360}
export PORT=${PORT:-3322}
export SERVER_PORT=${SERVER_PORT:-3321}
export PSK=${PSK:-testpassword}

IP=$(curl -s ip.3322.net)
echo local ip: $IP

V() {
	echo "$@"
	"$@"
}

V tuctl_client psk $PSK server $HOST server-port $SERVER_PORT <<<"server-add uid $TUTU_UID address $IP port $PORT comment $COMMENT"

# vim: set sw=2 expandtab:
