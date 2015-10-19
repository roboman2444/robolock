# robolock
"based" on slock

to keep this from happening... https://www.youtube.com/watch?v=dHJfafgLxBw


Pretty colors now enabled

##Usage:

robolock (opts)
 *   -h :: show the help documentation
 *   -i IMAGE :: use IMAGE instead of screenshot. no Blurring.
 *   -b INT :: blursize in pixels. Defaults to 25
 *   -t THREADS :: thread count for image manipulatoin. Default 8
 *   -c "COLORS" :: a space delimited list of colors that can be used for keystroke highlighting. Colors are specified in the form #RRGGBB
 *   -s :: Stealth-mode: no blurring, no colors, mouse pointer still active
 *   -a ALERT :: run $ALERT if an invalid password is entered
 *   -p :: if present, pressing a mouse button will trigger an alert
 *   -P :: if present, pressing a mouse button acts as [enter] this overrides the -p option, if applicable
 *   -l LOGSIZE :: log the output of ALERT, if present, and print the log to standard out once a correct password has been entered
