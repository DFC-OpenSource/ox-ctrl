#include <argp.h>
#include <stdint.h>
#include <string.h>

#include "include/ssd.h"

extern struct core_struct core;
const char *argp_program_version = "ox-ctrl 1.0";
const char *argp_program_bug_address = "Ivan L. Picoli <ivpi@itu.dk>";

enum cmdtypes {
    CMDARG_START = 1,
    CMDARG_TEST,
    CMDARG_DEBUG,
    CMDARG_ADMIN
};

static char doc_global[] = "\n*** OX Controller ***\n"
        " \n An OpenChannel SSD Controller\n\n"
        " Available commands:\n"
        "  start            Start controller with standard configuration\n"
        "  debug            Start controller and print Admin/IO commands\n"
        "  test             Start controller, run tests and close\n"
        "  admin            Execute specific tasks within the controller\n"
        " \n Initial release developed by Ivan L. Picoli, the red-eagle\n\n";

static char doc_test[] =
        "\nUse this command to run tests, it will start the controller,"
        " run the tests and close the controller.\n"
        "\n Examples:"
        "\n  Show all available set of tests + subtests:"
        "\n    ox-ctrl test -l\n"
        "\n  Run all available tests:"
        "\n    ox-ctrl test -a\n"
        "\n  Run a specific test set:"
        "\n    ox-ctrl test -s <set_name>\n"
        "\n  Run a specific subtest:"
        "\n    ox-ctrl test -s <set_name> -t <subtest_name>";

static char doc_admin[] =
        "\nUse this command to run specific tasks within the controller.\n"
        "\n Examples:"
        "\n  Show all available Admin tasks:"
        "\n    ox-ctrl admin -l\n"
        "\n  Run a specific admin task:"
        "\n    ox-ctrl admin -t <task_name>";

static struct argp_option opt_test[] = {
    {"list", 'l', "list", OPTION_ARG_OPTIONAL,"Show available tests."},
    {"all", 'a', "run_all", OPTION_ARG_OPTIONAL, "Use to run all tests."},
    {"set", 's', "test_set", 0, "Test set name. <char>"},
    {"test", 't', "test", 0, "Subtest name. <char>"},
    {0}
};

static struct argp_option opt_admin[] = {
    {"list", 'l', "list", OPTION_ARG_OPTIONAL,"Show available admin tasks."},
    {"task", 't', "admin_task", 0, "Admin task to be executed. <char>"},
    {0}
};

static error_t parse_opt_test(int key, char *arg, struct argp_state *state)
{
    struct nvm_init_arg *args = state->input;

    switch (key) {
        case 's':
            if (!arg || strlen(arg) == 0 || strlen(arg) > CMDARG_LEN) {
                argp_usage(state);
            }
            strcpy(args->test_setname,arg);
            args->arg_num++;
            args->arg_flag |= CMDARG_FLAG_S;
            break;
        case 't':
            if (!arg || strlen(arg) == 0 || strlen(arg) > CMDARG_LEN)
                argp_usage(state);
            strcpy(args->test_subtest,arg);
            args->arg_num++;
            args->arg_flag |= CMDARG_FLAG_T;
            break;
        case 'a':
            args->arg_num++;
            args->arg_flag |= CMDARG_FLAG_A;
            break;
        case 'l':
            args->arg_num++;
            args->arg_flag |= CMDARG_FLAG_L;
            break;
        case ARGP_KEY_END:
            if (args->arg_num > 2 || args->arg_num == 0)
                argp_usage(state);
            if (args->arg_flag & CMDARG_FLAG_A && args->arg_num > 1)
                argp_usage(state);
            if (args->arg_flag & CMDARG_FLAG_L && args->arg_num > 1)
                argp_usage(state);
            if (args->arg_flag & CMDARG_FLAG_T && args->arg_num != 2)
                argp_usage(state);
            break;
        case ARGP_KEY_ARG:
        case ARGP_KEY_NO_ARGS:
        case ARGP_KEY_ERROR:
        case ARGP_KEY_SUCCESS:
        case ARGP_KEY_FINI:
        case ARGP_KEY_INIT:
            break;
        default:
            return ARGP_ERR_UNKNOWN;
    }

    return 0;
}

static error_t parse_opt_admin(int key, char *arg, struct argp_state *state)
{
    struct nvm_init_arg *args = state->input;

    switch (key) {
        case 't':
            if (!arg || strlen(arg) == 0 || strlen(arg) > CMDARG_LEN)
                argp_usage(state);
            strcpy(args->admin_task,arg);
            args->arg_num++;
            args->arg_flag |= CMDARG_FLAG_T;
            break;
        case 'l':
            args->arg_num++;
            args->arg_flag |= CMDARG_FLAG_L;
            break;
        case ARGP_KEY_END:
            if (args->arg_num > 1 || args->arg_num == 0)
                argp_usage(state);
            break;
        case ARGP_KEY_ARG:
        case ARGP_KEY_NO_ARGS:
        case ARGP_KEY_ERROR:
        case ARGP_KEY_SUCCESS:
        case ARGP_KEY_FINI:
        case ARGP_KEY_INIT:
            break;
        default:
            return ARGP_ERR_UNKNOWN;
    }

    return 0;
}

static void cmd_prepare(struct argp_state *state, struct nvm_init_arg *args,
                                              char *cmd, struct argp *argp_cmd)
{
    /* Remove the first arg from the parser */
    int argc = state->argc - state->next + 1;
    char** argv = &state->argv[state->next - 1];
    char* argv0 = argv[0];

    argv[0] = malloc(strlen(state->name) + strlen(cmd) + 2);
    if(!argv[0])
        argp_failure(state, 1, ENOMEM, 0);

    sprintf(argv[0], "%s %s", state->name, cmd);

    argp_parse(argp_cmd, argc, argv, ARGP_IN_ORDER, &argc, args);

    free(argv[0]);
    argv[0] = argv0;
    state->next += argc - 1;
}

static struct argp argp_test = {opt_test, parse_opt_test, 0, doc_test};
static struct argp argp_admin = {opt_admin, parse_opt_admin, 0, doc_admin};

error_t parse_opt (int key, char *arg, struct argp_state *state)
{
    struct nvm_init_arg *args = state->input;

    switch(key)
    {
        case ARGP_KEY_ARG:
            if (strcmp(arg, "start") == 0)
                args->cmdtype = CMDARG_START;
            else if (strcmp(arg, "debug") == 0)
                args->cmdtype = CMDARG_DEBUG;
            else if (strcmp(arg, "test") == 0){
                args->cmdtype = CMDARG_TEST;
                cmd_prepare(state, args, "test", &argp_test);
            } else if (strcmp(arg, "admin") == 0){
                args->cmdtype = CMDARG_ADMIN;
                cmd_prepare(state, args, "admin", &argp_admin);
            }
            break;
        default:
            return ARGP_ERR_UNKNOWN;
    }
    return 0;
}

static struct argp argp_global={NULL, parse_opt,"ox-ctrl [<cmd> [cmd-options]]",
                                                                   doc_global};

int cmdarg_init (int argc, char **argv)
{
    int ret;

    core.args_global = malloc (sizeof (struct nvm_init_arg));
    memset (core.args_global, 0, sizeof(struct nvm_init_arg));

    argp_parse(&argp_global, argc, argv, ARGP_IN_ORDER, NULL, core.args_global);

    switch (core.args_global->cmdtype)
    {
        case CMDARG_START:
            return OX_RUN_MODE;
        case CMDARG_DEBUG:
            core.debug |= 1 << 0;
            return OX_RUN_MODE;
        case CMDARG_TEST:
            ret = nvm_test_unit(core.args_global);
            return (!ret) ? OX_TEST_MODE : -1;
        case CMDARG_ADMIN:
            ret = nvm_admin_unit(core.args_global);
            return (!ret) ? OX_ADMIN_MODE : -1;
        default:
            printf("Invalid command, please use --help to see more info.\n");
    }

    return -1;
}