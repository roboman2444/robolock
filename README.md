# robolock
"based" on slock

to keep this from happening... https://www.youtube.com/watch?v=dHJfafgLxBw


Pretty colors now enabled

Usage:

robolock (opts)

opts are

-i IMAGEFILE	- use an image file instead of screenshot... no blurring

-b BLURSIZE	- blursize in pixels. Default 25

-t THREADS	- number of threads used for image manipulation

-c COLORS	- colorset in "#xxxxxx ..."

-s		- stealthmode (no blurring, no colors, mouse pointer still visible)

-a ALERT	- command to run when wrong password is typed in. look at samplealert.sh

-p		- weather pressing the mouse buttons trigger an alert

-P		- weather pressing then mouse button is the same as pressing enter. Overrides -p
