#define _XOPEN_SOURCE 500

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <pwd.h>
#include <shadow.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>

#include <X11/extensions/Xrandr.h>
#include <X11/keysym.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <pthread.h>

#include <fcntl.h>
#include <linux/oom.h>

#define TRUE 1
#define FALSE 0
//slock as a ref



typedef struct lock_s {
	int screen;
	Window root, win;
	Pixmap pmap;
	XImage * screenshot;
	char *scrdata;
	unsigned int width;
	unsigned int height;
} lock_t;

int running = TRUE;
int rr = FALSE;
int failure = FALSE;

lock_t *locks;
int nscreens = 0;


#include <linux/oom.h>
#include <fcntl.h>
int outofmemnokill(void){
	#define OOMVALLEN 64
	char value[OOMVALLEN];
	int file = open("/proc/self/oom_score_adj", O_WRONLY);
	if(file < 0 && errno == ENOENT){
		return FALSE;
	}

	int len = snprintf(value, OOMVALLEN, "%d\n", OOM_SCORE_ADJ_MIN);
	if(len >= OOMVALLEN) return FALSE; //todo
	if(file < 0 || write(file, value, len) != len || close(file) != 0) return FALSE; // todo
	return TRUE;
}

char * getpw(void){
	char *rval;
	struct passwd *pw;
	errno = 0;
	pw = getpwuid(getuid());
	if(!pw){
		if(errno){
			printf("robolock getpwuid %s\n", strerror(errno));
			exit(1);
		} else {
			printf("robolock cant get pw\n");
			exit(1);
		}
	}
	rval = pw->pw_passwd;

	if(rval[0] == 'x' && rval[1] == '\0'){
		struct spwd *sp;
		sp = getspnam(getenv("USER"));
		if(!sp){
			printf("robolock cant get shadow... did you run MAKE as root?\n");
			exit(1);
		}
		rval = sp->sp_pwdp;
	}

	if(geteuid() == 0 && ((getegid() != pw->pw_gid && setgid(pw->pw_gid) < 0) || setuid(pw->pw_uid) < 0)){
			printf("robolock cant drop privv %s\n", strerror(errno));
			exit(1);
	}
	return rval;

}

void readpw(Display *disp, const char *pws){
	char buf[32], passwd[256];
	int num, screen;
	unsigned int len, color;
	KeySym ksym;
	XEvent ev;
//	static int oldc = INIT;
	len = 0;
	running = TRUE;


	while(running && !XNextEvent(disp, &ev)){
		if(ev.type == KeyPress){
			buf[0] = 0;
			num = XLookupString(&ev.xkey, buf, sizeof(buf), &ksym, 0);
			if(IsKeypadKey(ksym)){
				if(ksym == XK_KP_Enter) ksym = XK_Return;
				else if (ksym >= XK_KP_0 && ksym <= XK_KP_9) ksym = (ksym - XK_KP_0) + XK_0;
			}
			if(IsFunctionKey(ksym) || IsKeypadKey(ksym) || IsMiscFunctionKey(ksym) || IsPFKey(ksym) || IsPrivateKeypadKey(ksym)) continue;
			switch(ksym){
			case XK_Return:
				passwd[len] = 0;
//				printf("pws %s\npasswd %s\n", pws, passwd);

				running = !!strcmp(crypt(passwd, pws), pws);
				if(running){
					//HERE
					XBell(disp, 100);
					failure = TRUE;
				}
				len = 0;
			break;
			case XK_Escape:
				len = 0;
			break;
			case XK_BackSpace:
				if(len) len--;
			break;
			default:
				if(num && !iscntrl((int) buf[0]) && (len + num < sizeof(passwd))){
					memcpy(passwd + len, buf, num);
					len+= num;
				}
			break;
			}
//			color = len ? INPUT
		} //else if (rr &
		else for(screen = 0; screen < nscreens; screen++) XRaiseWindow(disp, locks[screen].win);
	}
}



int unlockscreen(Display * disp, lock_t * lock){
	if(!disp || !lock) return FALSE;
	XUngrabPointer(disp, CurrentTime);
	//XFreeColors(disp, DefaultColormap(disp, lock->screen), lock->colors, NUMCOLS, 0);
	if(lock->pmap)XFreePixmap(disp, lock->pmap);
	if(lock->image)XDestroyImage(lock->image);
	XDestroyWindow(disp, lock->win);
	memset(lock, 0, sizeof(lock_t));
	return TRUE;
}



typedef struct ppargs_s {
	unsigned char * d1;
	unsigned char * d2;
	int blursize;
	unsigned int numthreads;
	unsigned int mythread;
	unsigned int width;
	unsigned int height;
	unsigned int depth;
	pthread_t t;
} ppargs_t;

#define TILEX 8
#define TILEY 16

void postprocessx(ppargs_t * args){

	unsigned int numthreads = args->numthreads;
	unsigned int mythread = args->mythread;
	unsigned int width = args->width;
	unsigned int height = args->height;
	unsigned int depth = args->depth;
	unsigned char * data = args->d1;
	unsigned char * input = args->d2;
	int blursize = args->blursize;

	float blursquare = (float)(blursize * blursize);

	printf("mythread %i, numthreads %i, blursize %i\n", mythread, numthreads, blursize);

	unsigned int x, y, yint, xint;
	for(y = mythread * TILEY; y < height; y+=numthreads * TILEY){
	for(x = 0; x < width; x += TILEX){
	for(yint = 0; yint < TILEY && y + yint < height; yint++){
		int myy = y+yint;
		unsigned char * ydata = &data[(myy * width) * depth];
		unsigned char * yoffinput = &input[myy  * width * depth];
		for(xint = 0 ;xint < TILEX && x+xint < width; xint++){

			int myx = x+xint;

			unsigned char * xdata = &ydata[myx * depth];
			int result = 0;
			float r = 0;
			float g = 0;
			float b = 0;
			int xoff;
			float tweight = 0;

			for(xoff = -blursize; xoff < blursize; xoff++){
				unsigned int readin = ((unsigned int *)yoffinput)[(myx + xoff) % width];
				int abszoff = abs(xoff);
				float weight = 1.0 - (float)(abszoff*abszoff) / blursquare;
				r += (readin & 0xFF) * weight;
				g += ((readin >> 8) & 0xFF) * weight;
				b += ((readin >> 16) & 0xFF) * weight;
				tweight += weight;
			}
			float fr = (float)r/tweight;
			float fg = (float)g/tweight;
			float fb = (float)b/tweight;

			result = (int)(fr) | (int)(fg) << 8 | (int)(fb)<<16;
			*((unsigned int *)xdata) = result;
		}
	}
	}
	}
}

void postprocessy(ppargs_t * args){

	unsigned int numthreads = args->numthreads;
	unsigned int mythread = args->mythread;
	unsigned int width = args->width;
	unsigned int height = args->height;
	unsigned int depth = args->depth;

	unsigned char * data = args->d1;
	unsigned char * input = args->d2;
	int blursize = args->blursize;

	float blursquare = (float)(blursize * blursize);
	int halfheight= height/2;
	int halfwidth = width/2;
	float halfheightsq = (float)(halfheight * halfheight);
	float halfwidthsq = (float)(halfwidth * halfwidth);

//	printf("mythread %i, numthreads %i\n", mythread, numthreads);

	unsigned int x, y, yint, xint;
	for(y = mythread * TILEY; y < height; y+=numthreads * TILEY){
	for(x = 0; x < width; x += TILEX){
	for(yint = 0; yint < TILEY && y + yint < height; yint++){
		int myy = y+yint;
		unsigned char * ydata = &input[(myy * width) * depth];
		int ydistcenter = abs(halfheight - myy);
		float ydistsq = (float)(ydistcenter * ydistcenter);
		for(xint = 0 ;xint < TILEX && x+xint < width; xint++){

			int myx = x+xint;

			int xdistcenter = abs(halfwidth - myx);
			float xdistsq = (float)(xdistcenter * xdistcenter);
			float distw = 1.0 - (ydistsq + xdistsq)/(halfheightsq + halfwidthsq);

			unsigned char * xdata = &ydata[myx * depth];
			int result = 0;
			float r = 0;
			float g = 0;
			float b = 0;
			int yoff;
			float tweight = 0;
			for(yoff = -blursize; yoff < blursize; yoff++){
				unsigned char * yoffinput = &data[((yoff + myy) % height) * width * depth];
				unsigned int readin = ((unsigned int *)yoffinput)[myx];
				int abszoff = abs(yoff);
				float weight = 1.0 - (float)(abszoff*abszoff) / blursquare;
				r += (readin & 0xFF) * weight;
				g += ((readin >> 8) & 0xFF) * weight;
				b += ((readin >> 16) & 0xFF) * weight;
				tweight += weight;
			}
			float fr = (float)r/tweight;
			float fg = (float)g/tweight;
			float fb = (float)b/tweight;

			result = (int)(fr*distw) | (int)(fg*distw) << 8 | (int)(fb*distw)<<16;
			*((unsigned int *)xdata) = result;
		}
	}
	}
	}
}


int getscreenshot(Display * disp, lock_t *lock, const int blursize, const unsigned int numthreads){

	if(numthreads < 1) return FALSE;

	unsigned int width = lock->width;
	unsigned int height = lock->height;
	lock->screenshot = XGetImage(disp, lock->root, 0,0, width, height, AllPlanes, ZPixmap);
	int depth = lock->screenshot->depth / 8;
	if(depth == 3) depth =4;
	//printf("adada %i\n", depth);
	unsigned char * data = malloc (width * height * depth);
	unsigned char * input = (unsigned char *) lock->screenshot->data;

	memcpy(data, input, width * height * depth);

	ppargs_t * mythreads = malloc(numthreads * sizeof(ppargs_t));
	int i;
	for(i = 0; i < numthreads; i++){
		mythreads[i].d1 = data;
		mythreads[i].d2 = input;
		mythreads[i].blursize = blursize;
		mythreads[i].numthreads = numthreads;
		mythreads[i].mythread = i;
		mythreads[i].width = width;
		mythreads[i].height = height;
		mythreads[i].depth = depth;
		pthread_create(&mythreads[i].t, NULL, (void * )postprocessx, (void *)&mythreads[i]);
	}
	for(i = 0; i < numthreads; i++){
		pthread_join(mythreads[i].t, NULL);
	}
	for(i = 0; i < numthreads; i++){
		pthread_create(&mythreads[i].t, NULL, (void *) postprocessy, (void *)&mythreads[i]);
	}
	for(i = 0; i < numthreads; i++){
		pthread_join(mythreads[i].t, NULL);
	}

	free(mythreads);

	free(data);
	return TRUE;
}

int lockscreen(Display * disp, lock_t *lock){
	if(!disp || !lock) return FALSE;
	GC gc;
//	Cursor invisible;
//	char curs[] = {0,0,0,0,0,0,0,0};
//	size_t len;
//	int i;
	XSetWindowAttributes wa;
//	for(i = 0; i < NUMCOLS; i++){
//		XAllocNamedCOlor(disp, DefaultColormap(disp, lock->screen), colorname[i], &color, &dummy);
//	}
	lock->root = RootWindow(disp, lock->screen);

	lock->width = DisplayWidth(disp, lock->screen);
	lock->height = DisplayHeight(disp, lock->screen);

	gc = XCreateGC(disp, lock->root, 0, 0);
	getscreenshot(disp, lock, 25, 8);
	lock->pmap = XCreatePixmap(disp, lock->root, lock->width, lock->height, lock->screenshot->depth);
	XPutImage(disp, lock->pmap, gc, lock->screenshot, 0, 0, 0, 0, lock->width, lock->height);

	wa.override_redirect = 1;
	wa.background_pixel = 0;
	wa.background_pixmap = lock->pmap;
	XVisualInfo vinfo;
	XMatchVisualInfo(disp, lock->screen, 32, TrueColor, &vinfo);
	lock->win = XCreateWindow(disp, lock->root, 0, 0, lock->width, lock->height, 0, DefaultDepth(disp, lock->screen),
	CopyFromParent, DefaultVisual(disp, lock->screen), CWOverrideRedirect|CWBackPixmap, &wa);

	XMapRaised(disp, lock->win);


//	if(rr) XRRSelectInput(disp, lock->win, RRScreenChangeNotifyMask);
	int i = 1000;
	/*for(i = 1000; i; i--){
		if(XGrabPointer(disp, lock->root, False, ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
			GrabModeAsync, GrabModeAsync, None, invisible, CurrentTime) == GrabSuccess) break;
		usleep(1000);
	}*/
	if(running && i){
		for(i = 1000; i; i--){
			if(XGrabKeyboard(disp, lock->root, True, GrabModeAsync, GrabModeAsync, CurrentTime) == GrabSuccess) break;
			usleep(1000);
		}
	}
	running &= (i > 0);
	if(!running){
		unlockscreen(disp, lock);
		return FALSE;
	} else XSelectInput(disp, lock->root, SubstructureNotifyMask);
	return TRUE;
}


int main(const int argc, const char ** argv){
	const char *pws = 0;
	Display * disp;
//	int screen;

	if(!getpwuid(getuid())){
		printf("unable to get pwuid or shit\n");
		return TRUE;
	}
	pws = getpw();
	outofmemnokill();

	nscreens = 1;
	locks = malloc( nscreens * sizeof(lock_t));

	memset(locks, 0, nscreens * sizeof(lock_t));

	disp = XOpenDisplay(0);
	if(!disp){
		printf("No display\n");
		exit(1);
	}
	int i, nlocks = 0;;
	for(i = 0; i <nscreens; i++){
//		lockscreen(disp, &lock);
		locks[i].screen = i;
		if(lockscreen(disp, &locks[i])) nlocks++;
	}
	XSync(disp, FALSE);
	if(!nlocks){
		printf("no lock!\n");

		free(locks);
		XCloseDisplay(disp);
		return 1;
	}
	printf("locked!\n");
	readpw(disp, pws);
	for(i = 0; i <nscreens; i++){
//		lockscreen(disp, &lock);
		unlockscreen(disp, &locks[i]);
	}
	free(locks);
	XCloseDisplay(disp);
	return 0;
}
