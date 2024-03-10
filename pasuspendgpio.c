#include <stdio.h>
#include <string.h>
#include <err.h>
#include <gpiod.h>

#include <pulse/pulseaudio.h>


static char *sinkname = NULL;
struct gpiod_line_request *request = NULL;
unsigned int gpio;
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

void set_state(enum state newstate)
{
	enum state oldstate = state;
	if(oldstate == newstate || newstate == UNKNOWN)
		return;

	gpiod_line_request_set_value(request, gpio, newstate == SUSPENDED ? GPIOD_LINE_VALUE_INACTIVE : GPIOD_LINE_VALUE_ACTIVE);

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
	struct gpiod_line_settings *settings = NULL;
	struct gpiod_line_config *line_cfg = NULL;
	struct gpiod_request_config *req_cfg = NULL;
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

	settings = gpiod_line_settings_new();
	if (!settings)
	{
		warn("gpiod_line_settings_new");
		goto quit;
	}

	gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_OUTPUT);

	line_cfg = gpiod_line_config_new();
	if (!line_cfg)
	{
		warn("gpiod_line_config_new");
		goto quit;
	}

	if (gpiod_line_config_add_line_settings(line_cfg, &gpio, 1, settings))
	{
		warn("gpiod_line_config_add_line_settings");
		goto quit;
	}

	req_cfg = gpiod_request_config_new();
	if (!req_cfg)
	{
		warn("gpiod_request_config_new");
		goto quit;
	}

	gpiod_request_config_set_consumer(req_cfg, argv[0]);

	request = gpiod_chip_request_lines(chip, req_cfg, line_cfg);
	if (!request)
	{
		warn("gpiod_chip_request_lines");
		goto quit;
	}

	gpiod_request_config_free(req_cfg);
	req_cfg = NULL;

	gpiod_line_config_free(line_cfg);
	line_cfg = NULL;

	gpiod_line_settings_free(settings);
	settings = NULL;

	gpiod_chip_close(chip);
	chip = NULL;

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

	if (req_cfg)
	{
		gpiod_request_config_free(req_cfg);
	}

	if (line_cfg)
	{
		gpiod_line_config_free(line_cfg);
	}

	if (settings)
	{
		gpiod_line_settings_free(settings);
	}

	if (chip)
	{
		gpiod_chip_close(chip);
	}

	if (request)
	{
		set_state(UNSUSPENDED);
		gpiod_line_request_release(request);
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
