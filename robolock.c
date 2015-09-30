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



typedef struct lock_s {
	int screen;
	Window root, win;
	Pixmap pmap;
	GC gc;
	XImage *screenshot;
	unsigned char *scrdata;
	unsigned int depth;
	unsigned int width;
	unsigned int height;
} lock_t;

typedef struct options_s {
	unsigned int blur_size;
	unsigned int threads;
	char *imagename;
	XColor *colors;
	unsigned int color_count;
} options_t;

int running = TRUE;
int rr = FALSE;
int failure = FALSE;

lock_t *locks;
int nscreens = 0;

options_t opts = {0};


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


int unlockscreen(Display * disp, lock_t * lock){
	if(!disp || !lock) return FALSE;
	XUngrabPointer(disp, CurrentTime);
	if(lock->scrdata) free(lock->scrdata);
	if(lock->pmap)XFreePixmap(disp, lock->pmap);
	if(lock->screenshot)XDestroyImage(lock->screenshot);
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
	float r, g, b;
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

	unsigned int x, y, yint, xint;
	for(y = mythread * TILEY; y < height; y+=numthreads * TILEY){
	for(x = 0; x < width; x += TILEX){
	for(yint = 0; yint < TILEY && y + yint < height; yint++){
		int myy = y+yint;
		unsigned char * ydata = &data[(myy * width) * depth];
		for(xint = 0 ;xint < TILEX && x+xint < width; xint++){

			int myx = x+xint;

			unsigned char * xdata = &ydata[myx * depth];
			int result = 0;
			float r = 0;
			float g = 0;
			float b = 0;
			int yoff;
			float tweight = 0;
			for(yoff = -blursize; yoff < blursize; yoff++){
				unsigned char * yoffinput = &input[((yoff + myy) % height) * width * depth];
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

			result = (int)(fr) | (int)(fg) << 8 | (int)(fb)<<16;
			*((unsigned int *)xdata) = result;
		}
	}
	}
	}
}

void postprocesscolor(ppargs_t *args){
	unsigned int numthreads = args->numthreads;
	unsigned int mythread = args->mythread;
	unsigned int width = args->width;
	unsigned int height = args->height;
	unsigned int depth = args->depth;

	float red = args->r;
	float green = args->g;
	float blue = args->b;

	unsigned char * data = args->d1;
	unsigned char * input = args->d2;


	int halfheight= height/2;
	int halfwidth = width/2;
	float halfheightsq = (float)(halfheight * halfheight);
	float halfwidthsq = (float)(halfwidth * halfwidth);

	unsigned int x, y, yint, xint;
	for(y = mythread * TILEY; y < height; y+=numthreads * TILEY){
	for(x = 0; x < width; x += TILEX){
	for(yint = 0; yint < TILEY && y + yint < height; yint++){
		int myy = y+yint;
		unsigned char * yinput = &input[(myy * width) * depth];
		unsigned char * youtput = &data[(myy * width) * depth];
		int ydistcenter = abs(halfheight - myy);
		float ydistsq = (float)(ydistcenter * ydistcenter);
		for(xint = 0 ;xint < TILEX && x+xint < width; xint++){

			int myx = x+xint;

			int xdistcenter = abs(halfwidth - myx);
			float xdistsq = (float)(xdistcenter * xdistcenter);
			float distr = 1.0 - (ydistsq + xdistsq)/(halfheightsq + halfwidthsq) * red;
			float distg = 1.0 - (ydistsq + xdistsq)/(halfheightsq + halfwidthsq) * green;
			float distb = 1.0 - (ydistsq + xdistsq)/(halfheightsq + halfwidthsq) * blue;
			if(distr < 0.0) distr = 0.0;
			if(distg < 0.0) distg = 0.0;
			if(distb < 0.0) distb = 0.0;


			unsigned char * xinput = &yinput[myx * depth];
			unsigned char * xoutput = &youtput[myx * depth];
			unsigned int readin = *((unsigned int *)xinput);
			float b = (readin & 0xFF);
			float g = ((readin >> 8) & 0xFF);
			float r = ((readin >> 16) & 0xFF);

			unsigned int fb = (int)(b*distb);
			unsigned int fg = (int)(g*distg);
			unsigned int fr = (int)(r*distr);
			if(fb > 255) fb = 255;
			if(fg > 255) fg = 255;
			if(fr > 255) fr = 255;
			unsigned int result = fb | fg << 8 | fr<<16;
			*((unsigned int *)xoutput) = result;
		}
	}
	}
	}
}


float lastr = 0.0;
float lastg = 0.0;
float lastb = 0.0;

int updateColor(Display *disp, lock_t *lock, float red, float green, float blue){

	if(opts.threads < 1) return FALSE;

	if(lastr == red && lastg == green && lastb == blue) return 2;

	ppargs_t * mythreads = malloc(opts.threads * sizeof(ppargs_t));
	int i;
	for(i = 0; i < opts.threads; i++){
		mythreads[i].d1 = (unsigned char *)lock->screenshot->data;
		mythreads[i].d2 = lock->scrdata;
		mythreads[i].blursize = opts.blur_size;
		mythreads[i].numthreads = opts.threads;
		mythreads[i].mythread = i;
		mythreads[i].width = lock->width;
		mythreads[i].height = lock->height;
		mythreads[i].depth = lock->depth;
		mythreads[i].r = red;
		mythreads[i].g = green;
		mythreads[i].b = blue;
		pthread_create(&mythreads[i].t, NULL, (void * )postprocesscolor, (void *)&mythreads[i]);
	}
	for(i = 0; i < opts.threads; i++){
		pthread_join(mythreads[i].t, NULL);
	}
	XPutImage(disp, lock->pmap, lock->gc, lock->screenshot, 0, 0, 0, 0, lock->width, lock->height);
	XClearWindow(disp, lock->win);
	free(mythreads);
	lastr = red;
	lastg = green;
	lastb = blue;

	return TRUE;
}

int pickRandomColor(Display *disp, lock_t *lock, XColor *colors, int color_count) {
	int color = rand() % color_count;
	float r = colors[color].red / 65536.0;
	float g = colors[color].green / 65536.0;
	float b = colors[color].blue / 65536.0;
	return updateColor(disp, lock, r-1.0, g-1.0, b-1.0);
}


int getscreenshot(Display * disp, lock_t *lock){

	if(opts.threads < 1) return FALSE;

	unsigned int width = lock->width;
	unsigned int height = lock->height;
	lock->screenshot = XGetImage(disp, lock->root, 0,0, width, height, AllPlanes, ZPixmap);
	lock->depth = lock->screenshot->depth / 8;
	if(lock->depth == 3) lock->depth =4;
	unsigned char * data = malloc (width * height * lock->depth);
	lock->scrdata = malloc (width * height * lock->depth);
	unsigned char * input = (unsigned char *) lock->screenshot->data;

	memcpy(data, input, width * height * lock->depth);

	ppargs_t * mythreads = malloc(opts.threads * sizeof(ppargs_t));
	int i;
	for(i = 0; i < opts.threads; i++){
		mythreads[i].d1 = data;
		mythreads[i].d2 = input;
		mythreads[i].blursize = opts.blur_size;
		mythreads[i].numthreads = opts.threads;
		mythreads[i].mythread = i;
		mythreads[i].width = width;
		mythreads[i].height = height;
		mythreads[i].depth = lock->depth;
		pthread_create(&mythreads[i].t, NULL, (void * )postprocessx, (void *)&mythreads[i]);
	}
	for(i = 0; i < opts.threads; i++){
		pthread_join(mythreads[i].t, NULL);
	}
	for(i = 0; i < opts.threads; i++){
		mythreads[i].d1 = lock->scrdata;
		mythreads[i].d2 = data;
		pthread_create(&mythreads[i].t, NULL, (void *) postprocessy, (void *)&mythreads[i]);
	}
	for(i = 0; i < opts.threads; i++){
		pthread_join(mythreads[i].t, NULL);
	}

	free(mythreads);

	free(data);
	return TRUE;
}

void readpw(Display *disp, const char *pws, lock_t *locks, unsigned int numlocks){
	char buf[32], passwd[256];
	int num, screen;
	unsigned int len;
	KeySym ksym;
	XEvent ev;
	len = 0;
	running = TRUE;

	int bscolorthing = 0;



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

				running = !!strcmp(crypt(passwd, pws), pws);
				if(running){
					unsigned int i;
					for(i= 0; i <numlocks; i++){
						updateColor(disp, &locks[i], 0.0, 2.0, 2.0);
					}
					XBell(disp, 100);
					failure = TRUE;
				}
				len = 0;
			break;
			case XK_Escape:
				len = 0;
				unsigned int i;
				for(i= 0; i <numlocks; i++){
					updateColor(disp, &locks[i], 1.0, 1.0, 1.0);
				}
			break;
			case XK_BackSpace:
				if(len){
					len--;
					unsigned int i;
					if(len){
						for(i= 0; i <numlocks; i++){
							if(bscolorthing)updateColor(disp, &locks[i], -2.0, -2.0, -2.0);
							else updateColor(disp, &locks[i], 2.0, 2.0, 2.0);
						}
						bscolorthing = !bscolorthing;
					} else {
						for(i= 0; i <numlocks; i++){
							updateColor(disp, &locks[i], 1.0, 1.0, 1.0);
						}
					}
				}
			break;
			default:
				if(num && !iscntrl((int) buf[0]) && (len + num < sizeof(passwd))){
					memcpy(passwd + len, buf, num);
					len+= num;
					unsigned int i;
					for(i= 0; i <numlocks; i++){
						if (opts.colors) {
							pickRandomColor(disp, &locks[i], opts.colors, opts.color_count);
						} else {
							updateColor(disp, &locks[i],
								((float)rand()*2.0)/(float)RAND_MAX,
								((float)rand()*2.0)/(float)RAND_MAX,
								((float)rand()*2.0)/(float)RAND_MAX);
						}
					}
				}
			break;
			}
		}
		else for(screen = 0; screen < nscreens; screen++) XRaiseWindow(disp, locks[screen].win);
	}
}


#include "stb_image.h"

int getimage(Display *disp, lock_t *lock){
	char * filename = opts.imagename;
	if(!disp || !lock) return FALSE;
	int x, y, n;
	unsigned char * imagedata = 0;
	if(filename)imagedata = stbi_load(filename, &x, &y, &n, 0);

	int width = lock->width;
	int height = lock->height;

	lock->screenshot = XGetImage(disp, lock->root, 0,0, width, height, AllPlanes, ZPixmap);
	lock->depth = lock->screenshot->depth / 8;
	if(lock->depth == 3) lock->depth =4;




	lock->scrdata = malloc(lock->depth * width * height);
	unsigned char *outdata = lock->scrdata;

	int ofsy = (y-height)/2;
	int ofsx = (x-width)/2;
	if(imagedata && n){
		int ix, iy,in;


		//fill in pre-y with black
		for(iy = 0; iy < height && (iy + ofsy) < 0; iy++){
			for(ix = 0; ix < width; ix++){
				for(in = 0; in < n && in < 4; in++) outdata[(iy * width +ix) * 4 + in] = 0;
			}
		}
		for(; (iy+ofsy)<y && iy < height; iy++){
			//fill in pre-x with black
			for(ix = 0;ix < width && (ix + ofsx) < 0; ix++){
				for(in = 0; in < n && in < 4; in++) outdata[(iy * width + ix) * 4 + in] = 0;
			}
			for(; (ix+ofsx)<x && ix < width; ix++){
				for(in = 0; in < n && in < 4; in++) outdata[(iy * width + ix) * 4 + in] = imagedata[((iy+ofsy) * x + (ix+ofsx)) * n + in];
			}
			//fill in post-x with black
			for(;ix < width; ix++){
				for(in = 0; in < n && in < 4; in++) outdata[(iy * width + ix) * 4 + in] = 0;
			}
		}
		//fill in post-y with black
		for(; iy < height; iy++){
			for(ix = 0; ix < width; ix++){
				for(in = 0; in < n && in < 4; in++) outdata[(iy * width +ix) * 4 + in] = 0;
			}
		}

		stbi_image_free(imagedata);
	} else memset(outdata, 120, 4*width*height);

	memcpy(lock->screenshot->data, lock->scrdata, 4 * width * height);

	return TRUE;
}




int lockscreen(Display *disp, lock_t *lock){
	if(!disp || !lock) return FALSE;
	Cursor invisible;
	char curs[] = {0,0,0,0,0,0,0,0};
	XColor color = {0};
	XSetWindowAttributes wa;
	lock->root = RootWindow(disp, lock->screen);

	lock->width = DisplayWidth(disp, lock->screen);
	lock->height = DisplayHeight(disp, lock->screen);

	lock->gc = XCreateGC(disp, lock->root, 0, 0);

	if(opts.imagename){
		getimage(disp, lock);
	} else {
		getscreenshot(disp, lock);
	}

	lock->pmap = XCreatePixmap(disp, lock->root, lock->width, lock->height, lock->screenshot->depth);
	XPutImage(disp, lock->pmap, lock->gc, lock->screenshot, 0, 0, 0, 0, lock->width, lock->height);

	wa.override_redirect = 1;
	wa.background_pixel = 0;
	wa.background_pixmap = lock->pmap;
	XVisualInfo vinfo;
	XMatchVisualInfo(disp, lock->screen, 32, TrueColor, &vinfo);
	lock->win = XCreateWindow(disp, lock->root, 0, 0, lock->width, lock->height, 0, DefaultDepth(disp, lock->screen),
	CopyFromParent, DefaultVisual(disp, lock->screen), CWOverrideRedirect|CWBackPixmap, &wa);

	Pixmap cpmap = XCreateBitmapFromData(disp, lock->win, curs, 8, 8);
	invisible = XCreatePixmapCursor(disp, cpmap, cpmap, &color, &color, 0, 0);
	XDefineCursor(disp, lock->win, invisible);

	XMapRaised(disp, lock->win);
	if(cpmap)XFreePixmap(disp, cpmap);
	cpmap = 0;

	updateColor(disp, lock, 1.0, 1.0, 1.0);


	int i = 1000;
	for(i = 1000; i; i--){
		if(XGrabPointer(disp, lock->root, False, ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
			GrabModeAsync, GrabModeAsync, None, invisible, CurrentTime) == GrabSuccess) break;
		usleep(1000);
	}
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

int doMakeColor(XColor *at, char *str, Display *disp) {
	XVisualInfo vinfo;
	XMatchVisualInfo(disp, DefaultScreen(disp), 32, TrueColor, &vinfo);
	XSetWindowAttributes wa;
	wa.colormap = XCreateColormap(disp,
			DefaultRootWindow(disp),
			vinfo.visual,
			AllocNone);
	wa.border_pixel = 0;
	wa.background_pixel = 0;
	
	if(!XAllocNamedColor(disp, wa.colormap, str, at, at)) {
		return 0;
	}

	return 1;
}

void parseColors(char *optarg, Display *disp) {
	int rec_colors = 0;
	int max_colors = 4;
	int last_hash = -1;
	XColor *colors = malloc(sizeof(XColor) * max_colors);
	int done = 0;

	int ctr = 0;
	while(optarg[ctr]) {
		switch(optarg[ctr]) {
			case '#':
				last_hash = ctr;
				break;
			case ' ':
makenew:
				if (last_hash != -1) {
					optarg[ctr] = 0;
					if (rec_colors == max_colors) {
						max_colors *=2;
						colors = realloc(colors, sizeof(XColor) * max_colors);
					}
					doMakeColor((colors + rec_colors++)
							   ,(optarg + last_hash)
							   ,disp);
					last_hash = -1;
				}
				if (done) {
					goto fillargs;
				}
				break;
		}
		ctr++;
	}
	done = 1;
	goto makenew;
fillargs:
	opts.colors = colors;
	opts.color_count = rec_colors;
}


int main(const int argc, char ** argv){
	const char *pws = 0;
	Display * disp;
	disp = XOpenDisplay(0);
	
	{
		/* default blur size */
		opts.blur_size = 25;

		/* default thread count */
		opts.threads = 8;

		/* default image name */
		opts.imagename = 0;

		/* default color set */
		opts.color_count = 0;
		opts.colors = 0;
	}

	int c;
	while((c = getopt(argc, argv, "b:t:i:c:")) != -1) {
		switch(c) {
			case 't':
				opts.threads = atoi(optarg);
				break;
			case 'c':
				parseColors(optarg, disp);
				break;
			case 'i':
				opts.imagename = optarg;
				break;
			case 'b':
				opts.blur_size = atoi(optarg);
				break;
			case '?':
				switch(optopt) {
					case 'b':
						fprintf(stderr, "-b --blur [int]: missing [int]\n");
						exit(1);
						break;
					case 'c':
						fprintf(stderr, "-c --colorset \"#xxxxxx ...\": missing \"#xxxxxx ...\"\n");
						exit(1);
						break;
					case 'i':
						fprintf(stderr, "-i --image [path]: missing [path]\n");
						opts.imagename = "";
						break;
					case 't':
						fprintf(stderr, "-t --threads [int]: missing [int]\n");
						exit(1);
						break;
				}
				break;
		}
	}
	if(!getpwuid(getuid())){
		printf("unable to get pwuid or shit\n");
		return TRUE;
	}
	pws = getpw();
	outofmemnokill();

	nscreens = 1;
	locks = malloc(nscreens * sizeof(lock_t));

	memset(locks, 0, nscreens * sizeof(lock_t));

	if(!disp){
		printf("No display\n");
		exit(1);
	}
	int i, nlocks = 0;;
	for(i = 0; i <nscreens; i++){
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
	readpw(disp, pws, locks, nscreens);
	for(i = 0; i <nscreens; i++){
		unlockscreen(disp, &locks[i]);
	}
	free(locks);
	XCloseDisplay(disp);
	return 0;
}
