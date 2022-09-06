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

enum scroll_scaling_modes {
	SCALE_STRETCH = 0,
	SCALE_FIT_HORIZ,
	SCALE_FIT_VERT,
	SCALE_END
};

struct scroll_x11 {
	Display *display;
	Window root;
	Visual *visual;
	GC gc;
	Colormap colormap;
	int depth;
};

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
};

struct scroll_anim {
	struct scroll_vec *points;
	int num_points;
	int cur_point;
	struct scroll_vec cur_vector;
	struct scroll_vec cur_pos;
	int cur_time;
	double cur_travel_time;
};

struct scroll_ctx {
	struct scroll_x11 x11;

	struct scroll_screen **screens;
	int num_screens;

	struct scroll_anim anim;

	Imlib_Image image;

	struct scroll_opts opts;
};

struct scroll_ctx *scroll_init_ctx(struct scroll_ctx *ctx) {
	ctx->screens = NULL;
	ctx->num_screens = 0;

	ctx->anim = (struct scroll_anim) {
		NULL,
		0,
		-1,
		{0, 0},
		{0, 0},
		1,
		1.0
	};

	ctx->image = NULL;

	ctx->opts = (struct scroll_opts) {
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

	return ctx;
}


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


struct scroll_screen *new_scroll_screen(struct scroll_ctx *ctx, int x, int y, int width, int height) {
	_debug("Creating screen with size (%d; %d) at (%d; %d)", width, height, x, y);
	struct scroll_screen *res = malloc(sizeof(struct scroll_screen));

	res->x = x;
	res->y = y;
	res->width = width;
	res->height = height;

	/* Create desktop window */
	res->window = XCreateSimpleWindow(ctx->x11.display,
		ctx->x11.root,
		x, y,
		width, height,
		0, 0,
		BlackPixel(ctx->x11.display, 0));

	_check_or_die(res->window, "Failed to create window with size (%d; %d) at (%d; %d)",
		width, height, x, y);

	XSetBackground(ctx->x11.display, ctx->x11.gc, BlackPixel(ctx->x11.display, 0));

	Atom a = XInternAtom(ctx->x11.display, "_NET_WM_WINDOW_TYPE", False);
	if (a) {
		Atom a_desktop = XInternAtom(ctx->x11.display, "_NET_WM_WINDOW_TYPE_DESKTOP", False);
		XChangeProperty(ctx->x11.display, res->window, a, XA_ATOM, 32, PropModeReplace,
			(unsigned char *) &a_desktop, 1);
	}

	XMapWindow(ctx->x11.display, res->window);
	XLowerWindow(ctx->x11.display, res->window);

	/* Scale image correctly */
	imlib_context_set_image(ctx->image);
	int buf;
	double scale;

	switch (ctx->opts.scaling_mode) {
	case SCALE_FIT_VERT:
		buf = res->height * ctx->opts.scale;
		scale = (double)buf / imlib_image_get_height();
		res->image_width = imlib_image_get_width() * scale;
		res->image_height = buf;
		break;
	case SCALE_FIT_HORIZ:
		buf = res->width * ctx->opts.scale;
		scale = (double)buf / imlib_image_get_width();
		res->image_width = buf;
		res->image_height = imlib_image_get_height() * scale;
		break;
	case SCALE_STRETCH:
	default:
		res->image_width = res->width * ctx->opts.scale;
		res->image_height = res->height * ctx->opts.scale;
		break;
	}

	/* Draw image to pixmap */
	Pixmap pixmap = XCreatePixmap(ctx->x11.display, ctx->x11.root, res->image_width, res->image_height, ctx->x11.depth);
	_check_or_die(pixmap, "Failed to create pixmap");
	image_to_drawable(pixmap, ctx->image, 0, 0, res->image_width, res->image_height, 1, 1, 1);

	/* Create the "image window" which is moved around to scroll the image */
	res->image_window = XCreateSimpleWindow(ctx->x11.display,
		res->window,
		x, y,
		res->image_width,
		res->image_height,
		0, 0,
		BlackPixel(ctx->x11.display, 0));

	_check_or_die(res->image_window,
		"Failed to create image subwindow for window at (%d; %d)",
		x, y);

	XMapWindow(ctx->x11.display, res->image_window);

	XSetWindowBackgroundPixmap(ctx->x11.display, res->image_window, pixmap);
	XClearWindow(ctx->x11.display, res->image_window);
	XFlush(ctx->x11.display);

	XFreePixmap(ctx->x11.display, pixmap);

	return res;
}

void scroll_parse_points(struct scroll_ctx *ctx, char *point_string) {
	int len = strlen(point_string);

	ctx->opts.points = malloc(sizeof(struct scroll_vec) * len);
	ctx->opts.num_points = 0;

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
				ctx->opts.points[ctx->opts.num_points].x = coord_buf;
			} else {
				begun_point = 0;
				ctx->opts.points[ctx->opts.num_points].y = coord_buf;
				++ctx->opts.num_points;
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

void scroll_parse_args(struct scroll_ctx *ctx, int argc, char **argv) {
	for (int i = 1; i < argc; i++) {
		_check(argv[i][0] == '-', "Arguments must start with -");

		int not_last = i + 1 < argc;

		switch (argv[i][1]) {
		case 'i':
			_check(not_last, "Image expected");
			ctx->opts.image = argv[++i];
			break;
		case 's':
			_check(not_last, "Scale expected");
			ctx->opts.scale = atof(argv[++i]);
			_check(ctx->opts.scale >= 1, "Scale must be greater than or equal to 1");
			break;
		case 'm':
			_check(not_last, "Scaling mode expected");
			ctx->opts.scaling_mode = atoi(argv[++i]);
			_check(0 < ctx->opts.scaling_mode && ctx->opts.scaling_mode < SCALE_END, "Scaling mode must be between 0 and %d", SCALE_END-1);
			break;
		case 'V':
			_check(not_last, "Velocity expected");
			ctx->opts.speed = atof(argv[++i]) / 1000.0;
			_check(ctx->opts.speed > 0, "Velocity must be greater than zero");
			break;
		case 'p':
			_check(not_last, "Points expected");
			scroll_parse_points(ctx, argv[++i]);
			break;
		case 'f':
			_check(not_last, "FPS expected");
			ctx->opts.fps = atoi(argv[++i]);
			_check(ctx->opts.fps > 0, "FPS must be greater than zero");
			break;
		case 'r':
			_check(not_last, "Bezier resolution expected");
			ctx->opts.bezier_res = atoi(argv[++i]);
			_check(ctx->opts.bezier_res > 1, "Bezier resolution must be greater than one");
			break;
		case 'b':
			ctx->opts.bezier = 1;
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

	_check(ctx->opts.num_points > 1, "Need at least two points");
	_check(ctx->opts.image, "Need an image");

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

void scroll_copy(struct scroll_ctx *ctx) {
	ctx->anim.points = ctx->opts.points;
	ctx->anim.num_points = ctx->opts.num_points;
}

void scroll_bezierify(struct scroll_ctx *ctx) {
	if (ctx->opts.num_points <= 2) {
		scroll_copy(ctx);
		return;
	}

	ctx->anim.num_points = (ctx->opts.num_points - 2) * ctx->opts.bezier_res + 2;
	ctx->anim.points = malloc(ctx->anim.num_points * sizeof(struct scroll_vec));
	struct scroll_vec buf0, buf1;
	int p = 1;

	for (int i = 1; i < ctx->opts.num_points - 1; ++i) {
		CENTER(buf0, ctx->opts.points[i - 1], ctx->opts.points[i]);
		CENTER(buf1, ctx->opts.points[i], ctx->opts.points[i + 1]);

		double x_0_1 = buf0.x - ctx->opts.points[i].x;
		double y_0_1 = buf0.y - ctx->opts.points[i].y;

		double x_2_1 = buf1.x - ctx->opts.points[i].x;
		double y_2_1 = buf1.y - ctx->opts.points[i].y;

		double step = (1.0 / (ctx->opts.bezier_res - 1));
		for (int j = 0; j < ctx->opts.bezier_res; ++j) {
			double t = step * j;
			double t__2 = t * t;

			ctx->anim.points[p].x = ctx->opts.points[i].x + (1 - 2 * t + t__2) * x_0_1 + t__2 * x_2_1;
			ctx->anim.points[p++].y = ctx->opts.points[i].y + (1 - 2 * t + t__2) * y_0_1 + t__2 * y_2_1;
			_debug("Bezier: %d: (%f; %f)", j, ctx->anim.points[p - 1].x, ctx->anim.points[p - 1].y);
		}
	}
	memcpy(ctx->anim.points, ctx->opts.points, sizeof(struct scroll_vec));
	memcpy(ctx->anim.points + ctx->anim.num_points - 1, ctx->opts.points + ctx->opts.num_points - 1, sizeof(struct scroll_vec));
}

void scroll_init_x11(struct scroll_ctx *ctx) {
	ctx->x11.display = XOpenDisplay(NULL);
	_check_or_die(ctx->x11.display, "Can't open display");

	ctx->x11.root = RootWindow(ctx->x11.display, DefaultScreen(ctx->x11.display));
	ctx->x11.visual = DefaultVisual(ctx->x11.display, DefaultScreen(ctx->x11.display));
	ctx->x11.depth = DefaultDepth(ctx->x11.display, DefaultScreen(ctx->x11.display));
	ctx->x11.colormap = DefaultColormap(ctx->x11.display, DefaultScreen(ctx->x11.display));
	ctx->x11.gc = XCreateGC(ctx->x11.display, ctx->x11.root, 0, NULL);
}

void scroll_init_imlib(struct scroll_ctx *ctx) {
	imlib_context_set_display(ctx->x11.display);
	imlib_context_set_visual(ctx->x11.visual);
	imlib_context_set_colormap(ctx->x11.colormap);
	imlib_context_set_color_modifier(NULL);
	imlib_context_set_progress_function(NULL);
	imlib_context_set_operation(IMLIB_OP_COPY);

	imlib_set_cache_size(4 * 1024 * 1024);

	ctx->image = imlib_load_image(ctx->opts.image);
	_check_or_die(ctx->image, "Can't load image");
}

void scroll_init_screens(struct scroll_ctx *ctx) {
	/* Add a window for every screen */
#ifdef XINERAMA
	if (XineramaIsActive(ctx->x11.display)) {
		XineramaScreenInfo *xinerama_screens = XineramaQueryScreens(ctx->x11.display, &ctx->num_screens);

		ctx->screens = malloc(sizeof(struct scroll_screen *) * ctx->num_screens);

		for (int i = 0; i < ctx->num_screens; i++) {
		  ctx->screens[i] = new_scroll_screen(ctx, xinerama_screens[i].x_org, xinerama_screens[i].y_org,
				xinerama_screens[i].width, xinerama_screens[i].height);
		}
	}
#else
	Screen *screen = ScreenOfDisplay(ctx->x11.display, DefaultScreen(ctx->x11.display));
	ctx->num_screens = 1;
	ctx->screens = malloc(sizeof(struct scroll_screen *));
	ctx->screens[0] = new_scroll_screen(ctx, 0, 0, screen->width, screen->height);
#endif
}

void scroll_setup(struct scroll_ctx *ctx) {
	scroll_init_x11(ctx);
	scroll_init_imlib(ctx);
	scroll_init_screens(ctx);

	/* Create bezier curve if requested */
	if (ctx->opts.bezier)
		scroll_bezierify(ctx);
	else
		scroll_copy(ctx);

	/* Adjust speed for scale */
	ctx->opts.speed /= ctx->opts.scale;
}

void scroll_step(struct scroll_ctx *ctx, int delta) {
	if (delta == 0)
		return;

	ctx->anim.cur_time += delta;
	double pos = ctx->anim.cur_time / ctx->anim.cur_travel_time;

	if (ctx->anim.cur_time >= ctx->anim.cur_travel_time) {
		++ctx->anim.cur_point;
		ctx->anim.cur_point %= ctx->anim.num_points;

		_debug("Overshoot: %f, %f",
			ctx->anim.points[ctx->anim.cur_point].x - ctx->anim.cur_pos.x,
			ctx->anim.points[ctx->anim.cur_point].y - ctx->anim.cur_pos.y);

		int next_point = (ctx->anim.cur_point + 1) % ctx->anim.num_points;

		ctx->anim.cur_vector.x = ctx->anim.points[next_point].x - ctx->anim.points[ctx->anim.cur_point].x;
		ctx->anim.cur_vector.y = ctx->anim.points[next_point].y - ctx->anim.points[ctx->anim.cur_point].y;

		ctx->anim.cur_travel_time = ABS(ctx->anim.cur_vector) / ctx->opts.speed;
		ctx->anim.cur_time = 0;

		_debug("Moving to point %d at (%f,%f) via vector (%f,%f) in %f millis",
			next_point, ctx->anim.points[next_point].x, ctx->anim.points[next_point].y,
			ctx->anim.cur_vector.x, ctx->anim.cur_vector.y,
			ctx->anim.cur_travel_time);

		return;
	}

	ctx->anim.cur_pos.x = ctx->anim.points[ctx->anim.cur_point].x + ctx->anim.cur_vector.x * pos;
	ctx->anim.cur_pos.y = ctx->anim.points[ctx->anim.cur_point].y + ctx->anim.cur_vector.y * pos;
}

void scroll_draw(struct scroll_ctx *ctx) {
	for (int i = 0; i < ctx->num_screens; ++i) {
		XMoveWindow(ctx->x11.display,
			ctx->screens[i]->image_window,
			(double) -(ctx->screens[i]->image_width - ctx->screens[i]->width) * ctx->anim.cur_pos.x,
			(double) -(ctx->screens[i]->image_height - ctx->screens[i]->height) * ctx->anim.cur_pos.y);
	}

	XSync(ctx->x11.display, False);
}

void scroll_run(struct scroll_ctx *ctx) {
	int millis_per_frame = 1000 / ctx->opts.fps;
	int last;
	int delta = 1000;

	for (;;) {
		if (delta >= millis_per_frame) {
			last = millis();
			scroll_step(ctx, delta);
			scroll_draw(ctx);
		}

		while (millis_per_frame > millis() - last) {
			usleep(1000);
		}

		delta = millis() - last;
	}
}

int main(int argc, char **argv) {
	struct scroll_ctx ctx;
	scroll_init_ctx(&ctx);

	scroll_parse_args(&ctx, argc, argv);
	scroll_setup(&ctx);
#ifdef DEBUG
	_debug("Points:");
	for (int i = 0; i < ctx.anim.num_points; i++) {
	  _debug("(%f, %f)", ctx.anim.points[i].x, ctx.anim.points[i].y);
	}
#endif
	scroll_run(&ctx);
	return 0;
}
