/* OX: Open-Channel NVM Express SSD Controller
 *
 *  - Command Line
 *
 * Copyright 2017 IT University of Copenhagen
 * 
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

int cmdline_start_output (char *line, ox_cmd *cmd);
int cmdline_stop_output (char *line, ox_cmd *cmd);
int cmdline_set_debug (char *line, ox_cmd *cmd);
int cmdline_show_debug (char *line, ox_cmd *cmd);
int cmdline_show_mq_status (char *line, ox_cmd *cmd);
int cmdline_show_memory (char *line, ox_cmd *cmd);
int cmdline_show_io (char *line, ox_cmd *cmd);
int cmdline_show_gc (char *line, ox_cmd *cmd);
int cmdline_show_recovery (char *line, ox_cmd *cmd);
int cmdline_show_checkpoint (char *line, ox_cmd *cmd);
int cmdline_show_all (char *line, ox_cmd *cmd);
int cmdline_show_reset (char *line, ox_cmd *cmd);
int cmdline_admin (char *line, ox_cmd *cmd);
int cmdline_exit (char *line, ox_cmd *cmd);

char **command_completion (const char *text, int start, int end);
char *command_generator (const char *text, int state);
void find_command (char *line, ox_cmd **p_cmd, ox_cmd *p_cmd_list[]);
void remove_trailing_whitespace (char *line);
int is_whitespace (const char *text);
int is_help_cmd (char *line);