/* OX: Open-Channel NVM Express SSD Controller
 *
 *  - DFC NAND Media Manager
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

extern struct core_struct core;

typedef struct ox_cmd ox_cmd;
typedef int (*ox_cmdline_func_t)(char *line, ox_cmd *cmd);

typedef struct ox_cmd {
        char *command;
        struct ox_cmd *next;
        ox_cmdline_func_t func;
        void *value;
        char *short_help;
        char *help;
} ox_cmd;

int cmdline_set_debug (char *line, ox_cmd *cmd);
int cmdline_show_debug (char *line, ox_cmd *cmd);
int cmdline_exit (char *line, ox_cmd *cmd);

void cmdline_start (void);

char **command_completion (const char *text, int start, int end);
char *command_generator (const char *text, int state);
void find_command (char *line, ox_cmd **p_cmd, ox_cmd *p_cmd_list[]);
void remove_trailing_whitespace (char *line);
int is_whitespace (const char *text);
int is_help_cmd (char *line);
