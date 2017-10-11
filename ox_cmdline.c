/* OX: Open-Channel NVM Express SSD Controller
 *
 *  - OX Command line
 *
 * Copyright (C) 2017, IT University of Copenhagen. All rights reserved.
 * Written by Frey Alfredsson <frea@itu.dk>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  - Redistributions of source code must retain the above copyright notice,
 *  this list of conditions and the following disclaimer.
 *  - Redistributions in binary form must reproduce the above copyright notice,
 *  this list of conditions and the following disclaimer in the documentation
 *  and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <readline/readline.h>
#include <readline/history.h>
#include "include/ssd.h"
#include "include/cmdline.h"

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
        { "debug",
          debug_cmd,
          NULL,
          NULL,
          "Enables or disables debugging output",
          "Usage: debug [sub-command]\n"
          "    Enables or disables debugging output."
        },
        { "show",
          show_cmd,
          NULL,
          NULL,
          "Displays run-time information",
          "Usage: show [sub-command]\n"
          "    Displays run-time information and status information."
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

int cmdline_show_mq_status (char *line, ox_cmd *cmd)
{
        ox_mq_show_all();
        return 0;
}

int cmdline_exit (char *line, ox_cmd *cmd)
{
        core.nvm_nvme_ctrl->running = 1;
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

void cmdline_start (void)
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

        while (!core.nvm_nvme_ctrl->running) {
                char *buffer = readline("ox> ");
                if (!buffer || is_whitespace(buffer)) {
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
                add_history(buffer);
                free(buffer);
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
        free(token_line);
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
