CC      = gcc
CFLAGS  = -Wall -Wextra -std=c11 -pthread
LDFLAGS = -largtable3 -pthread

# Relaxed flags for BNFC-generated code — flex/bison output triggers
# sign-conversion and unused-parameter warnings that are not our code.
CFLAGS_GEN = -g -std=c11 \
             -Wno-unused-parameter -Wno-unused-function \
             -Wno-sign-conversion  -Wno-implicit-function-declaration

# All command sources — same list as week6
SRCS = aishell_main.c \
       cmd_ls.c cmd_cat.c cmd_stat.c cmd_head.c cmd_tail.c \
       cmd_cp.c cmd_mv.c cmd_rm.c cmd_mkdir.c cmd_rmdir.c cmd_touch.c \
       cmd_rg.c cmd_ps.c cmd_kill.c cmd_wait.c cmd_jobs.c \
       cmd_cd.c cmd_pwd.c cmd_echo.c \
       cmd_env.c cmd_export.c cmd_unset.c cmd_type.c \
       cmd_edit_replace_line.c cmd_edit_insert_line.c \
       cmd_edit_delete_line.c cmd_edit_replace.c \
       edit_utils.c \
       cmd_wc.c cmd_sort.c cmd_date.c cmd_find.c cmd_edit_show.c \
       cmd_help_json.c registry.c

# BNFC-generated object files (built in bnfc/ after running bnfc-gen)
BNFC_DIR  = bnfc
BNFC_OBJS = $(BNFC_DIR)/Absyn.o  \
            $(BNFC_DIR)/Buffer.o \
            $(BNFC_DIR)/Lexer.o  \
            $(BNFC_DIR)/Parser.o \
            $(BNFC_DIR)/Printer.o

.PHONY: all clean bnfc-gen bnfc-test bnfc-clean

# ── default: build ./aishell with BNFC parser (week7) ────────────────────────
# On the week7 branch ./aishell IS the BNFC-powered shell.
# Run  make bnfc-gen  once (or after Grammar.cf changes), then  make.
all: aishell

aishell: $(SRCS) cmd_spec.h cmd_help_json.h $(BNFC_OBJS)
	$(CC) $(CFLAGS) -DUSE_BNFC -I$(BNFC_DIR) \
	      -o aishell $(SRCS) $(BNFC_OBJS) $(LDFLAGS)

# ── bnfc-gen: generate lexer/parser/AST sources from Grammar.cf ──────────────
# Run this once after cloning, or whenever Grammar.cf changes.
bnfc-gen:
	cd $(BNFC_DIR) && bnfc --c -m Grammar.cf

# ── compile BNFC-generated C files with relaxed warning flags ────────────────

# Grammar.l  → flex  → Lexer.c
# -Pgrammar_ sets the function prefix to match what BNFC generates
$(BNFC_DIR)/Lexer.c: $(BNFC_DIR)/Grammar.l
	flex -Pgrammar_ -o $@ $<

# Grammar.y  → bison → Parser.c + Bison.h
# -pgrammar_ sets the symbol prefix to match BNFC's generated parser calls
$(BNFC_DIR)/Parser.c $(BNFC_DIR)/Bison.h: $(BNFC_DIR)/Grammar.y
	bison -t -pgrammar_ $< -o $(BNFC_DIR)/Parser.c

$(BNFC_DIR)/Absyn.o:  $(BNFC_DIR)/Absyn.c  $(BNFC_DIR)/Absyn.h
	$(CC) $(CFLAGS_GEN) -c $< -o $@

$(BNFC_DIR)/Buffer.o: $(BNFC_DIR)/Buffer.c $(BNFC_DIR)/Buffer.h
	$(CC) $(CFLAGS_GEN) -c $< -o $@

$(BNFC_DIR)/Lexer.o:  $(BNFC_DIR)/Lexer.c  $(BNFC_DIR)/Bison.h
	$(CC) $(CFLAGS_GEN) -c $< -o $@

$(BNFC_DIR)/Parser.o: $(BNFC_DIR)/Parser.c $(BNFC_DIR)/Absyn.h $(BNFC_DIR)/Bison.h
	$(CC) $(CFLAGS_GEN) -c $< -o $@

$(BNFC_DIR)/Printer.o: $(BNFC_DIR)/Printer.c $(BNFC_DIR)/Printer.h $(BNFC_DIR)/Absyn.h
	$(CC) $(CFLAGS_GEN) -c $< -o $@

# ── bnfc-test: build the BNFC Test AST pretty-printer (grammar debugger) ─────
bnfc-test:
	$(MAKE) -C $(BNFC_DIR)

# ── clean ─────────────────────────────────────────────────────────────────────
clean:
	rm -f aishell *.o libcmds.a \
	      ls cat stat head tail cp mv rm mkdir rmdir touch \
	      rg ps kill wait jobs cd pwd echo

bnfc-clean:
	rm -f $(BNFC_OBJS)
	$(MAKE) -C $(BNFC_DIR) clean
