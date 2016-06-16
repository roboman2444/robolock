#!/bin/sh

TIME=$(date)
LOGDATE=$(date +"%Y-%m-%d_%H%M%S")
LOGDIR=/mnt/thing1/home/roboman2444/Dropbox/robolock-logs
LOGFILE="$LOGDIR/alert-$LOGDATE.txt"
IMAGEFILE="$LOGDIR/alert-$LOGDATE.jpeg"
echo printing to log $LOGFILE
date > $LOGFILE
fswebcam $IMAGEFILE
killall xmessage
xmessage "ROBOLOCK: wrong password! last attempt: $LOGDATE" &
#xmessage `whoami` &
echo
echo done
