#!/bin/sh

TIME=$(date)
LOGDATE=$(date +"%Y-%m-%d_%H%M%S")
LOGDIR=~/Dropbox/robolock-logs
LOGFILE="$LOGDIR/alert-$LOGDATE.txt"
IMAGEFILE="$LOGDIR/alert-$LOGDATE.jpeg"
echo printing to log $LOGFILE
date > $LOGFILE

streamer -o $IMAGEFILE
