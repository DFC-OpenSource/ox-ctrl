/* OX: Open-Channel NVM Express SSD Controller
 *
 *  - OX Command line
 *
 * Copyright 2017 IT University of Copenhagen
 * Written by Frey Alfredsson <frea@itu.dk>
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <libox.h>
#include <ox-mq.h>
#include <ox-cmdline.h>
#include <nvme.h>

static char *seperator = " \t";

static int debug_on = 1;
static int debug_off = 0;
ox_cmd debug_cmd[] = {
        { "on",
          NULL,
          cmdline_set_debug,
          &debug_on,
          "Enables debugging",
          "Enables debugging\n"
          "\n"
          "    Will enable the display of live debugging information of NVMe commands."
        },
        { "off",
          NULL,
          cmdline_set_debug,
          &debug_off,
          "Disables debugging",
          "Disables debugging\n"
          "\n"
          "    Will disable the display of live debugging information of NVMe commands."
        },
        { NULL, NULL, NULL, NULL, NULL, NULL }
};

ox_cmd admin_cmd[] = {
        { "create-bbt",
          NULL,
          cmdline_admin,
          "create-bbt",
          "Create or update the bad block table",
          "Create or update the bad block table\n"
          "\n"
          "    An interactive command that creates or updates the bad block table,\n"
          "    which offers two modes:\n"
          "        1 - Full scan (Check all blocks by erasing, writing and reading)\n"
          "        2 - Emergency table (Creates an empty bad block table without\n"
          "            erasing blocks)"
        },
        { "erase-blk",
          NULL,
          cmdline_admin,
          "erase-blk",
          "Erase specific block",
          "Erase specific block\n"
          "\n"
          "    An interactive command that allows you to erase any specified block."
        },
        { NULL, NULL, NULL, NULL, NULL, NULL }
};

ox_cmd mq_cmd[] = {
        { "status",
          NULL,
          cmdline_show_mq_status,
          NULL,
          "Shows the status of all the internal mq queues",
          "Shows the status of all the internal mq queues\n"
          "\n"
          "    Displays run-time status of the internal queue groups of mq. This includes\n"
          "    their name and status of each queue.\n"
          "      Key  Description\n"
          "      Q:   Queue number\n"
          "      SF:  Submission Free queue  -  Available for new submission entries\n"
          "      SU:  Submission Used queue  -  Ready to be processed\n"
          "      SW:  Submission Wait queue  -  In process\n"
          "      CF:  Completion Free queue  -  Available for new completion entries\n"
          "      CU:  Completion Used queue  -  Processed, but waiting for completion"
        },
        { NULL, NULL, NULL, NULL, NULL, NULL }
};

ox_cmd show_cmd[] = {
        { "debug",
          NULL,
          cmdline_show_debug,
          NULL,
          "Shows if debugging mode is enabled",
          "Shows if debugging mode is enabled\n"
          "\n"
          "    Displays if reporting of live debugging information of NVMe commands\n"
          "    is enabled."
        },
        { "mq",
          mq_cmd,
          NULL,
          NULL,
          "Displays run-time information for multi-queue",
          "Usage: show mq [sub-command]\n"
          "    Displays run-time information and status information for multi-queue."
        },
        { "memory",
          NULL,
          cmdline_show_memory,
          NULL,
          "Displays memory usage information",
          "Usage: show memory"
        },
        { "io",
          NULL,
          cmdline_show_io,
          NULL,
          "Displays physical I/O information",
          "Usage: show io"
        },
        { "gc",
          NULL,
          cmdline_show_gc,
          NULL,
          "Displays garbage collection information",
          "Usage: show gc"
        },
        { "rec",
          NULL,
          cmdline_show_recovery,
          NULL,
          "Displays recovery and log information",
          "Usage: show rec"
        },
        { "cp",
          NULL,
          cmdline_show_checkpoint,
          NULL,
          "Displays checkpoint information",
          "Usage: show all"
        },
        { "all",
          NULL,
          cmdline_show_all,
          NULL,
          "Displays memory, I/O, GC, recovery, log, and checkpoint information",
          "Usage: show all"
        },
        { "reset",
          NULL,
          cmdline_show_reset,
          NULL,
          "Restart I/O and bytes count of 'show io' command",
          "Usage: show reset"
        },
        { NULL, NULL, NULL, NULL, NULL, NULL }
};

ox_cmd output_cmd[] = {
        { "start",
          NULL,
          cmdline_start_output,
          NULL,
          "Starts logging I/O information in memory",
          "Starts logging I/O information in memory\n"
          "\n"
          "    I/O information will be appended in memory until 'stop' is called."
        },
        { "stop",
          NULL,
          cmdline_stop_output,
          NULL,
          "Stops logging I/O information and flushes the log to a CSV file",
          "Stops logging I/O information and flushes the log to a CSV file\n"
          "\n"
          "    A CSV file per ox-mq instance will be flushed containing I/O"
                                                                " information."
        },
        { NULL, NULL, NULL, NULL, NULL, NULL }
};

ox_cmd main_cmd[] = {
        { "help",
          &main_cmd[1],  // Skip help in auto-complete
          NULL,
          NULL,
          "Display information about builtin commands.",
          "Usage: help [command]\n"
          "    Display information about builtin commands.\n"
          "\n"
          "    Displays brief summaries of builtin commands. If a command is\n"
          "    specified, it will give a listing of all its sub-commands."
        },
        { "admin",
          admin_cmd,
          NULL,
          NULL,
          "Used for testing",
          "Usage: admin [sub-command]\n"
          "    "
        },
        { "debug",
          debug_cmd,
          NULL,
          NULL,
          "Enables or disables debugging output",
          "Usage: debug [sub-command]\n"
          "    Enables or disables debugging output."
        },
        { "exit",
          NULL,
          cmdline_exit,
          NULL,
          "Exit the OX application",
          "Usage: exit\n"
          "    Exit the OX application\n"
          "\n"
          "    Exits the OX application and gracefully turns off all controller\n"
          "    functionality."


        },
        { "show",
          show_cmd,
          NULL,
          NULL,
          "Displays run-time information",
          "Usage: show [sub-command]\n"
          "    Displays run-time information and status information."
        },
        { "output",
          output_cmd,
          NULL,
          NULL,
          "Logs I/O latency in memory and flushes a CSV file with data",
          "Usage: output [start/stop]\n"
          "   Logs I/O latency in memory and flushes a CSV file with data."
        },
        { NULL, NULL, NULL, NULL, NULL, NULL }
};

void cmdline_longhelp (ox_cmd *cmd)
{
        ox_cmd *cmd_list = NULL;
        ox_cmd *subcmd;
        int cmd_index = 0;

        printf("%s: %s\n", cmd->command, cmd->help);
        if (!cmd->next)
                return;
        cmd_list = cmd->next;
        printf("\n");
        printf("    List of sub-commands:\n");
        while ((subcmd = &cmd_list[cmd_index++])->command) {
                printf("        %s:\t%s\n", subcmd->command, subcmd->short_help);
        }
}

int cmdline_set_debug (char *line, ox_cmd *cmd)
{
        core.debug = *((int *)(cmd->value));

        printf("OX: debugging %s\n", core.debug ? "enabled" : "disabled");
        return 0;
}

int cmdline_show_debug (char *line, ox_cmd *cmd)
{
        printf("OX: debugging is %s\n", core.debug ? "on" : "off");
        return 0;
}

int cmdline_show_memory (char *line, ox_cmd *cmd)
{
        ox_mem_print_memory ();
        return 0;
}

int cmdline_show_io (char *line, ox_cmd *cmd)
{
        ox_stats_print_io ();
        return 0;
}

int cmdline_show_gc (char *line, ox_cmd *cmd)
{
        ox_stats_print_gc ();
        return 0;
}

int cmdline_show_recovery (char *line, ox_cmd *cmd)
{
        ox_stats_print_recovery ();
        return 0;
}

int cmdline_show_checkpoint (char *line, ox_cmd *cmd)
{
        ox_stats_print_checkpoint ();
        return 0;
}

int cmdline_show_all (char *line, ox_cmd *cmd)
{
        ox_mem_print_memory ();
        ox_stats_print_io ();
        ox_stats_print_gc ();
        ox_stats_print_recovery ();
        ox_stats_print_checkpoint ();
        return 0;
}

int cmdline_show_reset (char *line, ox_cmd *cmd)
{
        ox_stats_reset_io ();
        ox_stats_print_io ();
        return 0;
}

int cmdline_start_output (char *line, ox_cmd *cmd)
{
        ox_mq_output_start ();
        return 0;
}

int cmdline_stop_output (char *line, ox_cmd *cmd)
{
        ox_mq_output_stop ();
        return 0;
}

int cmdline_show_mq_status (char *line, ox_cmd *cmd)
{
        ox_mq_show_all();
        return 0;
}

int cmdline_admin (char *line, ox_cmd *cmd)
{
        struct nvm_init_arg args = {
                CMDARG_ADMIN,
                1,
                CMDARG_FLAG_T,
                { '\0' },
        };
        strcpy(args.admin_task, (char*) cmd->value);
        ox_admin_init (&args);

        return 0;
}

int cmdline_exit (char *line, ox_cmd *cmd)
{
    core.nvme_ctrl->running = 1;
    core.nvme_ctrl->stop = 1;
    if (core.run_flag & RUN_READY)
        core.run_flag ^= RUN_READY;

    return 0;
}

void cmdline_runcommand (char *line, ox_cmd *cmd)
{
        if (cmd->func) {
                cmd->func(line, cmd);
        }
        else {
                printf("%s: Incomplete command, missing sub-command\n",
                       cmd->command);
                printf("    %s\n", cmd->short_help ? cmd->short_help : "N/A");
        }
}

int is_help_cmd (char *line)
{
        char *token_line = strdup(line);
        char *tok_state;
        char *word = strtok_r(token_line, seperator, &tok_state);
        if (word && !strcmp("help", word)) {
                return 1;
        }
        if (token_line) free (token_line);
        return 0;
}

void remove_trailing_whitespace (char *line)
{
        char *end = line + strlen(line) - 1;
        while (end > line && isspace((unsigned char) *end))
                end--;
        *(end + 1) = 0; // New end of string
}

static void sigint_handler (int handle)
{
        // Ignore for now, cancel running commands in the future
}

void ox_cmdline_init (void)
{
        struct sigaction new_sa;
        struct sigaction old_sa;
        sigfillset(&new_sa.sa_mask);
        new_sa.sa_handler = SIG_IGN;
        new_sa.sa_flags = 0;

        if (sigaction(SIGINT, &new_sa, &old_sa) == 0
            && old_sa.sa_handler != SIG_IGN) {
                new_sa.sa_handler = sigint_handler;
                sigaction(SIGINT, &new_sa, 0);
        }

        rl_attempted_completion_function = command_completion;

        while (core.run_flag & RUN_READY) {
                char *buffer = readline("ox> ");
                if (!buffer || is_whitespace(buffer)) {
                        if (buffer)
                            free (buffer);
                        continue;
                }
                remove_trailing_whitespace(buffer);
                ox_cmd *cmd;
                ox_cmd *cmd_list;
                find_command(buffer, &cmd, &cmd_list);
                if (cmd == NULL) {
                        printf("%s: command not found.\n", buffer);
                }
                else if (is_help_cmd(buffer)) {
                        if (cmd->help) {
                                cmdline_longhelp(cmd);
                        }
                        else {
                                printf("%s: Missing help string\n", cmd->command);
                        }
                }
                else {
                        cmdline_runcommand(buffer, cmd);
                }
                add_history (buffer);
                free (buffer);
        }
}

int is_full_match (const char *word, ox_cmd *cmd)
{
        int len = strlen(cmd->command);
        return !strncmp(word, cmd->command, len);
}

void find_command (char *line, ox_cmd **p_cmd, ox_cmd *p_cmd_list[])
{
        ox_cmd *cmd_list = main_cmd;
        ox_cmd *comp_cmd = NULL;
        ox_cmd *cmd = NULL;

        char *token_line = strdup(line);
        char *tok_state;
        char *word = strtok_r(token_line, seperator, &tok_state);
        while (word != NULL) {
                int cmd_index = 0;
                int len = strlen(word);

                int found = 0;
                int full_match = 0;
                if (!cmd_list) {
                        break;
                }
                while (!found && (comp_cmd = &cmd_list[cmd_index++])) {
                        if (comp_cmd->command == NULL) {
                                cmd_list = NULL;
                                break;
                        }
                        if (strncmp(comp_cmd->command, word, len) == 0) {
                                full_match = is_full_match(word, comp_cmd);
                                if (full_match) {
                                        cmd = comp_cmd;
                                }
                                found = 1;
                                break;
                        }
                }
                if (full_match) {
                        if (full_match && comp_cmd->next) {
                                cmd_list = comp_cmd->next;
                        }
                        else {
                                cmd_list = NULL;
                        }
                }
                word = strtok_r(NULL, seperator, &tok_state);
                if (word) {
                        cmd = NULL;
                }
        }
        *p_cmd = cmd;
        *p_cmd_list = cmd_list;
}

char **command_completion (const char *text, int start, int end)
{
        rl_attempted_completion_over = 1;
        return rl_completion_matches(text, command_generator);
}

int is_whitespace (const char *text)
{
        const char *ch = text;
        while (*ch) {
                if (!isspace(*ch)) {
                        return 0;
                }
                ch++;
        }
        return 1;
}

char *command_generator (const char *text, int state)
{
        static int cmd_index;
        static int len;
        static ox_cmd *cmd_list;
        static ox_cmd *cmd;
        char *cmd_name;
        const char *comp_cmd = !is_whitespace(text) ? text : "";
        if (!state) {
                cmd_index = 0;
                len = strlen(text);
                find_command(rl_line_buffer, &cmd, &cmd_list);
        }

        if (cmd && is_full_match(text, cmd)) {
                char *command = strdup(cmd->command);
                cmd = NULL;
                cmd_list = NULL;
                return command;
        }
        if (!cmd_list) {
                return NULL;
        }
        while ((cmd_name = cmd_list[cmd_index++].command)) {
                if (strncmp(cmd_name, comp_cmd, len) == 0) {
                        return strdup(cmd_name);
                }
        }
        return NULL;
}
