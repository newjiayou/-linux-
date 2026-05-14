#!/bin/bash
APP_NAME="chatserver"
APP_PATH="/home/vboxuser/桌面/projects/linux_server/build/chatserver"
log_file="/home/vboxuser/桌面/projects/linux_server/build/monitor.log"
if ! pgrep -x "$APP_NAME" > /dev/null; then
    echo "$(date): $APP_NAME is not running. Starting..." >> "$log_file"
    # 执行命令并将其放入后台
    "$APP_PATH" > /dev/null 2>&1 &
else
    echo "$(date): $APP_PATH is running." >> "$log_file"
fi