#!/usr/bin/bash

ALIAS_CHECK=$(/usr/bin/grep gitpull /system/comma/home/.bash_profile)
GET_PROP1=$(getprop persist.sys.locale)
GET_PROP2=$(getprop persist.sys.local)
GET_PROP3=$(getprop persist.sys.timezone)

if [ "$ALIAS_CHECK" == "" ]; then
    sleep 3
    mount -o remount,rw /system
    echo "alias gi='/data/openpilot/selfdrive/assets/addon/script/gitpull.sh'" >> /system/comma/home/.bash_profile
    mount -o remount,r /system
fi

if [ "$GET_PROP1" != "en-US" ]; then
    setprop persist.sys.locale en-US
fi
if [ "$GET_PROP2" != "en-US" ]; then
    setprop persist.sys.local en-US
fi
if [ "$GET_PROP3" != "America/New_York" ]; then
    setprop persist.sys.timezone America/New_York
fi

if [ ! -f "/system/fonts/opensans_regular.ttf" ]; then
    sleep 3
    mount -o remount,rw /system
  	cp -rf /data/openpilot/selfdrive/assets/fonts/opensans* /system/fonts/
    cp -rf /data/openpilot/selfdrive/assets/addon/font/fonts.xml /system/etc/fonts.xml
    chmod 644 /system/etc/fonts.xml
  	chmod 644 /system/fonts/opensans*
    mount -o remount,r /system
    reboot
fi

export PASSIVE="0"
exec ./launch_chffrplus.sh

