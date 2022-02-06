#!/usr/bin/env bash
shopt -s extglob

for file in /opt/hfs+([0-9]).+([0-9]).+([0-9])*; do
    echo -e \\n\\n------------------ $(basename $file)\\n\\n
    HFS=$file make clean
    HFS=$file make install
    HFS=$file make clean
done;
