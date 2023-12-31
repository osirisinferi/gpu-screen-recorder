#!/bin/sh

script_dir=$(dirname "$0")
cd "$script_dir"

[ $(id -u) -ne 0 ] && echo "You need root privileges to run the install script" && exit 1

./build.sh
strip gsr-kms-server
strip gpu-screen-recorder
rm -f "/usr/local/bin/gpu-screen-recorder"
install -Dm755 "gsr-kms-server" "/usr/bin/gsr-kms-server"
install -Dm755 "gpu-screen-recorder" "/usr/bin/gpu-screen-recorder"

echo "Successfully installed gpu-screen-recorder"
