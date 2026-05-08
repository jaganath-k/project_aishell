CC      = gcc
CFLAGS  = -Wall -Wextra -std=c11
LDFLAGS = -largtable3

# All command sources compiled together into one binary — no separate .o files
SRCS = aishell_main.c \
       cmd_ls.c cmd_cat.c cmd_stat.c cmd_head.c cmd_tail.c \
       cmd_cp.c cmd_mv.c cmd_rm.c cmd_mkdir.c cmd_rmdir.c cmd_touch.c \
       cmd_rg.c cmd_ps.c cmd_kill.c cmd_wait.c cmd_jobs.c \
       cmd_cd.c cmd_pwd.c cmd_echo.c \
       cmd_env.c cmd_export.c cmd_unset.c cmd_type.c \
       cmd_help_json.c registry.c

.PHONY: all clean

all: aishell

aishell: $(SRCS) cmd_spec.h cmd_help_json.h
	$(CC) $(CFLAGS) -o aishell $(SRCS) $(LDFLAGS)

clean:
	rm -f aishell *.o libcmds.a \
	      ls cat stat head tail cp mv rm mkdir rmdir touch \
	      rg ps kill wait jobs cd pwd echo
