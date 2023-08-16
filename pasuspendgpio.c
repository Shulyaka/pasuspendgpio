#include <stdio.h>
#include <string.h>
#include <err.h>
#include <gpiod.h>

#include <pulse/pulseaudio.h>


static char *sinkname = NULL;
struct gpiod_line *line = NULL;
uint32_t sindex = 0;
const char *consumer = NULL;

static pa_context *context = NULL;
static pa_mainloop_api *mainloop_api = NULL;

enum state {UNKNOWN, SUSPENDED, UNSUSPENDED} state=UNKNOWN;

static int verbose = 1;

/* A shortcut for terminating the application */
static void quit(int ret)
{
	assert(mainloop_api);
	mainloop_api->quit(mainloop_api, ret);
}

void gpio_write(int value)
{
	if(!gpiod_line_is_requested(line))
	{
		if(gpiod_line_request_output(line, consumer, value))
		{
			warn("gpiod_line_request_output");
			quit(1);
		}
	}
	else
	{
		if(gpiod_line_set_value(line, value))
		{
			warn("gpiod_line_set_value");
			quit(1);
		}
	}
}

void set_state(enum state newstate)
{
	enum state oldstate = state;
	if(oldstate == newstate || newstate == UNKNOWN)
		return;

	gpio_write(newstate == SUSPENDED ? 0 : 1);

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
	struct gpiod_chip *chip;
	int gpio;
	char *chip_path;

	if(argc < 3 || !strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))
	{
		fprintf(stderr, "Usage: %s chip_path gpio sinkname [server]\n", argv[0]);
		return 0;
	}

	consumer = argv[0];

	chip_path = argv[1];

	gpio = atoi(argv[2]);

	sinkname = argv[3];

	if(argc > 4)
		server = argv[4];

	chip = gpiod_chip_open(chip_path);
	if (!chip)
	{
		warn("gpiod_chip_open");
		goto quit;
	}

	line = gpiod_chip_get_line(chip, gpio);
	if (!line)
	{
		warn("gpiod_chip_get_line");
		goto quit;
	}

	if(gpiod_line_is_used(line))
	{
		fprintf(stderr, "Line is already used.\n");
		goto quit;
	}

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
	if (pa_context_connect(context, server, PA_CONTEXT_NOFLAGS, NULL) < 0)
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

	if(line && gpiod_line_is_requested(line))
	{
		set_state(UNSUSPENDED);
		gpiod_line_release(line);
	}

	if(chip)
	{
		gpiod_chip_close(chip);
	}

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
