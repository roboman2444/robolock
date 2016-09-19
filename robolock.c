#define _XOPEN_SOURCE 500
#define _GNU_SOURCE

#include <security/pam_appl.h>
#include <security/pam_misc.h>
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

#define SIMD
#ifdef SIMD
#include <xmmintrin.h>
#endif

/*notes for further blur opti (rewrite of blur funcs and etc)
//pre-pass to convert to (degamma) float in a simd friendly format for the first pass (might be simd-optimized? idk, all it is is a shift, a mask, convert to float, degamma and then put in memory in a differnt place for each channel)

//no need to worry about simd running off the edge (ki int) because the simd friendly format will take care of it

//first pass outputs a rotated 90* simd-friendly version
//second blur pass converts back to int in the right direction(rotated back 90*) and does the gamma
//weighting is precomputed into a (small) table.
//tiled eventually

*/

//#define WEIGHTSTABLE
#define BENCHMARK
#ifdef BENCHMARK
#include <time.h>
#endif

#define TRUE 1
#define FALSE 0
//gamma and degamma taken from
//http://www.iquilezles.org/www/articles/gamma/gamma.htm
#include <math.h>
//#define GAMMA(a) pow((float)(a), 1.0/2.2)
//#define DEGAMMA(a) pow((float)(a), 2.2)
#define GAMMA(a) ((a))
#define DEGAMMA(a) ((a))
//#define DEGAMMA(x) ((x)*(x))
//#define GAMMA(X) sqrt((x))
typedef struct loglist {
	struct loglist *next;
	char *loginfo;
} loglist;

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
	char stealth;
	char alertpress;
	char *command;
	char print_logs_on_pwcorrect;
	unsigned int total_logs;
} options_t;

int running = TRUE;
int rr = FALSE;
int failure = FALSE;

lock_t *locks;
int nscreens = 0;
loglist *logs = NULL;
unsigned int log_count = 0;

unsigned int login(const char *password);
options_t opts = {0};




#ifdef WEIGHTSTABLE
float *weightsmiddle = 0;
float *weightstable = 0;
float weightstablesize = 0;

void genweightstable(int size){
	if(weightstable) free(weightstable);
	weightstablesize = size;
	weightstable = malloc(size * 2 * sizeof(float));
	weightsmiddle = weightstable + size;
	float blursquare = (float) (size * size);
	int kek;
	for(kek = - size; kek < size; kek++){
		int abszoff = abs(kek);
		float weight = 1.0 - (float)(abszoff*abszoff) / blursquare;
		weightsmiddle[kek] = weight;
	}
}
#endif

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

void append_log(char *logstr) {
	if (log_count == opts.total_logs) {
		if (logs) {
			free(logs->loginfo);
			logs->loginfo = strdup("logs truncated");
		}
	}
	if (log_count < opts.total_logs) {
		loglist *newlog = calloc(sizeof(loglist), 1);
		newlog->next = logs;
		newlog->loginfo = strdup(logstr);
		logs = newlog;
	}
	log_count++;
}

void print_logs() {
	loglist *tmp = logs;
	while(tmp) {
		if (tmp->loginfo) { // this should always be true, but just to be safe
			puts(tmp->loginfo);
		}
		tmp = tmp->next; // yeah, fuck it a linked list was fast to implement
	}
}

void free_logs() {
	loglist *tmp = logs;
	if (tmp) {
		logs = tmp->next;
		if (tmp->loginfo) {
			free(tmp->loginfo); // always strdup'd in
		}
		free(tmp);
		free_logs(); // wheeeeeeee
	}
}

void runcmd(char *command){
	if (command /*&& !fork()*/) {
		FILE *forkd = popen(command, "r");
		if (forkd) {
			char line[128] = {0}; // fuck you and your long lines
			if (fgets(line, 128, forkd)) {
				append_log(line); // dont worry, it is memcpyd
			}
			fclose(forkd);
		}
//		exit(0);
	}
}

char *getthepw(void){
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


int unlockscreen(Display *disp, lock_t *lock){
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
	unsigned char *d1;
	unsigned char *d2;
	int blursize;
	unsigned int numthreads;
	unsigned int mythread;
	unsigned int width;
	unsigned int height;
	unsigned int depth;
	float r, g, b, s;
	pthread_t t;
} ppargs_t;

#define TILEX 8
#define TILEY 16

void postprocessx(ppargs_t *args){

	unsigned int numthreads = args->numthreads;
	unsigned int mythread = args->mythread;
	unsigned int width = args->width;
	unsigned int height = args->height;
	unsigned int depth = args->depth;
	unsigned char *data = args->d1;
	unsigned char *input = args->d2;
	int blursize = args->blursize;
#ifndef WEIGHTSTABLE
	float blursquare = (float)(blursize * blursize);
#endif
	unsigned int x, y, yint, xint;
	for(y = mythread * TILEY; y < height; y+=numthreads * TILEY){
	for(x = 0; x < width; x += TILEX){
#ifdef SIMD
	for(yint = 0; yint < TILEY && y + yint < height; yint+=4){
		int myy = y+yint;
		int ki;
		unsigned char *ydata0, *ydata1, *ydata2, *ydata3, *yoffinput0, *yoffinput1, *yoffinput2, *yoffinput3;
		ydata0 =ydata1 = ydata2 = ydata3 = &data[(myy * width) * depth];
		yoffinput0 = yoffinput1 = yoffinput2 =yoffinput3  = &input[myy  * width * depth];
		if((ki = myy+1) < height){
			ydata1 = &data[ki * width * depth];
			yoffinput1 = &input[ki * width * depth];
		}
		if((ki = myy+2) < height){
			ydata2 = &data[ki * width * depth];
			yoffinput2 = &input[ki * width * depth];
		}
		if((ki = myy+3) < height){
			ydata3 = &data[ki * width * depth];
			yoffinput3 = &input[ki * width * depth];
		}
		for(xint = 0 ;xint < TILEX && x+xint < width; xint++){

			int myx = x+xint;
			unsigned int offs = myx * depth;

			unsigned char *xdata0 = &ydata0[offs];
			unsigned char *xdata1 = &ydata1[offs];
			unsigned char *xdata2 = &ydata2[offs];
			unsigned char *xdata3 = &ydata3[offs];

			__m128 r = _mm_setzero_ps();
			__m128 g = _mm_setzero_ps();
			__m128 b = _mm_setzero_ps();
			int xoff;
			float tweight = 0;

			for(xoff = -blursize; xoff < blursize; xoff++){
				unsigned int readin0 = ((unsigned int *)yoffinput0)[(myx + xoff) % width];
				unsigned int readin1 = ((unsigned int *)yoffinput1)[(myx + xoff) % width];
				unsigned int readin2 = ((unsigned int *)yoffinput2)[(myx + xoff) % width];
				unsigned int readin3 = ((unsigned int *)yoffinput3)[(myx + xoff) % width];
				#ifdef WEIGHTSTABLE
					float weight = weightsmiddle[xoff];
				#else
				int abszoff = abs(xoff);
				float weight = 1.0 - (float)(abszoff*abszoff) / blursquare;
				#endif
				//will do the conversion using simd later
				float in[4];
				in[0] = DEGAMMA((readin0 & 0xFF));
				in[1] = DEGAMMA((readin1 & 0xFF));
				in[2] = DEGAMMA((readin2 & 0xFF));
				in[3] = DEGAMMA((readin3 & 0xFF));
				__m128 nr = _mm_load_ps(in);
				in[0] = DEGAMMA(((readin0>>8) & 0xFF));
				in[1] = DEGAMMA(((readin1>>8) & 0xFF));
				in[2] = DEGAMMA(((readin2>>8) & 0xFF));
				in[3] = DEGAMMA(((readin3>>8) & 0xFF));
				__m128 ng = _mm_load_ps(in);
				in[0] = DEGAMMA(((readin0>>16) & 0xFF));
				in[1] = DEGAMMA(((readin1>>16) & 0xFF));
				in[2] = DEGAMMA(((readin2>>16) & 0xFF));
				in[3] = DEGAMMA(((readin3>>16) & 0xFF));
				__m128 nb = _mm_load_ps(in);

				__m128 nw = _mm_set1_ps(weight);
				r = _mm_add_ps(r, _mm_mul_ps(nr,nw));
				g = _mm_add_ps(g, _mm_mul_ps(ng,nw));
				b = _mm_add_ps(b, _mm_mul_ps(nb,nw));
				tweight += weight;
			}
			__m128 ntw = _mm_set1_ps(tweight);
			r = _mm_div_ps(r, ntw);
			g = _mm_div_ps(g, ntw);
			b = _mm_div_ps(b, ntw);
			float outr[4];
			float outg[4];
			float outb[4];
			_mm_store_ps(outr, r);
			_mm_store_ps(outg, g);
			_mm_store_ps(outb, b);

	//todo put gamma
			int result0 = (int)(outr[0]) | (int)(outg[0]) << 8 | (int)(outb[0])<<16;
			int result1 = (int)(outr[1]) | (int)(outg[1]) << 8 | (int)(outb[1])<<16;
			int result2 = (int)(outr[2]) | (int)(outg[2]) << 8 | (int)(outb[2])<<16;
			int result3 = (int)(outr[3]) | (int)(outg[3]) << 8 | (int)(outb[3])<<16;
			*((unsigned int *)xdata0) = result0;
			*((unsigned int *)xdata1) = result1;
			*((unsigned int *)xdata2) = result2;
			*((unsigned int *)xdata3) = result3;
		}
	}
#else
	for(yint = 0; yint < TILEY && y + yint < height; yint++){
		int myy = y+yint;
		unsigned char *ydata = &data[(myy * width) * depth];
		unsigned char *yoffinput = &input[myy  * width * depth];
		for(xint = 0 ;xint < TILEX && x+xint < width; xint++){

			int myx = x+xint;

			unsigned char *xdata = &ydata[myx * depth];
			int result = 0;
			float r = 0;
			float g = 0;
			float b = 0;
			int xoff;
			float tweight = 0;

			for(xoff = -blursize; xoff < blursize; xoff++){
				unsigned int readin = ((unsigned int *)yoffinput)[(myx + xoff) % width];
				#ifdef WEIGHTSTABLE
					float weight = weightsmiddle[xoff];
				#else
				int abszoff = abs(xoff);
				float weight = 1.0 - (float)(abszoff*abszoff) / blursquare;
				#endif
				r += DEGAMMA((readin & 0xFF)) * weight;
				g += DEGAMMA(((readin >> 8) & 0xFF)) * weight;
				b += DEGAMMA(((readin >> 16) & 0xFF)) * weight;
				tweight += weight;
			}
			float fr = GAMMA((float)r/tweight);
			float fg = GAMMA((float)g/tweight);
			float fb = GAMMA((float)b/tweight);

			result = (int)(fr) | (int)(fg) << 8 | (int)(fb)<<16;
			*((unsigned int *)xdata) = result;
		}
	}
	#endif
	}
	}
}

void postprocessy(ppargs_t *args){

	unsigned int numthreads = args->numthreads;
	unsigned int mythread = args->mythread;
	unsigned int width = args->width;
	unsigned int height = args->height;
	unsigned int depth = args->depth;

	unsigned char *data = args->d1;
	unsigned char *input = args->d2;
	int blursize = args->blursize;
#ifndef WEIGHTSTABLE
	float blursquare = (float)(blursize *blursize);
#endif
	unsigned int x, y, yint, xint;
	for(y = mythread * TILEY; y < height; y+=numthreads * TILEY){
	for(x = 0; x < width; x += TILEX){

#ifdef SIMDTODODOODODDODODODDODODODDOO JUST REWRITE IT ALL
	for(yint = 0; yint < TILEY && y + yint < height; yint++){
		int myy = y+yint;
		unsigned char *ydata = &data[(myy * width) * depth];
		for(xint = 0 ;xint < TILEX && x+xint < width; xint++){

			int myx = x+xint;
			int ki;
			unsigned char *xdata0, *xdata1, *xdata2, *xdata3, *yoffinput0, ,*yoffinput1, *yoffinput2, *yoffinput3;
			xdata0= xdata1 = xdata2 = xdata3 = &ydata[myx * depth];
			if((ki = myx+1) < width){
				
			}

			int result = 0;
			float r = 0;
			float g = 0;
			float b = 0;
			int yoff;
			float tweight = 0;
			for(yoff = -blursize; yoff < blursize; yoff++){
				unsigned char *yoffinput = &input[((yoff + myy) % height) * width * depth];
				unsigned int readin = ((unsigned int *)yoffinput)[myx];
				#ifdef WEIGHTSTABLE
					float weight = weightsmiddle[yoff];
				#else
				int abszoff = abs(yoff);
				float weight = 1.0 - (float)(abszoff*abszoff) / blursquare;
				#endif
				r += DEGAMMA((readin & 0xFF)) * weight;
				g += DEGAMMA(((readin >> 8) & 0xFF)) * weight;
				b += DEGAMMA(((readin >> 16) & 0xFF)) * weight;
				tweight += weight;
			}
			float fr = GAMMA((float)r/tweight);
			float fg = GAMMA((float)g/tweight);
			float fb = GAMMA((float)b/tweight);

			result = (int)(fr) | (int)(fg) << 8 | (int)(fb)<<16;
			*((unsigned int *)xdata) = result;
		}
	}
#else
	for(yint = 0; yint < TILEY && y + yint < height; yint++){
		int myy = y+yint;
		unsigned char *ydata = &data[(myy * width) * depth];
		for(xint = 0 ;xint < TILEX && x+xint < width; xint++){

			int myx = x+xint;

			unsigned char *xdata = &ydata[myx * depth];
			int result = 0;
			float r = 0;
			float g = 0;
			float b = 0;
			int yoff;
			float tweight = 0;
			for(yoff = -blursize; yoff < blursize; yoff++){
				unsigned char *yoffinput = &input[((yoff + myy) % height) * width * depth];
				unsigned int readin = ((unsigned int *)yoffinput)[myx];
				#ifdef WEIGHTSTABLE
					float weight = weightsmiddle[yoff];
				#else
				int abszoff = abs(yoff);
				float weight = 1.0 - (float)(abszoff*abszoff) / blursquare;
				#endif
				r += DEGAMMA((readin & 0xFF)) * weight;
				g += DEGAMMA(((readin >> 8) & 0xFF)) * weight;
				b += DEGAMMA(((readin >> 16) & 0xFF)) * weight;
				tweight += weight;
			}
			float fr = GAMMA((float)r/tweight);
			float fg = GAMMA((float)g/tweight);
			float fb = GAMMA((float)b/tweight);

			result = (int)(fr) | (int)(fg) << 8 | (int)(fb)<<16;
			*((unsigned int *)xdata) = result;
		}
	}
#endif
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

	unsigned char *data = args->d1;
	unsigned char *input = args->d2;


	int halfheight= height/2;
	int halfwidth = width/2;
	float halfheightsq = (float)(halfheight * halfheight);
	float halfwidthsq = (float)(halfwidth * halfwidth);

	float start = args->s;

	unsigned int x, y, yint, xint;
	for(y = mythread * TILEY; y < height; y+=numthreads * TILEY){
	for(x = 0; x < width; x += TILEX){
	for(yint = 0; yint < TILEY && y + yint < height; yint++){
		int myy = y+yint;
		unsigned char *yinput = &input[(myy * width) * depth];
		unsigned char *youtput = &data[(myy * width) * depth];
		int ydistcenter = abs(halfheight - myy);
		float ydistsq = (float)(ydistcenter * ydistcenter);
		for(xint = 0 ;xint < TILEX && x+xint < width; xint++){

			int myx = x+xint;

			int xdistcenter = abs(halfwidth - myx);
			float xdistsq = (float)(xdistcenter * xdistcenter);
			float distr = start - (ydistsq + xdistsq)/(halfheightsq + halfwidthsq) * red;
			float distg = start - (ydistsq + xdistsq)/(halfheightsq + halfwidthsq) * green;
			float distb = start - (ydistsq + xdistsq)/(halfheightsq + halfwidthsq) * blue;
			if(distr < 0.0) distr = 0.0;
			if(distg < 0.0) distg = 0.0;
			if(distb < 0.0) distb = 0.0;


			unsigned char *xinput = &yinput[myx * depth];
			unsigned char *xoutput = &youtput[myx * depth];
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
float lasts = 0.0;

int updateColor(Display *disp, lock_t *lock, float red, float green, float blue, float start){

	if(opts.threads < 1) return FALSE;

	if(lastr == red && lastg == green && lastb == blue && lasts == start) return 2;

	ppargs_t *mythreads = malloc(opts.threads * sizeof(ppargs_t));
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
		mythreads[i].s = start;
		pthread_create(&mythreads[i].t, NULL, (void *)postprocesscolor, (void *)&mythreads[i]);
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
	lasts = start;

	return TRUE;
}

int pickRandomColor(Display *disp, lock_t *lock, XColor *colors, int color_count) {
	int color = rand() % color_count;
	float r = colors[color].red / 65536.0;
	float g = colors[color].green / 65536.0;
	float b = colors[color].blue / 65536.0;
	return updateColor(disp, lock, r-1.0, g-1.0, b-1.0, 1.0);
}


int getstealth(Display *disp, lock_t *lock){
	unsigned int width = lock->width;
	unsigned int height = lock->height;
	lock->screenshot = XGetImage(disp, lock->root, 0, 0, width, height, AllPlanes, ZPixmap);
	lock->depth = lock->screenshot->depth / 8;
	if(lock->depth == 3) lock->depth = 4;
	return TRUE;
}

int getscreenshot(Display *disp, lock_t *lock){

	if(opts.threads < 1) return FALSE;

	unsigned int width = lock->width;
	unsigned int height = lock->height;
	lock->screenshot = XGetImage(disp, lock->root, 0,0, width, height, AllPlanes, ZPixmap);
	lock->depth = lock->screenshot->depth / 8;
	if(lock->depth == 3) lock->depth =4;
	unsigned char *data = malloc (width * height * lock->depth);
	lock->scrdata = malloc (width * height * lock->depth);
	unsigned char *input = (unsigned char *) lock->screenshot->data;

	memcpy(data, input, width * height * lock->depth);

	ppargs_t *mythreads = malloc(opts.threads * sizeof(ppargs_t));
#ifdef WEIGHTSTABLE
	genweightstable(opts.blur_size);
#endif
#ifdef BENCHMARK
	struct timespec tstart = {0}, tend = {0};
	clock_gettime(CLOCK_MONOTONIC, &tstart);
#endif
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
		pthread_create(&mythreads[i].t, NULL, (void *)postprocessx, (void *)&mythreads[i]);
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

#ifdef BENCHMARK
	clock_gettime(CLOCK_MONOTONIC, &tend);
	double time = ((double)tend.tv_sec + 1.0e-9 * tend.tv_nsec) - ((double)tstart.tv_sec + 1.0e-9 * tstart.tv_nsec);
	printf("time to do blur %.5f\n", time);
#endif
	free(mythreads);

	free(data);
	return TRUE;
}

void readpw(Display *disp, lock_t *locks, unsigned int numlocks){
	char buf[32], passwd[256];
	int num, screen;
	unsigned int len;
	KeySym ksym;
	XEvent ev;
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
				if(len){
                    running = !login(passwd);
					if(running){
						unsigned int i;
						for(i= 0; i <numlocks && !opts.stealth; i++){
							updateColor(disp, &locks[i], -2.0, 0.0, 0.0, 0.0);
						}
						XBell(disp, 100);
						failure = TRUE;
						if(opts.command){
							XSync(disp, True);
							runcmd(opts.command);
							XSync(disp, True);
						}
					}
					len = 0;
					break;
				}
			case XK_Escape:
				len = 0;
				unsigned int i;
				for(i= 0; i <numlocks && !opts.stealth; i++){
					updateColor(disp, &locks[i], 1.0, 1.0, 1.0, 1.0);
				}
			break;
			case XK_BackSpace:
				if(len){
					len--;
					unsigned int i;
					if(len){
						for(i= 0; i <numlocks && !opts.stealth; i++){
							if(len%2)updateColor(disp, &locks[i], 2.0, 2.0, 2.0, 1.0);
							else updateColor(disp, &locks[i], -2.0, -2.0, -2.0, 0.0);
						}
					}
				}
				//ted dont change this its beautiful
				for(i= 0; i <numlocks && !len && !opts.stealth; i++)updateColor(disp, &locks[i], 1.0, 1.0, 1.0, 1.0);

			break;
			default:
				if(num && !iscntrl((int) buf[0]) && (len + num < sizeof(passwd))){
					memcpy(passwd + len, buf, num);
					len+= num;
					unsigned int i;
					for(i= 0; i <numlocks && !opts.stealth; i++){
						if (opts.colors) {
							pickRandomColor(disp, &locks[i], opts.colors, opts.color_count);
						} else {
							updateColor(disp, &locks[i],
								((float)rand()*2.0)/(float)RAND_MAX,
								((float)rand()*2.0)/(float)RAND_MAX,
								((float)rand()*2.0)/(float)RAND_MAX, 1.0);
						}
					}
				}
			break;
			}
		} else if(ev.type == ButtonPress){
			if(opts.alertpress == 1){
				if(opts.command){
					XSync(disp, True);
					runcmd(opts.command);
					XSync(disp, True);
				}
			} else if(opts.alertpress == 2){
				passwd[len] = 0;
				if(len){
                    running = !login(passwd);
					if(running){
						unsigned int i;
						for(i= 0; i <numlocks && !opts.stealth; i++){
							updateColor(disp, &locks[i], -2.0, 0.0, 0.0, 0.0);
						}
						XBell(disp, 100);
						failure = TRUE;
						if(opts.command){
							XSync(disp, True);
							runcmd(opts.command);
							XSync(disp, True);
						}
					}
					len = 0;
				}
			}
		}
		else for(screen = 0; screen < nscreens; screen++) XRaiseWindow(disp, locks[screen].win);
	}
}


#include "stb_image.h"

int getimage(Display *disp, lock_t *lock){
	char *filename = opts.imagename;
	if(!disp || !lock) return FALSE;
	int x, y, n;
	unsigned char *imagedata = 0;
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

	if(opts.stealth){
		getstealth(disp, lock);
	} else {
		if(opts.imagename){
			getimage(disp, lock);
		} else {
			getscreenshot(disp, lock);
		}
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
	if(!opts.stealth) XDefineCursor(disp, lock->win, invisible);

	XMapRaised(disp, lock->win);
	if(cpmap)XFreePixmap(disp, cpmap);
	cpmap = 0;

	if(!opts.stealth)updateColor(disp, lock, 1.0, 1.0, 1.0, 1.0);


	int i = 1000;
	for(i = 1000; i; i--){
		if(opts.stealth && XGrabPointer(disp, lock->root, False, ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
			GrabModeAsync, GrabModeAsync, None, None, CurrentTime) == GrabSuccess) break;

		else if(XGrabPointer(disp, lock->root, False, ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
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


void usage() { //print HELP
	puts("robolock help documentation");
	puts("   -h :: show this help documentation");
	puts("   -i IMAGE :: use IMAGE instead of screenshot. no Blurring.");
	puts("   -b INT :: blursize in pixels. Defaults to 25");
	puts("   -t THREADS :: thread count for image manipulatoin. Default 8");
	puts("   -c \"COLORS\" :: a space delimited list of colors that can be");
	puts("                    used for keystroke highlighting. Colors are");
	puts("                    specified in the form #RRGGBB");
	puts("   -s :: Stealth-mode: no blurring, no colors, mouse pointer");
	puts("         still active");
	puts("   -a ALERT :: run $ALERT if an invalid password is entered");
	puts("   -p :: if present, pressing a mouse button will trigger an alert");
	puts("   -P :: if present, pressing a mouse button acts as [enter]");
	puts("         this overrides the -p option, if applicable");
	puts("   -l LOGSIZE :: log the output of ALERT, if present, and print");
	puts("                 the log to standard out once a correct password");
	puts("                 has been entered");
}

static char *_username = 0;
char *username(void) {
    return _username;
}

int main(const int argc, char ** argv){
	Display *disp;
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

		/* default logging information */
		opts.print_logs_on_pwcorrect = 0;
		opts.total_logs = 0;
	}

	int c;
	while((c = getopt(argc, argv, "b:t:l:i:c:a:spPh")) != -1) {
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
			case 's':
				opts.stealth = TRUE;
				break;
			case 'a':
				opts.command = optarg;
				break;
			case 'p':
				if(!opts.alertpress) opts.alertpress = 1;
				break;
			case 'P':
				opts.alertpress = 2;
				break;
			case 'h':
				usage();
				exit(1);
			case 'l':
				opts.print_logs_on_pwcorrect = 1;
				opts.total_logs = atoi(optarg);
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
					case 'a':
						fprintf(stderr, "-a --alert [command]: missing [command]\n");
						exit(1);
						break;
				}
				break;
		}
	}
	uid_t my_uid = getuid();
	if(!getpwuid(my_uid)){
		printf("unable to get pwuid or shit\n");
		return TRUE;
    }
    _username = getpwuid(my_uid)->pw_name;
	outofmemnokill();
	// no more pesky root for you!
	runcmd("sudo -K"); //invalidates any sudo session for this user
	if(setresuid(my_uid, my_uid, my_uid)){
		printf("setresuid failed, aborting\n");
		return TRUE;
	}

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

	readpw(disp, locks, nscreens);
	for(i = 0; i <nscreens; i++){
		unlockscreen(disp, &locks[i]);
	}
	free(locks);
	XCloseDisplay(disp);
	if (opts.print_logs_on_pwcorrect) {
		print_logs();
		free_logs();
	}
	return 0;
}

#define err(name)                                   \
    do {                                            \
        fprintf(stderr, "%s: %s\n", name,           \
                pam_strerror(pam_handle, result));  \
        end(result);                                \
        return 0;                               \
    } while (1);   

pam_handle_t *pam_handle;
static inline int end(int last_result) {
    int result = pam_end(pam_handle, last_result);
    pam_handle = 0;
    return result;
}

static int conv(int num_msg, const struct pam_message **msg,
        struct pam_response **resp, void *appdata_ptr) {
    int i;
    *resp = calloc(num_msg, sizeof(struct pam_response));
    if (*resp == NULL) {
        return PAM_BUF_ERR;
    }
    int result = PAM_SUCCESS;
    for (i = 0; i < num_msg; i++) {
        char *username, *password;
        switch (msg[i]->msg_style) {
            case PAM_PROMPT_ECHO_ON:
                username = ((char **) appdata_ptr)[0];
                (*resp)[i].resp = strdup(username);
                break;
            case PAM_PROMPT_ECHO_OFF:
                password = ((char **) appdata_ptr)[1];
                (*resp)[i].resp = strdup(password);
                break;
            case PAM_ERROR_MSG:
                fprintf(stderr, "%s\n", msg[i]->msg);
                result = PAM_CONV_ERR;
                break;
            case PAM_TEXT_INFO:
                printf("%s\n", msg[i]->msg);
                break;
        }
        if (result != PAM_SUCCESS) {
            break;
        }
    }

    if (result != PAM_SUCCESS) {
        free(*resp);
        *resp = 0;
    }

    return result;
}

unsigned int login(const char *password) {
    const char *data[2] = {username(), password};
    struct pam_conv pam_conv = {
        conv, data
    };

    int result = pam_start("ttydm", username(), &pam_conv, &pam_handle);
    if (result != PAM_SUCCESS) {
        err("pam_start");
    }

    result = pam_authenticate(pam_handle, 0);
    if (result != PAM_SUCCESS) {
        err("pam_authenticate");
    }

    result = pam_acct_mgmt(pam_handle, 0);
    if (result != PAM_SUCCESS) {
        err("pam_acct_mgmt");
    }

    result = pam_setcred(pam_handle, PAM_ESTABLISH_CRED);
    if (result != PAM_SUCCESS) {
        err("pam_setcred");
    }

    result = pam_open_session(pam_handle, 0);
    if (result != PAM_SUCCESS) {
        pam_setcred(pam_handle, PAM_DELETE_CRED);
        err("pam_open_session");
    }

    return 1;
}
