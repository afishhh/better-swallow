#!/usr/bin/env bash

set -euo pipefail

[ -e dwm-6.3 ] || {
	curl http://dl.suckless.org/dwm/dwm-6.3.tar.gz | tar xzvaf -
	cp -r dwm-6.3{,-orig}
	(cd dwm-6.3; patch -p1 -i ../dwm-6.3-better-swallow.diff)
}

diff -up -x dwm -x '*.o' -x compile_commands.json dwm-6.3-orig dwm-6.3 >dwm-6.3-better-swallow.diff
