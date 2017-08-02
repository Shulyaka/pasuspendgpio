#include <stdio.h>
#include <string.h>
#include <err.h>

#include <pulse/pulseaudio.h>


static char *sinkname = NULL;
int gpio = 0;
uint32_t sindex = 0;

static pa_context *context = NULL;
static pa_mainloop_api *mainloop_api = NULL;

enum state {UNKNOWN, SUSPENDED, UNSUSPENDED} state=UNKNOWN;

static int verbose = 1;

void gpio_export(int gpio)
{
	FILE* f;
	if(!(f=fopen("/sys/class/gpio/export", "w")))
		err(1, "/sys/class/gpio/export");

	if(fprintf(f, "%d\n", gpio) <= 0)
		err(1, "/sys/class/gpio/export");

	if(fclose(f))
		err(1, "/sys/class/gpio/export");

	char buf[256];
	snprintf(buf, sizeof(buf), "/sys/class/gpio/gpio%d/direction", gpio);

	if(!(f=fopen(buf, "w")))
		err(1, "%s", buf);

	if(fprintf(f, "out\n") <= 0)
		err(1, "%s", buf);

	if(fclose(f))
		err(1, "%s", buf);
}

void gpio_unexport(int gpio)
{
	FILE* f;
	if(!(f=fopen("/sys/class/gpio/unexport", "w")))
		err(1, "/sys/class/gpio/unexport");

	if(fprintf(f, "%d\n", gpio) <= 0)
		err(1, "/sys/class/gpio/unexport");

	if(fclose(f))
		err(1, "/sys/class/gpio/unexport");
}

void gpio_write(int gpio, int value)
{
	FILE* f;
	char buf[256];
	snprintf(buf, sizeof(buf), "/sys/class/gpio/gpio%d/value", gpio);

	if(!(f=fopen(buf, "w")))
		err(1, "%s", buf);

	if(fprintf(f, "%d\n", value) <= 0)
		err(1, "%s", buf);

	if(fclose(f))
		err(1, "%s", buf);
}

/* A shortcut for terminating the application */
static void quit(int ret)
{
	assert(mainloop_api);
	mainloop_api->quit(mainloop_api, ret);
}

void set_state(enum state newstate)
{
	enum state oldstate = state;
	if(oldstate == newstate || newstate == UNKNOWN)
		return;

	gpio_write(gpio, newstate == SUSPENDED ? 1 : 0);

	state = newstate;

}

static void sink_info_callback(pa_context *c, const pa_sink_info *i, int eol, void *userdata)
{
	assert(c);

	if(eol)
		return;

	assert(i);

	sindex = i->index;

	if(verbose)
		switch (i->state)
		{
			case PA_SINK_INVALID_STATE:
				fprintf(stderr, "Invalid\n");
				break;
			case PA_SINK_RUNNING:
				fprintf(stderr, "Running\n");
				break;
			case PA_SINK_IDLE:
				fprintf(stderr, "Idle\n");
				break;
			case PA_SINK_SUSPENDED:
				fprintf(stderr, "Suspended\n");
				break;
			default:
				fprintf(stderr, "Unknown\n");
		}

	set_state(i->state == PA_SINK_SUSPENDED ? SUSPENDED : UNSUSPENDED);
}

static void context_event_callback(pa_context *c, pa_subscription_event_type_t t, uint32_t idx, void *userdata)
{
	assert(c);
	assert((t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) == PA_SUBSCRIPTION_EVENT_SINK);
	if(idx != sindex)
		return;

	pa_operation_unref(pa_context_get_sink_info_by_name(c, sinkname, sink_info_callback, userdata));

}

static void context_subscribe_callback(pa_context *c, int success, void *userdata)
{
	if(success <= 0)
	{
		quit(1);
		return;
	}

	if(verbose)
		fprintf(stderr, "Subscribed to sink events\n");

	pa_operation_unref(pa_context_get_sink_info_by_name(c, sinkname, sink_info_callback, userdata));

}

/* This is called whenever the context status changes */
static void context_state_callback(pa_context *c, void *userdata)
{
	assert(c);

	switch (pa_context_get_state(c))
	{
		case PA_CONTEXT_CONNECTING:
		case PA_CONTEXT_AUTHORIZING:
		case PA_CONTEXT_SETTING_NAME:
			break;

		case PA_CONTEXT_READY:
			assert(c);

			if(verbose)
				fprintf(stderr, "Connection established.\n");

			pa_context_set_subscribe_callback(c, context_event_callback, userdata);
			pa_operation_unref(pa_context_subscribe(c, PA_SUBSCRIPTION_MASK_SINK, context_subscribe_callback, userdata));

			break;

		case PA_CONTEXT_TERMINATED:
			quit(0);
			break;

		case PA_CONTEXT_FAILED:
		default:
			fprintf(stderr, "Connection failure: %s\n", pa_strerror(pa_context_errno(c)));
			goto fail;
	}

	return;

fail:
	quit(1);

}

/* UNIX signal to quit recieved */
static void exit_signal_callback(pa_mainloop_api*m, pa_signal_event *e, int sig, void *userdata)
{
	if(verbose)
		fprintf(stderr, "Got signal, exiting.\n");
	quit(0);
}

static void sigusr1_signal_callback(pa_mainloop_api*m, pa_signal_event *e, int sig, void *userdata)
{
	return;
}

int main(int argc, char *argv[])
{
	pa_mainloop* m = NULL;
	int ret = 1, r;
	char *server = NULL;

	if(argc < 3 || !strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))
	{
		fprintf(stderr, "Usage: %s gpio sinkname [server]\n", argv[0]);
		return 0;
	}

	gpio = atoi(argv[1]);

	sinkname = argv[2];

	if(argc > 3)
		server = argv[3];

	gpio_export(gpio);

	/* Set up a new main loop */
	if (!(m = pa_mainloop_new()))
	{
		fprintf(stderr, "pa_mainloop_new() failed.\n");
		goto quit;
	}

	mainloop_api = pa_mainloop_get_api(m);

	r = pa_signal_init(mainloop_api);
	assert(r == 0);
	pa_signal_new(SIGINT, exit_signal_callback, NULL);
	pa_signal_new(SIGTERM, exit_signal_callback, NULL);
#ifdef SIGUSR1
	pa_signal_new(SIGUSR1, sigusr1_signal_callback, NULL);
#endif
#ifdef SIGPIPE
	signal(SIGPIPE, SIG_IGN);
#endif

	/* Create a new connection context */
	if (!(context = pa_context_new(mainloop_api, "pasuspendgpio")))
	{
		fprintf(stderr, "pa_context_new() failed.\n");
		goto quit;
	}

	pa_context_set_state_callback(context, context_state_callback, NULL);

	/* Connect the context */
	if (pa_context_connect(context, server, 0, NULL) < 0)
	{
		fprintf(stderr, "pa_context_connect() failed: %s\n", pa_strerror(pa_context_errno(context)));
		goto quit;
	}

	/* Run the main loop */
	if (pa_mainloop_run(m, &ret) < 0)
	{
		fprintf(stderr, "pa_mainloop_run() failed.\n");
		goto quit;
	}

quit:

	set_state(UNSUSPENDED);

	gpio_unexport(gpio);

	if (context)
	{
		pa_context_disconnect(context);
		pa_context_unref(context);
	}

	if (m)
	{
		pa_signal_done();
		pa_mainloop_free(m);
	}

	return ret;
}
