#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <getopt.h>

#include "extract/filewalk.hpp"
#include "alloc/gnew.hpp"
#include "config/cmd_args.hpp"
#include "errors.hpp"
#include "logger.hpp"

namespace extract {

void usage(const char *name) {
    printf("Usage:\n");
    printf("        %s [OPTIONS] -f data_file [-o dumpfile]\n", name);
    printf("\nOptions:\n"
           "  -h  --help                Print these usage options.\n"
           "      --force-block-size    Specifies block size, overriding file headers\n"
           "      --force-extent-size   Specifies extent size, overriding file headers\n"
           "      --force-mod-count     Specifies number of slices in *this* file,\n"
           "                            overriding file headers.\n"
           "  -f  --file                Path to file or block device where part or all of\n"
           "                            the database exists.\n"
           "  -l  --log-file            File to log to.  If not provided, messages will be\n"
           "                            printed to stderr.\n"
           "  -o  --output-file         File to which to output text memcached protocol\n"
           "                            messages.  This file must not already exist.\n");
    printf("                            Defaults to \"%s\"\n", EXTRACT_CONFIG_DEFAULT_OUTPUT_FILE);

    exit(-1);
}

enum { force_block_size = 256,  // Start these values above the ASCII range.
       force_extent_size,
       force_mod_count
};

void parse_cmd_args(int argc, char **argv, extract_config_t *config) {
    config->init();

    optind = 1;  // reinit getopt.
    for (;;) {
        int do_help = 0;
        struct option long_options[] =
            {
                {"force-block-size", required_argument, 0, force_block_size},
                {"force-extent-size", required_argument, 0, force_extent_size},
                {"force-mod-count", required_argument, 0, force_mod_count},

                {"file", required_argument, 0, 'f'},
                {"log-file", required_argument, 0, 'l'},
                {"output-file", required_argument, 0, 'o'},
                {"help", no_argument, &do_help, 1},
                {0, 0, 0, 0}
            };

        int option_index = 0;
        int c = getopt_long(argc, argv, "f:l:o:h", long_options, &option_index);

        if (do_help) {
            c = 'h';
        }

        // Detect the end of the options.
        if (c == -1)
            break;

        switch (c) {
        case 0:
            break;
        case 'f':
            config->input_file = optarg;
            break;
        case 'l':
            config->log_file = optarg;
            break;
        case 'o':
            config->output_file = optarg;
            break;
        case force_block_size: {
            char *endptr;
            config->overrides.block_size = strtol(optarg, &endptr, 10);
            if (*endptr != '\0' || config->overrides.block_size <= 0) {
                fail("Block size must be a positive integer.\n");
            }
        } break;
        case force_extent_size: {
            char *endptr;
            config->overrides.extent_size = strtol(optarg, &endptr, 10);
            if (*endptr != '\0' || config->overrides.extent_size <= 0) {
                fail("Extent size must be a positive integer.\n");
            }
        } break;
        case force_mod_count: {
            char *endptr;
            config->overrides.mod_count = strtol(optarg, &endptr, 10);
            if (*endptr != '\0' || config->overrides.mod_count <= 0) {
                fail("The mod count must be a positive integer.\n");
            }
        } break;
        case 'h':
            usage(argv[0]);
            break;

        default:
            // getopt_long already printed an error message.
            usage(argv[0]);

        }
    }

    if (optind < argc) {
        fail("Unexpected extra argument: \"%s\"", argv[optind]);
    }

    // Sanity-check the input.

    if (config->input_file.empty()) {
        fail("You must explicitly specify a path with -f.");
    }

    if (config->overrides.extent_size && config->overrides.block_size
        && (config->overrides.extent_size % config->overrides.block_size != 0)) {
        fail("The forced extent size (%d) is not a multiple of the forced block size (%d).",
             config->overrides.extent_size, config->overrides.block_size);
    }
}

// This can be treated practically like a main function, except that a
// thread pool has already been created so that the loggers work.
void extractmain(int argc, char **argv) {
    extract_config_t cfg;
    parse_cmd_args(argc, argv, &cfg);
    dumpfile(cfg);
}

void filecheck_crash_handler(int signum) {
    fail("Internal crash detected.");
}


// TODO: put run_in_loggers_fsm_t in its own file.  Maybe.
struct blocking_runner_t {
    virtual void run() = 0;
};

cmd_config_t *make_fake_config() {
    static cmd_config_t fake_config;
    init_config(&fake_config);
    return &fake_config;
}

struct run_in_loggers_fsm_t : public log_controller_t::ready_callback_t,
                              public log_controller_t::shutdown_callback_t {
    run_in_loggers_fsm_t(thread_pool_t *pool, blocking_runner_t *runner) : pool(pool), runner(runner), controller(make_fake_config()) { }
    ~run_in_loggers_fsm_t() {
        gdelete(runner);
    }

    void start() {
        if (controller.start(this)) {
            on_logger_ready();
        }
    }

    void on_logger_ready() {
        runner->run();

        if (controller.shutdown(this)) {
            on_logger_shutdown();
        }
    }

    void on_logger_shutdown() {
        pool->shutdown();
        gdelete(this);
    }

private:
    thread_pool_t *pool;
    blocking_runner_t *runner;
    log_controller_t controller;
};



struct runner : public blocking_runner_t {
    int argc;
    char **argv;
    runner(int argc, char **argv) : argc(argc), argv(argv) { }
    void run() {
        extractmain(argc, argv);
    }
};

}  // namespace extract

int main(int argc, char **argv) {

    int res;

    struct sigaction action;
    bzero((char*)&action, sizeof(action));
    action.sa_handler = extract::filecheck_crash_handler;
    res = sigaction(SIGSEGV, &action, NULL);
    check("Could not install SEGV handler", res < 0);

    // Initial CPU message to start server
    struct server_starter_t :
        public cpu_message_t
    {
        int argc;
        char **argv;
        thread_pool_t *pool;
        void on_cpu_switch() {
            extract::runner *runner = gnew<extract::runner>(argc, argv);
            extract::run_in_loggers_fsm_t *fsm = gnew<extract::run_in_loggers_fsm_t>(pool, runner);
            fsm->start();
        }
    } starter;

    starter.argc = argc;
    starter.argv = argv;

    // Run the server
    thread_pool_t thread_pool(1);
    starter.pool = &thread_pool;
    thread_pool.run(&starter);

    return 0;
}
