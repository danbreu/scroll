#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#ifdef XINERAMA
#include <X11/extensions/Xinerama.h>
#endif

#include <Imlib2.h>
#include <sys/types.h>

#include "utils.h"


struct scroll_vec {
	double x, y;
};
#define ABS(V) sqrt(V.x *V.x + V.y * V.y)
#define CENTER(R, A, B) \
	R.x = A.x + (B.x - A.x) / 2; \
	R.y = A.y + (B.y - A.y) / 2;

struct scroll_screen {
	int x, y;
	int width, height;
	Window window;
	Window image_window;
	int image_width, image_height;
};

enum scroll_scaling_modes { SCALE_STRETCH = 0, SCALE_FIT_HORIZ, SCALE_FIT_VERT, SCALE_END };

struct scroll_x11 {
	Display *display;
	Window root;
	Visual *visual;
	GC gc;
	Colormap colormap;
	int depth;
} x11;

Imlib_Image image = NULL;

struct scroll_screen **screens;
int num_screens;

struct scroll_opts {
	char *image;
	double scale;
	enum scroll_scaling_modes scaling_mode;
	int num_points;
	struct scroll_vec *points;
	double speed;
	int bezier;
	int bezier_res;
	int fps;
} opts = {
	NULL,
	1,
	SCALE_STRETCH,
	0,
	NULL,
	0.1/1000,
	0,
	15,
	60,
};

struct scroll_anim {
	struct scroll_vec *points;
	int num_points;
	int cur_point;
	struct scroll_vec cur_vector;
	struct scroll_vec cur_pos;
	int cur_time;
	double cur_travel_time;
} anim = {
	NULL,
	0,
	-1,
	{0, 0},
	{0, 0},
	1,
	1.0
};


/* Helpers */
static int millis(void) {
	struct timespec spec;
	clock_gettime(CLOCK_MONOTONIC, &spec);
	return spec.tv_sec * 1000 + spec.tv_nsec / 1000000;
}

void image_to_drawable(Drawable drw, Imlib_Image img, int x, int y, int w, int h,
	char dither, char blend, char alias) {
	imlib_context_set_image(img);
	imlib_context_set_drawable(drw);
	imlib_context_set_anti_alias(alias);
	imlib_context_set_dither(dither);
	imlib_context_set_blend(blend);
	imlib_context_set_angle(0);
	imlib_render_image_on_drawable_at_size(x, y, w, h);
}


struct scroll_screen *new_scroll_screen(int x, int y, int width, int height) {
	_debug("Creating screen with size (%d; %d) at (%d; %d)", width, height, x, y);
	struct scroll_screen *res = _malloc_or_die(sizeof(struct scroll_screen));

	res->x = x;
	res->y = y;
	res->width = width;
	res->height = height;

	/* Create desktop window */
	res->window = XCreateSimpleWindow(x11.display,
		x11.root,
		x, y,
		width, height,
		0, 0,
		BlackPixel(x11.display, 0));

	_check_or_die(res->window, "Failed to create window with size (%d; %d) at (%d; %d)",
		width, height, x, y);

	XSetBackground(x11.display, x11.gc, BlackPixel(x11.display, 0));

	Atom a = XInternAtom(x11.display, "_NET_WM_WINDOW_TYPE", False);
	if (a) {
		Atom a_desktop = XInternAtom(x11.display, "_NET_WM_WINDOW_TYPE_DESKTOP", False);
		XChangeProperty(x11.display, res->window, a, XA_ATOM, 32, PropModeReplace,
			(unsigned char *) &a_desktop, 1);
	}

	XMapWindow(x11.display, res->window);
	XLowerWindow(x11.display, res->window);

	/* Scale image correctly */
	imlib_context_set_image(image);
	int buf;
	double scale;

	switch (opts.scaling_mode) {
	case SCALE_FIT_VERT:
		buf = res->height * opts.scale;
		scale = (double)buf / imlib_image_get_height();
		res->image_width = imlib_image_get_width() * scale;
		res->image_height = buf;
		break;
	case SCALE_FIT_HORIZ:
		buf = res->width * opts.scale;
		scale = (double)buf / imlib_image_get_width();
		res->image_width = buf;
		res->image_height = imlib_image_get_height() * scale;
		break;
	case SCALE_STRETCH:
	default:
		res->image_width = res->width * opts.scale;
		res->image_height = res->height * opts.scale;
		break;
	}

	/* Draw image to pixmap */
	Pixmap pixmap = XCreatePixmap(x11.display, x11.root, res->image_width, res->image_height, x11.depth);
	_check_or_die(pixmap, "Failed to create pixmap");
	image_to_drawable(pixmap, image, 0, 0, res->image_width, res->image_height, 1, 1, 1);

	/* Create the "image window" which is moved around to scroll the image */
	res->image_window = XCreateSimpleWindow(x11.display,
		res->window,
		x, y,
		res->image_width,
		res->image_height,
		0, 0,
		BlackPixel(x11.display, 0));

	_check_or_die(res->image_window,
		"Failed to create image subwindow for window at (%d; %d)",
		x, y);

	XMapWindow(x11.display, res->image_window);

	XSetWindowBackgroundPixmap(x11.display, res->image_window, pixmap);
	XClearWindow(x11.display, res->image_window);
	XFlush(x11.display);

	XFreePixmap(x11.display, pixmap);

	return res;
}

void scroll_parse_points(char *point_string) {
	int len = strlen(point_string);

	opts.points = _malloc_or_die(sizeof(struct scroll_vec) * len);
	opts.num_points = 0;

	char *last = point_string, *p = point_string, buf;
	double coord_buf = 0;
	int begun_point = 0;

	while (*p++) {
		if (*p == ',' || *p == ';' || *p == '\0') {
			_check((*p == ',' && !begun_point) || ((*p == ';' || *p == '\0') && begun_point),
				"Points need two dimensions");

			buf = *p;
			*p = '\0';

			errno = 0;
			coord_buf = atof(last);
			_check(!errno, "Invalid number format");

			*p = buf;

			if (*p == ',') {
				begun_point = 1;
				opts.points[opts.num_points].x = coord_buf;
			} else {
				begun_point = 0;
				opts.points[opts.num_points].y = coord_buf;
				++opts.num_points;
			}

			last = p + 1;
		}
	}

	_check(!begun_point, "Unfinished point");

	return;

error:
	printf("Point string '%s' is invalid.\n", point_string);
	exit(1);
}

void scroll_parse_args(int argc, char **argv) {
	for (int i = 1; i < argc; i++) {
		_check(argv[i][0] == '-', "Arguments must start with -");

		int not_last = i + 1 < argc;

		switch (argv[i][1]) {
		case 'i':
			_check(not_last, "Image expected");
			opts.image = argv[++i];
			break;
		case 's':
			_check(not_last, "Scale expected");
			opts.scale = atof(argv[++i]);
			_check(opts.scale >= 1, "Scale must be greater than or equal to 1");
			break;
		case 'm':
			_check(not_last, "Scaling mode expected");
			opts.scaling_mode = atoi(argv[++i]);
			_check(0 < opts.scaling_mode && opts.scaling_mode < SCALE_END, "Scaling mode must be between 0 and %d", SCALE_END-1);
			break;
		case 'V':
			_check(not_last, "Velocity expected");
			opts.speed =	atof(argv[++i]) / 1000.0;
			_check(opts.speed > 0, "Velocity must be greater than zero");
			break;
		case 'p':
			_check(not_last, "Points expected");
			scroll_parse_points(argv[++i]);
			break;
		case 'f':
			_check(not_last, "FPS expected");
			opts.fps = atoi(argv[++i]);
			_check(opts.fps > 0, "FPS must be greater than zero");
			break;
		case 'r':
			_check(not_last, "Bezier resolution expected");
			opts.bezier_res = atoi(argv[++i]);
			_check(opts.bezier_res > 1, "Bezier resolution must be greater than one");
			break;
		case 'b':
			opts.bezier = 1;
			break;
#ifdef VERSION
		case 'v':
			printf("%s " VERSION "\nCompiled: " DATE "\n", argv[0]);
			exit(0);
			break;
#endif
		default:
			goto error;
		}
	}

	_check(opts.num_points > 1, "Need at least two points");
	_check(opts.image, "Need an image");

	return;

error:
	printf("Usage %s [-h"
#ifdef VERSION
		"|-v]"
#else
		"]"
#endif
		" [-b] [-r BEZIER RESOLUTION] [-f FPS] [-V VELOCITY] "
		"[-i IMAGE] [-s SCALE] [-p x0,y0;x1,y1;x2,y2;...]\n",
		argv[0]);
	exit(1);
}

void scroll_copy(void) {
	anim.points = opts.points;
	anim.num_points = opts.num_points;
}

void scroll_bezierify(void) {
	if (opts.num_points <= 2) {
		scroll_copy();
		return;
	}

	anim.num_points = (opts.num_points - 2) * opts.bezier_res + 2;
	anim.points = _malloc_or_die(anim.num_points * sizeof(struct scroll_vec));
	struct scroll_vec buf0, buf1;
	int p = 1;

	for (int i = 1; i < opts.num_points - 1; ++i) {
		CENTER(buf0, opts.points[i - 1], opts.points[i]);
		CENTER(buf1, opts.points[i], opts.points[i + 1]);

		double x_0_1 = buf0.x - opts.points[i].x;
		double y_0_1 = buf0.y - opts.points[i].y;

		double x_2_1 = buf1.x - opts.points[i].x;
		double y_2_1 = buf1.y - opts.points[i].y;

		double step = (1.0 / (opts.bezier_res - 1));
		for (int j = 0; j < opts.bezier_res; ++j) {
			double t = step * j;
			double t__2 = t * t;

			anim.points[p].x = opts.points[i].x + (1 - 2 * t + t__2) * x_0_1 + t__2 * x_2_1;
			anim.points[p++].y = opts.points[i].y + (1 - 2 * t + t__2) * y_0_1 + t__2 * y_2_1;
			_debug("Bezier: %d: (%f; %f)", j, anim.points[p - 1].x, anim.points[p - 1].y);
		}
	}
	memcpy(anim.points, opts.points, sizeof(struct scroll_vec));
	memcpy(anim.points + anim.num_points - 1, opts.points + opts.num_points - 1, sizeof(struct scroll_vec));
}

void scroll_setup(void) {
	/* Xlib setup */
	x11.display = XOpenDisplay(NULL);
	_check_or_die(x11.display, "Can't open display");

	x11.root = RootWindow(x11.display, DefaultScreen(x11.display));
	x11.visual = DefaultVisual(x11.display, DefaultScreen(x11.display));
	x11.depth = DefaultDepth(x11.display, DefaultScreen(x11.display));
	x11.colormap = DefaultColormap(x11.display, DefaultScreen(x11.display));
	x11.gc = XCreateGC(x11.display, x11.root, 0, NULL);

	/* Imlib setup */
	imlib_context_set_display(x11.display);
	imlib_context_set_visual(x11.visual);
	imlib_context_set_colormap(x11.colormap);
	imlib_context_set_color_modifier(NULL);
	imlib_context_set_progress_function(NULL);
	imlib_context_set_operation(IMLIB_OP_COPY);

	imlib_set_cache_size(4 * 1024 * 1024);

	image = imlib_load_image(opts.image);
	_check_or_die(image, "Can't load image");

	/* Add screens */
#ifdef XINERAMA
	if (XineramaIsActive(x11.display)) {
		XineramaScreenInfo *xinerama_screens = XineramaQueryScreens(x11.display, &num_screens);

		screens = _malloc_or_die(sizeof(struct scroll_screen *) * num_screens);

		for (int i = 0; i < num_screens; i++) {
			screens[i] = new_scroll_screen(xinerama_screens[i].x_org, xinerama_screens[i].y_org,
				xinerama_screens[i].width, xinerama_screens[i].height);
		}
	}
#else
	Screen *screen = ScreenOfDisplay(x11.display, DefaultScreen(x11.display));
	num_screens = 1;
	screens = _malloc_or_die(sizeof(struct scroll_screen *));
	screens[0] = new_scroll_screen(0, 0, screen->width, screen->height);
#endif

	/* Create bezier curve if requested */
	if (opts.bezier)
		scroll_bezierify();
	else
		scroll_copy();

	/* Adjust speed for scale */
	opts.speed /= opts.scale;
}

void scroll_step(int delta) {
	if (delta == 0)
		return;

	anim.cur_time += delta;
	double pos = anim.cur_time / anim.cur_travel_time;

	if (anim.cur_time >= anim.cur_travel_time) {
		++anim.cur_point;
		anim.cur_point %= anim.num_points;

		_debug("Overshoot: %f, %f",
			anim.points[anim.cur_point].x - anim.cur_pos.x,
			anim.points[anim.cur_point].y - anim.cur_pos.y);

		int next_point = (anim.cur_point + 1) % anim.num_points;

		anim.cur_vector.x = anim.points[next_point].x - anim.points[anim.cur_point].x;
		anim.cur_vector.y = anim.points[next_point].y - anim.points[anim.cur_point].y;

		anim.cur_travel_time = ABS(anim.cur_vector) / opts.speed;
		anim.cur_time = 0;

		_debug("Moving to point %d at (%f,%f) via vector (%f,%f) in %f millis",
			next_point, anim.points[next_point].x, anim.points[next_point].y,
			anim.cur_vector.x, anim.cur_vector.y,
			anim.cur_travel_time);

		return;
	}

	anim.cur_pos.x = anim.points[anim.cur_point].x + anim.cur_vector.x * pos;
	anim.cur_pos.y = anim.points[anim.cur_point].y + anim.cur_vector.y * pos;
}

void scroll_draw(void) {
	for (int i = 0; i < num_screens; ++i) {
		XMoveWindow(x11.display,
			screens[i]->image_window,
			(double) -(screens[i]->image_width - screens[i]->width) * anim.cur_pos.x,
			(double) -(screens[i]->image_height - screens[i]->height) * anim.cur_pos.y);
	}

	XSync(x11.display, False);
}

void scroll_run(void) {
	int millis_per_frame = 1000 / opts.fps;
	int last;
	int delta = 1000;

	for (;;) {
		if (delta >= millis_per_frame) {
			last = millis();
			scroll_step(delta);
			scroll_draw();
		}

		while (millis_per_frame > millis() - last) {
			usleep(1000);
		}

		delta = millis() - last;
	}
}

int main(int argc, char **argv) {
	scroll_parse_args(argc, argv);
	scroll_setup();
#ifdef DEBUG
	_debug("Points:");
	for (int i = 0; i < anim.num_points; i++) {
		_debug("(%f, %f)", anim.points[i].x, anim.points[i].y);
	}
#endif
	scroll_run();
	return 0;
}
