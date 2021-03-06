/*
 * VIM - Vi IMproved	by Bram Moolenaar
 *
 * Do ":help uganda"  in Vim to read copying and usage conditions.
 * Do ":help credits" in Vim to see a list of people who contributed.
 * See README.txt for an overview of the Vim source code.
 */

#define EXTERN
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#include <msgpack.h>

#include "nvim/ascii.h"
#include "nvim/vim.h"
#include "nvim/main.h"
#include "nvim/buffer.h"
#include "nvim/charset.h"
#include "nvim/diff.h"
#include "nvim/eval.h"
#include "nvim/ex_cmds.h"
#include "nvim/ex_cmds2.h"
#include "nvim/ex_docmd.h"
#include "nvim/fileio.h"
#include "nvim/fold.h"
#include "nvim/getchar.h"
#include "nvim/hashtab.h"
#include "nvim/iconv.h"
#include "nvim/if_cscope.h"
#ifdef HAVE_LOCALE_H
# include <locale.h>
#endif
#include "nvim/mark.h"
#include "nvim/mbyte.h"
#include "nvim/memline.h"
#include "nvim/message.h"
#include "nvim/misc1.h"
#include "nvim/misc2.h"
#include "nvim/garray.h"
#include "nvim/log.h"
#include "nvim/memory.h"
#include "nvim/move.h"
#include "nvim/mouse.h"
#include "nvim/normal.h"
#include "nvim/ops.h"
#include "nvim/option.h"
#include "nvim/os_unix.h"
#include "nvim/path.h"
#include "nvim/profile.h"
#include "nvim/quickfix.h"
#include "nvim/screen.h"
#include "nvim/strings.h"
#include "nvim/syntax.h"
#include "nvim/ui.h"
#include "nvim/version.h"
#include "nvim/window.h"
#include "nvim/os/input.h"
#include "nvim/os/os.h"
#include "nvim/os/time.h"
#include "nvim/os/event.h"
#include "nvim/os/signal.h"
#include "nvim/msgpack_rpc/helpers.h"
#include "nvim/api/private/defs.h"
#include "nvim/api/private/helpers.h"
#include "nvim/api/private/handle.h"

/* Maximum number of commands from + or -c arguments. */
#define MAX_ARG_CMDS 10

/* values for "window_layout" */
#define WIN_HOR     1       /* "-o" horizontally split windows */
#define WIN_VER     2       /* "-O" vertically split windows */
#define WIN_TABS    3       /* "-p" windows on tab pages */

/* Struct for various parameters passed between main() and other functions. */
typedef struct {
  int argc;
  char        **argv;

  char_u      *use_vimrc;               /* vimrc from -u argument */

  int n_commands;                            /* no. of commands from + or -c */
  char_u      *commands[MAX_ARG_CMDS];       /* commands from + or -c arg. */
  char_u cmds_tofree[MAX_ARG_CMDS];          /* commands that need free() */
  int n_pre_commands;                        /* no. of commands from --cmd */
  char_u      *pre_commands[MAX_ARG_CMDS];   /* commands from --cmd argument */

  int edit_type;                        /* type of editing to do */
  char_u      *tagname;                 /* tag from -t argument */
  char_u      *use_ef;                  /* 'errorfile' from -q argument */

  int want_full_screen;
  bool input_isatty;                    // stdin is a terminal
  bool output_isatty;                   // stdout is a terminal
  bool err_isatty;                      // stderr is a terminal
  bool headless;                        // Dont try to start an user interface
                                        // or read/write to stdio(unless
                                        // embedding)
  char_u      *term;                    /* specified terminal name */
  int no_swap_file;                     /* "-n" argument used */
  int use_debug_break_level;
  int window_count;                     /* number of windows to use */
  int window_layout;                    /* 0, WIN_HOR, WIN_VER or WIN_TABS */

#if !defined(UNIX)
  int literal;                          /* don't expand file names */
#endif
  int diff_mode;                        /* start with 'diff' set */
} mparm_T;

/* Values for edit_type. */
#define EDIT_NONE   0       /* no edit type yet */
#define EDIT_FILE   1       /* file name argument[s] given, use argument list */
#define EDIT_STDIN  2       /* read file from stdin */
#define EDIT_TAG    3       /* tag name argument given, use tagname */
#define EDIT_QF     4       /* start in quickfix mode */

#ifdef INCLUDE_GENERATED_DECLARATIONS
# include "main.c.generated.h"
#endif

// Error messages
static const char *main_errors[] = {
  N_("Unknown option argument"),
#define ME_UNKNOWN_OPTION       0
  N_("Too many edit arguments"),
#define ME_TOO_MANY_ARGS        1
  N_("Argument missing after"),
#define ME_ARG_MISSING          2
  N_("Garbage after option argument"),
#define ME_GARBAGE              3
  N_("Too many \"+command\", \"-c command\" or \"--cmd command\" arguments")
#define ME_EXTRA_CMD            4
};


/// Performs early initialization.
///
/// Needed for unit tests. Must be called after `time_init()`.
void early_init(void)
{
  handle_init();

  (void)mb_init();      // init mb_bytelen_tab[] to ones
  eval_init();          // init global variables

  // Init the table of Normal mode commands.
  init_normal_cmds();

#if defined(HAVE_LOCALE_H) || defined(X_LOCALE)
  // Setup to use the current locale (for ctype() and many other things).
  // NOTE: Translated messages with encodings other than latin1 will not
  // work until set_init_1() has been called!
  init_locale();
#endif

  // Allocate the first window and buffer.
  // Can't do anything without it, exit when it fails.
  if (!win_alloc_first()) {
    mch_exit(0);
  }

  init_yank();                  // init yank buffers

  alist_init(&global_alist);    // Init the argument list to empty.
  global_alist.id = 0;

  // Set the default values for the options.
  // NOTE: Non-latin1 translated messages are working only after this,
  // because this is where "has_mbyte" will be set, which is used by
  // msg_outtrans_len_attr().
  // First find out the home directory, needed to expand "~" in options.
  init_homedir();               // find real value of $HOME
  set_init_1();
  TIME_MSG("inits 1");

  set_lang_var();               // set v:lang and v:ctype
}

#ifdef MAKE_LIB
int nvim_main(int argc, char **argv)
#else
int main(int argc, char **argv)
#endif
{
  char_u      *fname = NULL;            /* file name from command line */
  mparm_T params;                       /* various parameters passed between
                                         * main() and other functions. */
  time_init();

  /* Many variables are in "params" so that we can pass them to invoked
   * functions without a lot of arguments.  "argc" and "argv" are also
   * copied, so that they can be changed. */
  init_params(&params, argc, argv);

  init_startuptime(&params);

  early_init();

  // Check if we have an interactive window.
  check_and_set_isatty(&params);

  // Get the name with which Nvim was invoked, with and without path.
  set_vim_var_string(VV_PROGPATH, (char_u *)argv[0], -1);
  set_vim_var_string(VV_PROGNAME, path_tail((char_u *)argv[0]), -1);

  /*
   * Process the command line arguments.  File names are put in the global
   * argument list "global_alist".
   */
  command_line_scan(&params);

  if (GARGCOUNT > 0)
    fname = get_fname(&params);

  TIME_MSG("expanding arguments");

  if (params.diff_mode && params.window_count == -1)
    params.window_count = 0;            /* open up to 3 windows */

  /* Don't redraw until much later. */
  ++RedrawingDisabled;

  /*
   * When listing swap file names, don't do cursor positioning et. al.
   */
  if (recoverymode && fname == NULL)
    params.want_full_screen = FALSE;

  setbuf(stdout, NULL);

  /* This message comes before term inits, but after setting "silent_mode"
   * when the input is not a tty. */
  if (GARGCOUNT > 1 && !silent_mode)
    printf(_("%d files to edit\n"), GARGCOUNT);

  event_init();
  full_screen = true;
  t_colors = 256;
  check_tty(&params);

  /*
   * Set the default values for the options that use Rows and Columns.
   */
  win_init_size();
  /* Set the 'diff' option now, so that it can be checked for in a .vimrc
   * file.  There is no buffer yet though. */
  if (params.diff_mode)
    diff_win_options(firstwin, FALSE);

  assert(p_ch >= 0 && Rows >= p_ch && Rows - p_ch <= INT_MAX);
  cmdline_row = (int)(Rows - p_ch);
  msg_row = cmdline_row;
  screenalloc(false);           /* allocate screen buffers */
  set_init_2();
  TIME_MSG("inits 2");

  msg_scroll = TRUE;
  no_wait_return = TRUE;

  init_highlight(TRUE, FALSE);   /* set the default highlight groups */
  TIME_MSG("init highlight");

  /* Set the break level after the terminal is initialized. */
  debug_break_level = params.use_debug_break_level;

  bool reading_input = !params.headless && (params.input_isatty
      || params.output_isatty || params.err_isatty);

  if (reading_input) {
    // Its possible that one of the startup commands(arguments, sourced scripts
    // or plugins) will prompt the user, so start reading from a tty stream
    // now. 
    int fd = fileno(stdin);
    if (!params.input_isatty || params.edit_type == EDIT_STDIN) {
      // use stderr or stdout since stdin is not a tty and/or could be used to
      // read the file we'll edit when the "-" argument is given(eg: cat file |
      // nvim -)
      fd = params.err_isatty ? fileno(stderr) : fileno(stdout);
    }
    input_start_stdin(fd);
  }

  /* Execute --cmd arguments. */
  exe_pre_commands(&params);

  /* Source startup scripts. */
  source_startup_scripts(&params);

  /*
   * Read all the plugin files.
   * Only when compiled with +eval, since most plugins need it.
   */
  load_plugins();

  /* Decide about window layout for diff mode after reading vimrc. */
  set_window_layout(&params);

  /*
   * Recovery mode without a file name: List swap files.
   * This uses the 'dir' option, therefore it must be after the
   * initializations.
   */
  if (recoverymode && fname == NULL) {
    recover_names(NULL, TRUE, 0, NULL);
    mch_exit(0);
  }

  /*
   * Set a few option defaults after reading .vimrc files:
   * 'title' and 'icon', Unix: 'shellpipe' and 'shellredir'.
   */
  set_init_3();
  TIME_MSG("inits 3");

  /*
   * "-n" argument: Disable swap file by setting 'updatecount' to 0.
   * Note that this overrides anything from a vimrc file.
   */
  if (params.no_swap_file)
    p_uc = 0;

  if (curwin->w_p_rl && p_altkeymap) {
    p_hkmap = FALSE;              /* Reset the Hebrew keymap mode */
    curwin->w_p_arab = FALSE;       /* Reset the Arabic keymap mode */
    p_fkmap = TRUE;               /* Set the Farsi keymap mode */
  }

  /*
   * Read in registers, history etc, but not marks, from the viminfo file.
   * This is where v:oldfiles gets filled.
   */
  if (*p_viminfo != NUL) {
    read_viminfo(NULL, VIF_WANT_INFO | VIF_GET_OLDFILES);
    TIME_MSG("reading viminfo");
  }
  /* It's better to make v:oldfiles an empty list than NULL. */
  if (get_vim_var_list(VV_OLDFILES) == NULL)
    set_vim_var_list(VV_OLDFILES, list_alloc());

  /*
   * "-q errorfile": Load the error file now.
   * If the error file can't be read, exit before doing anything else.
   */
  handle_quickfix(&params);

  /*
   * Start putting things on the screen.
   * Scroll screen down before drawing over it
   * Clear screen now, so file message will not be cleared.
   */
  starting = NO_BUFFERS;
  no_wait_return = FALSE;
  if (!exmode_active)
    msg_scroll = FALSE;

  /*
   * If "-" argument given: Read file from stdin.
   * Do this before starting Raw mode, because it may change things that the
   * writing end of the pipe doesn't like, e.g., in case stdin and stderr
   * are the same terminal: "cat | vim -".
   * Using autocommands here may cause trouble...
   */
  if (params.edit_type == EDIT_STDIN && !recoverymode)
    read_stdin();


  if (reading_input && (need_wait_return || msg_didany)) {
    // Since at this point there's no UI instance running yet, error messages
    // would have been printed to stdout. Before starting (which can result in
    // a alternate screen buffer being shown) we need confirmation that the
    // user has seen the messages and that is done with a call to wait_return.
    TIME_MSG("waiting for return");
    wait_return(TRUE);
  }

  if (!params.headless) {
    // Stop reading from stdin, the UI layer will take over now
    input_stop_stdin();
    ui_builtin_start();
  }

  setmouse();  // may start using the mouse
  ui_reset_scroll_region();  // In case Rows changed

  // Don't clear the screen when starting in Ex mode, unless using the GUI.
  if (exmode_active)
    must_redraw = CLEAR;
  else {
    screenclear();                        /* clear screen */
    TIME_MSG("clearing screen");
  }

  no_wait_return = TRUE;

  /*
   * Create the requested number of windows and edit buffers in them.
   * Also does recovery if "recoverymode" set.
   */
  create_windows(&params);
  TIME_MSG("opening buffers");

  /* clear v:swapcommand */
  set_vim_var_string(VV_SWAPCOMMAND, NULL, -1);

  /* Ex starts at last line of the file */
  if (exmode_active)
    curwin->w_cursor.lnum = curbuf->b_ml.ml_line_count;

  apply_autocmds(EVENT_BUFENTER, NULL, NULL, FALSE, curbuf);
  TIME_MSG("BufEnter autocommands");
  setpcmark();

  /*
   * When started with "-q errorfile" jump to first error now.
   */
  if (params.edit_type == EDIT_QF) {
    qf_jump(NULL, 0, 0, FALSE);
    TIME_MSG("jump to first error");
  }

  /*
   * If opened more than one window, start editing files in the other
   * windows.
   */
  edit_buffers(&params);

  if (params.diff_mode) {
    /* set options in each window for "nvim -d". */
    FOR_ALL_WINDOWS_IN_TAB(wp, curtab) {
      diff_win_options(wp, TRUE);
    }
  }

  /*
   * Shorten any of the filenames, but only when absolute.
   */
  shorten_fnames(FALSE);

  /*
   * Need to jump to the tag before executing the '-c command'.
   * Makes "vim -c '/return' -t main" work.
   */
  handle_tag(params.tagname);

  /* Execute any "+", "-c" and "-S" arguments. */
  if (params.n_commands > 0)
    exe_commands(&params);

  RedrawingDisabled = 0;
  redraw_all_later(NOT_VALID);
  no_wait_return = FALSE;
  starting = 0;

  /* start in insert mode */
  if (p_im)
    need_start_insertmode = TRUE;

  apply_autocmds(EVENT_VIMENTER, NULL, NULL, FALSE, curbuf);
  TIME_MSG("VimEnter autocommands");

  /* When a startup script or session file setup for diff'ing and
   * scrollbind, sync the scrollbind now. */
  if (curwin->w_p_diff && curwin->w_p_scb) {
    update_topline();
    check_scrollbind((linenr_T)0, 0L);
    TIME_MSG("diff scrollbinding");
  }

  /* If ":startinsert" command used, stuff a dummy command to be able to
   * call normal_cmd(), which will then start Insert mode. */
  if (restart_edit != 0)
    stuffcharReadbuff(K_NOP);

  TIME_MSG("before starting main loop");

  /*
   * Call the main command loop.  This never returns.
   */
  main_loop(FALSE, FALSE);

  return 0;
}

/*
 * Main loop: Execute Normal mode commands until exiting Vim.
 * Also used to handle commands in the command-line window, until the window
 * is closed.
 * Also used to handle ":visual" command after ":global": execute Normal mode
 * commands, return when entering Ex mode.  "noexmode" is TRUE then.
 */
void
main_loop (
    int cmdwin,                 /* TRUE when working in the command-line window */
    int noexmode               /* TRUE when return on entering Ex mode */
)
{
  oparg_T oa;                                   /* operator arguments */
  int previous_got_int = FALSE;                 /* "got_int" was TRUE */
  linenr_T conceal_old_cursor_line = 0;
  linenr_T conceal_new_cursor_line = 0;
  int conceal_update_lines = FALSE;

  ILOG("Starting Neovim main loop.");

  clear_oparg(&oa);
  while (!cmdwin
      || cmdwin_result == 0
      ) {
    if (stuff_empty()) {
      did_check_timestamps = FALSE;
      if (need_check_timestamps)
        check_timestamps(FALSE);
      if (need_wait_return)             /* if wait_return still needed ... */
        wait_return(FALSE);             /* ... call it now */
      if (need_start_insertmode && goto_im()
          && !VIsual_active
         ) {
        need_start_insertmode = FALSE;
        stuffReadbuff((char_u *)"i");           /* start insert mode next */
        /* skip the fileinfo message now, because it would be shown
         * after insert mode finishes! */
        need_fileinfo = FALSE;
      }
    }

    /* Reset "got_int" now that we got back to the main loop.  Except when
     * inside a ":g/pat/cmd" command, then the "got_int" needs to abort
     * the ":g" command.
     * For ":g/pat/vi" we reset "got_int" when used once.  When used
     * a second time we go back to Ex mode and abort the ":g" command. */
    if (got_int) {
      if (noexmode && global_busy && !exmode_active && previous_got_int) {
        /* Typed two CTRL-C in a row: go back to ex mode as if "Q" was
         * used and keep "got_int" set, so that it aborts ":g". */
        exmode_active = EXMODE_NORMAL;
        State = NORMAL;
      } else if (!global_busy || !exmode_active) {
        if (!quit_more)
          (void)vgetc();                        /* flush all buffers */
        got_int = FALSE;
      }
      previous_got_int = TRUE;
    } else
      previous_got_int = FALSE;

    if (!exmode_active)
      msg_scroll = FALSE;
    quit_more = FALSE;

    /*
     * If skip redraw is set (for ":" in wait_return()), don't redraw now.
     * If there is nothing in the stuff_buffer or do_redraw is TRUE,
     * update cursor and redraw.
     */
    if (skip_redraw || exmode_active)
      skip_redraw = FALSE;
    else if (do_redraw || stuff_empty()) {
      /* Trigger CursorMoved if the cursor moved. */
      if (!finish_op && (
            has_cursormoved()
            ||
            curwin->w_p_cole > 0
            )
          && !equalpos(last_cursormoved, curwin->w_cursor)) {
        if (has_cursormoved())
          apply_autocmds(EVENT_CURSORMOVED, NULL, NULL,
              FALSE, curbuf);
        if (curwin->w_p_cole > 0) {
          conceal_old_cursor_line = last_cursormoved.lnum;
          conceal_new_cursor_line = curwin->w_cursor.lnum;
          conceal_update_lines = TRUE;
        }
        last_cursormoved = curwin->w_cursor;
      }

      /* Trigger TextChanged if b_changedtick differs. */
      if (!finish_op && has_textchanged()
          && last_changedtick != curbuf->b_changedtick) {
        if (last_changedtick_buf == curbuf)
          apply_autocmds(EVENT_TEXTCHANGED, NULL, NULL,
              FALSE, curbuf);
        last_changedtick_buf = curbuf;
        last_changedtick = curbuf->b_changedtick;
      }

      /* Scroll-binding for diff mode may have been postponed until
       * here.  Avoids doing it for every change. */
      if (diff_need_scrollbind) {
        check_scrollbind((linenr_T)0, 0L);
        diff_need_scrollbind = FALSE;
      }
      /* Include a closed fold completely in the Visual area. */
      foldAdjustVisual();
      /*
       * When 'foldclose' is set, apply 'foldlevel' to folds that don't
       * contain the cursor.
       * When 'foldopen' is "all", open the fold(s) under the cursor.
       * This may mark the window for redrawing.
       */
      if (hasAnyFolding(curwin) && !char_avail()) {
        foldCheckClose();
        if (fdo_flags & FDO_ALL)
          foldOpenCursor();
      }

      /*
       * Before redrawing, make sure w_topline is correct, and w_leftcol
       * if lines don't wrap, and w_skipcol if lines wrap.
       */
      update_topline();
      validate_cursor();

      if (VIsual_active)
        update_curbuf(INVERTED);        /* update inverted part */
      else if (must_redraw)
        update_screen(0);
      else if (redraw_cmdline || clear_cmdline)
        showmode();
      redraw_statuslines();
      if (need_maketitle)
        maketitle();
      /* display message after redraw */
      if (keep_msg != NULL) {
        char_u *p;

        // msg_attr_keep() will set keep_msg to NULL, must free the string
        // here. Don't reset keep_msg, msg_attr_keep() uses it to check for
        // duplicates.
        p = keep_msg;
        msg_attr(p, keep_msg_attr);
        free(p);
      }
      if (need_fileinfo) {              /* show file info after redraw */
        fileinfo(FALSE, TRUE, FALSE);
        need_fileinfo = FALSE;
      }

      emsg_on_display = FALSE;          /* can delete error message now */
      did_emsg = FALSE;
      msg_didany = FALSE;               /* reset lines_left in msg_start() */
      may_clear_sb_text();              /* clear scroll-back text on next msg */
      showruler(FALSE);

      if (conceal_update_lines
          && (conceal_old_cursor_line != conceal_new_cursor_line
            || conceal_cursor_line(curwin)
            || need_cursor_line_redraw)) {
        if (conceal_old_cursor_line != conceal_new_cursor_line
            && conceal_old_cursor_line
            <= curbuf->b_ml.ml_line_count)
          update_single_line(curwin, conceal_old_cursor_line);
        update_single_line(curwin, conceal_new_cursor_line);
        curwin->w_valid &= ~VALID_CROW;
      }
      setcursor();

      do_redraw = FALSE;

      /* Now that we have drawn the first screen all the startup stuff
       * has been done, close any file for startup messages. */
      if (time_fd != NULL) {
        TIME_MSG("first screen update");
        TIME_MSG("--- NVIM STARTED ---");
        fclose(time_fd);
        time_fd = NULL;
      }
    }

    /*
     * Update w_curswant if w_set_curswant has been set.
     * Postponed until here to avoid computing w_virtcol too often.
     */
    update_curswant();

    /*
     * May perform garbage collection when waiting for a character, but
     * only at the very toplevel.  Otherwise we may be using a List or
     * Dict internally somewhere.
     * "may_garbage_collect" is reset in vgetc() which is invoked through
     * do_exmode() and normal_cmd().
     */
    may_garbage_collect = (!cmdwin && !noexmode);
    /*
     * If we're invoked as ex, do a round of ex commands.
     * Otherwise, get and execute a normal mode command.
     */
    if (exmode_active) {
      if (noexmode)         /* End of ":global/path/visual" commands */
        return;
      do_exmode(exmode_active == EXMODE_VIM);
    } else
      normal_cmd(&oa, TRUE);
  }
}


/* Exit properly */
void getout(int exitval)
{
  tabpage_T   *tp, *next_tp;

  exiting = TRUE;

  /* When running in Ex mode an error causes us to exit with a non-zero exit
   * code.  POSIX requires this, although it's not 100% clear from the
   * standard. */
  if (exmode_active)
    exitval += ex_exitval;

  /* Position the cursor on the last screen line, below all the text */
  ui_cursor_goto((int)Rows - 1, 0);

  /* Optionally print hashtable efficiency. */
  hash_debug_results();

  if (get_vim_var_nr(VV_DYING) <= 1) {
    /* Trigger BufWinLeave for all windows, but only once per buffer. */
    for (tp = first_tabpage; tp != NULL; tp = next_tp) {
      next_tp = tp->tp_next;
      FOR_ALL_WINDOWS_IN_TAB(wp, tp) {
        if (wp->w_buffer == NULL) {
          /* Autocmd must have close the buffer already, skip. */
          continue;
        }

        buf_T *buf = wp->w_buffer;
        if (buf->b_changedtick != -1) {
          apply_autocmds(EVENT_BUFWINLEAVE, buf->b_fname,
              buf->b_fname, FALSE, buf);
          buf->b_changedtick = -1;            /* note that we did it already */
          /* start all over, autocommands may mess up the lists */
          next_tp = first_tabpage;
          break;
        }
      }
    }

    /* Trigger BufUnload for buffers that are loaded */
    FOR_ALL_BUFFERS(buf) {
      if (buf->b_ml.ml_mfp != NULL) {
        apply_autocmds(EVENT_BUFUNLOAD, buf->b_fname, buf->b_fname,
            FALSE, buf);
        if (!buf_valid(buf))            /* autocmd may delete the buffer */
          break;
      }
    }
    apply_autocmds(EVENT_VIMLEAVEPRE, NULL, NULL, FALSE, curbuf);
  }

  if (p_viminfo && *p_viminfo != NUL)
    /* Write out the registers, history, marks etc, to the viminfo file */
    write_viminfo(NULL, FALSE);

  if (get_vim_var_nr(VV_DYING) <= 1)
    apply_autocmds(EVENT_VIMLEAVE, NULL, NULL, FALSE, curbuf);

  profile_dump();

  if (did_emsg
     ) {
    /* give the user a chance to read the (error) message */
    no_wait_return = FALSE;
    wait_return(FALSE);
  }

  /* Position the cursor again, the autocommands may have moved it */
  ui_cursor_goto((int)Rows - 1, 0);

#if defined(USE_ICONV) && defined(DYNAMIC_ICONV)
  iconv_end();
#endif
  cs_end();
  if (garbage_collect_at_exit)
    garbage_collect();

  mch_exit(exitval);
}

/// Gets the integer value of a numeric command line argument if given,
/// such as '-o10'.
///
/// @param[in] p         pointer to argument
/// @param[in, out] idx  pointer to index in argument, is incremented
/// @param[in] def       default value
///
/// @return def unmodified if:
///   - argument isn't given
///   - argument is non-numeric
///
/// @return argument's numeric value otherwise
static int get_number_arg(const char *p, int *idx, int def)
{
  if (vim_isdigit(p[*idx])) {
    def = atoi(&(p[*idx]));
    while (vim_isdigit(p[*idx])) {
      *idx = *idx + 1;
    }
  }
  return def;
}

#if defined(HAVE_LOCALE_H) || defined(X_LOCALE)
/*
 * Setup to use the current locale (for ctype() and many other things).
 */
static void init_locale(void)
{
  setlocale(LC_ALL, "");

# ifdef LC_NUMERIC
  /* Make sure strtod() uses a decimal point, not a comma. */
  setlocale(LC_NUMERIC, "C");
# endif

  {
    int mustfree = FALSE;
    char_u  *p;

    /* expand_env() doesn't work yet, because chartab[] is not initialized
     * yet, call vim_getenv() directly */
    p = vim_getenv((char_u *)"VIMRUNTIME", &mustfree);
    if (p != NULL && *p != NUL) {
      vim_snprintf((char *)NameBuff, MAXPATHL, "%s/lang", p);
      bindtextdomain(VIMPACKAGE, (char *)NameBuff);
    }
    if (mustfree)
      free(p);
    textdomain(VIMPACKAGE);
  }
  TIME_MSG("locale set");
}
#endif


/*
 * Scan the command line arguments.
 */
static void command_line_scan(mparm_T *parmp)
{
  int argc = parmp->argc;
  char        **argv = parmp->argv;
  int argv_idx;                         /* index in argv[n][] */
  int had_minmin = FALSE;               /* found "--" argument */
  int want_argument;                    /* option argument with argument */
  int c;
  char_u      *p = NULL;
  long n;

  --argc;
  ++argv;
  argv_idx = 1;             /* active option letter is argv[0][argv_idx] */
  while (argc > 0) {
    /*
     * "+" or "+{number}" or "+/{pat}" or "+{command}" argument.
     */
    if (argv[0][0] == '+' && !had_minmin) {
      if (parmp->n_commands >= MAX_ARG_CMDS)
        mainerr(ME_EXTRA_CMD, NULL);
      argv_idx = -1;                /* skip to next argument */
      if (argv[0][1] == NUL)
        parmp->commands[parmp->n_commands++] = (char_u *)"$";
      else
        parmp->commands[parmp->n_commands++] = (char_u *)&(argv[0][1]);
    }
    /*
     * Optional argument.
     */
    else if (argv[0][0] == '-' && !had_minmin) {
      want_argument = FALSE;
      c = argv[0][argv_idx++];
      switch (c) {
        case NUL:                 /* "vim -"  read from stdin */
          if (exmode_active) {
            // "ex -" silent mode
            silent_mode = TRUE;
          } else {
            if (parmp->edit_type != EDIT_NONE) {
              mainerr(ME_TOO_MANY_ARGS, argv[0]);
            }
            parmp->edit_type = EDIT_STDIN;
          }
          argv_idx = -1;                  /* skip to next argument */
          break;

        case '-':                 /* "--" don't take any more option arguments */
          /* "--help" give help message */
          /* "--version" give version message */
          /* "--literal" take files literally */
          /* "--nofork" don't fork */
          /* "--noplugin[s]" skip plugins */
          /* "--cmd <cmd>" execute cmd before vimrc */
          if (STRICMP(argv[0] + argv_idx, "help") == 0)
            usage();
          else if (STRICMP(argv[0] + argv_idx, "version") == 0) {
            Columns = 80;                 /* need to init Columns */
            info_message = TRUE;           /* use mch_msg(), not mch_errmsg() */
            list_version();
            msg_putchar('\n');
            msg_didout = FALSE;
            mch_exit(0);
          } else if (STRICMP(argv[0] + argv_idx, "api-info") == 0) {
            msgpack_sbuffer* b = msgpack_sbuffer_new();
            msgpack_packer* p = msgpack_packer_new(b, msgpack_sbuffer_write);
            Object md = DICTIONARY_OBJ(api_metadata());
            msgpack_rpc_from_object(md, p);

            for (size_t i = 0; i < b->size; i++) {
              putchar(b->data[i]);
            }

            mch_exit(0);
          } else if (STRICMP(argv[0] + argv_idx, "headless") == 0) {
            parmp->headless = true;
          } else if (STRICMP(argv[0] + argv_idx, "embed") == 0) {
            embedded_mode = true;
            parmp->headless = true;
          } else if (STRNICMP(argv[0] + argv_idx, "literal", 7) == 0) {
#if !defined(UNIX)
            parmp->literal = TRUE;
#endif
          } else if (STRNICMP(argv[0] + argv_idx, "nofork", 6) == 0) {
          } else if (STRNICMP(argv[0] + argv_idx, "noplugin", 8) == 0)
            p_lpl = FALSE;
          else if (STRNICMP(argv[0] + argv_idx, "cmd", 3) == 0) {
            want_argument = TRUE;
            argv_idx += 3;
          } else if (STRNICMP(argv[0] + argv_idx, "startuptime", 11) == 0) {
            want_argument = TRUE;
            argv_idx += 11;
          } else {
            if (argv[0][argv_idx])
              mainerr(ME_UNKNOWN_OPTION, argv[0]);
            had_minmin = TRUE;
          }
          if (!want_argument)
            argv_idx = -1;                /* skip to next argument */
          break;

        case 'A':                 /* "-A" start in Arabic mode */
          set_option_value((char_u *)"arabic", 1L, NULL, 0);
          break;

        case 'b':                 /* "-b" binary mode */
          /* Needs to be effective before expanding file names, because
           * for Win32 this makes us edit a shortcut file itself,
           * instead of the file it links to. */
          set_options_bin(curbuf->b_p_bin, 1, 0);
          curbuf->b_p_bin = 1;                /* binary file I/O */
          break;

        case 'e':                 /* "-e" Ex mode */
          exmode_active = EXMODE_NORMAL;
          break;

        case 'E':                 /* "-E" Improved Ex mode */
          exmode_active = EXMODE_VIM;
          break;

        case 'f':                 /* "-f"  GUI: run in foreground. */
          break;

        case 'g':                 /* "-g" start GUI */
          main_start_gui();
          break;

        case 'F':                 /* "-F" start in Farsi mode: rl + fkmap set */
          p_fkmap = TRUE;
          set_option_value((char_u *)"rl", 1L, NULL, 0);
          break;

        case 'h':                 /* "-h" give help message */
          usage();
          break;

        case 'H':                 /* "-H" start in Hebrew mode: rl + hkmap set */
          p_hkmap = TRUE;
          set_option_value((char_u *)"rl", 1L, NULL, 0);
          break;

        case 'l':                 /* "-l" lisp mode, 'lisp' and 'showmatch' on */
          set_option_value((char_u *)"lisp", 1L, NULL, 0);
          p_sm = TRUE;
          break;

        case 'M':                 /* "-M"  no changes or writing of files */
          reset_modifiable();
          /* FALLTHROUGH */

        case 'm':                 /* "-m"  no writing of files */
          p_write = FALSE;
          break;

        case 'N':                 /* "-N"  Nocompatible */
          /* No-op */
          break;

        case 'n':                 /* "-n" no swap file */
          parmp->no_swap_file = TRUE;
          break;

        case 'p':                 /* "-p[N]" open N tab pages */
#ifdef TARGET_API_MAC_OSX
          /* For some reason on MacOS X, an argument like:
             -psn_0_10223617 is passed in when invoke from Finder
             or with the 'open' command */
          if (argv[0][argv_idx] == 's') {
            argv_idx = -1;           /* bypass full -psn */
            main_start_gui();
            break;
          }
#endif
          /* default is 0: open window for each file */
          parmp->window_count = get_number_arg(argv[0], &argv_idx, 0);
          parmp->window_layout = WIN_TABS;
          break;

        case 'o':                 /* "-o[N]" open N horizontal split windows */
          /* default is 0: open window for each file */
          parmp->window_count = get_number_arg(argv[0], &argv_idx, 0);
          parmp->window_layout = WIN_HOR;
          break;

        case 'O':                 /* "-O[N]" open N vertical split windows */
          /* default is 0: open window for each file */
          parmp->window_count = get_number_arg(argv[0], &argv_idx, 0);
          parmp->window_layout = WIN_VER;
          break;

        case 'q':                 /* "-q" QuickFix mode */
          if (parmp->edit_type != EDIT_NONE)
            mainerr(ME_TOO_MANY_ARGS, argv[0]);
          parmp->edit_type = EDIT_QF;
          if (argv[0][argv_idx]) {                /* "-q{errorfile}" */
            parmp->use_ef = (char_u *)argv[0] + argv_idx;
            argv_idx = -1;
          } else if (argc > 1)                    /* "-q {errorfile}" */
            want_argument = TRUE;
          break;

        case 'R':                 /* "-R" readonly mode */
          readonlymode = TRUE;
          curbuf->b_p_ro = TRUE;
          p_uc = 10000;                           /* don't update very often */
          break;

        case 'r':                 /* "-r" recovery mode */
        case 'L':                 /* "-L" recovery mode */
          recoverymode = 1;
          break;

        case 's':
          if (exmode_active)              /* "-s" silent (batch) mode */
            silent_mode = TRUE;
          else                    /* "-s {scriptin}" read from script file */
            want_argument = TRUE;
          break;

        case 't':                 /* "-t {tag}" or "-t{tag}" jump to tag */
          if (parmp->edit_type != EDIT_NONE)
            mainerr(ME_TOO_MANY_ARGS, argv[0]);
          parmp->edit_type = EDIT_TAG;
          if (argv[0][argv_idx]) {                /* "-t{tag}" */
            parmp->tagname = (char_u *)argv[0] + argv_idx;
            argv_idx = -1;
          } else                                  /* "-t {tag}" */
            want_argument = TRUE;
          break;

        case 'D':                 /* "-D"		Debugging */
          parmp->use_debug_break_level = 9999;
          break;
        case 'd':                 /* "-d"		'diff' */
          parmp->diff_mode = TRUE;
          break;
        case 'V':                 /* "-V{N}"	Verbose level */
          /* default is 10: a little bit verbose */
          p_verbose = get_number_arg(argv[0], &argv_idx, 10);
          if (argv[0][argv_idx] != NUL) {
            set_option_value((char_u *)"verbosefile", 0L,
                (char_u *)argv[0] + argv_idx, 0);
            argv_idx = (int)STRLEN(argv[0]);
          }
          break;

        case 'w':                 /* "-w{number}"	set window height */
          /* "-w {scriptout}"	write to script */
          if (vim_isdigit(((char_u *)argv[0])[argv_idx])) {
            n = get_number_arg(argv[0], &argv_idx, 10);
            set_option_value((char_u *)"window", n, NULL, 0);
            break;
          }
          want_argument = TRUE;
          break;

        case 'X':                 /* "-X"  don't connect to X server */
          break;

        case 'Z':                 /* "-Z"  restricted mode */
          restricted = TRUE;
          break;

        case 'c':                 /* "-c{command}" or "-c {command}" execute
                                     command */
          if (argv[0][argv_idx] != NUL) {
            if (parmp->n_commands >= MAX_ARG_CMDS)
              mainerr(ME_EXTRA_CMD, NULL);
            parmp->commands[parmp->n_commands++] = (char_u *)argv[0]
              + argv_idx;
            argv_idx = -1;
            break;
          }
          /*FALLTHROUGH*/
        case 'S':                 /* "-S {file}" execute Vim script */
        case 'i':                 /* "-i {viminfo}" use for viminfo */
        case 'T':                 /* "-T {terminal}" terminal name */
        case 'u':                 /* "-u {vimrc}" vim inits file */
        case 'U':                 /* "-U {gvimrc}" gvim inits file */
        case 'W':                 /* "-W {scriptout}" overwrite */
          want_argument = TRUE;
          break;

        default:
          mainerr(ME_UNKNOWN_OPTION, argv[0]);
      }

      /*
       * Handle option arguments with argument.
       */
      if (want_argument) {
        /*
         * Check for garbage immediately after the option letter.
         */
        if (argv[0][argv_idx] != NUL)
          mainerr(ME_GARBAGE, argv[0]);

        --argc;
        if (argc < 1 && c != 'S')          /* -S has an optional argument */
          mainerr(ME_ARG_MISSING, argv[0]);
        ++argv;
        argv_idx = -1;

        switch (c) {
          case 'c':               /* "-c {command}" execute command */
          case 'S':               /* "-S {file}" execute Vim script */
            if (parmp->n_commands >= MAX_ARG_CMDS)
              mainerr(ME_EXTRA_CMD, NULL);
            if (c == 'S') {
              char    *a;

              if (argc < 1)
                /* "-S" without argument: use default session file
                 * name. */
                a = SESSION_FILE;
              else if (argv[0][0] == '-') {
                /* "-S" followed by another option: use default
                 * session file name. */
                a = SESSION_FILE;
                ++argc;
                --argv;
              } else
                a = argv[0];
              p = xmalloc(STRLEN(a) + 4);
              sprintf((char *)p, "so %s", a);
              parmp->cmds_tofree[parmp->n_commands] = TRUE;
              parmp->commands[parmp->n_commands++] = p;
            } else
              parmp->commands[parmp->n_commands++] =
                (char_u *)argv[0];
            break;

          case '-':
            if (argv[-1][2] == 'c') {
              /* "--cmd {command}" execute command */
              if (parmp->n_pre_commands >= MAX_ARG_CMDS)
                mainerr(ME_EXTRA_CMD, NULL);
              parmp->pre_commands[parmp->n_pre_commands++] =
                (char_u *)argv[0];
            }
            /* "--startuptime <file>" already handled */
            break;

          case 'q':               /* "-q {errorfile}" QuickFix mode */
            parmp->use_ef = (char_u *)argv[0];
            break;

          case 'i':               /* "-i {viminfo}" use for viminfo */
            use_viminfo = (char_u *)argv[0];
            break;

          case 's':               /* "-s {scriptin}" read from script file */
            if (scriptin[0] != NULL) {
scripterror:
              mch_errmsg(_("Attempt to open script file again: \""));
              mch_errmsg(argv[-1]);
              mch_errmsg(" ");
              mch_errmsg(argv[0]);
              mch_errmsg("\"\n");
              mch_exit(2);
            }
            if ((scriptin[0] = mch_fopen(argv[0], READBIN)) == NULL) {
              mch_errmsg(_("Cannot open for reading: \""));
              mch_errmsg(argv[0]);
              mch_errmsg("\"\n");
              mch_exit(2);
            }
            save_typebuf();
            break;

          case 't':               /* "-t {tag}" */
            parmp->tagname = (char_u *)argv[0];
            break;

          case 'T':               /* "-T {terminal}" terminal name */
            /*
             * The -T term argument is always available and when
             * HAVE_TERMLIB is supported it overrides the environment
             * variable TERM.
             */
            parmp->term = (char_u *)argv[0];
            break;

          case 'u':               /* "-u {vimrc}" vim inits file */
            parmp->use_vimrc = (char_u *)argv[0];
            break;

          case 'U':               /* "-U {gvimrc}" gvim inits file */
            break;

          case 'w':               /* "-w {nr}" 'window' value */
            /* "-w {scriptout}" append to script file */
            if (vim_isdigit(*((char_u *)argv[0]))) {
              argv_idx = 0;
              n = get_number_arg(argv[0], &argv_idx, 10);
              set_option_value((char_u *)"window", n, NULL, 0);
              argv_idx = -1;
              break;
            }
            /*FALLTHROUGH*/
          case 'W':               /* "-W {scriptout}" overwrite script file */
            if (scriptout != NULL)
              goto scripterror;
            if ((scriptout = mch_fopen(argv[0],
                    c == 'w' ? APPENDBIN : WRITEBIN)) == NULL) {
              mch_errmsg(_("Cannot open for script output: \""));
              mch_errmsg(argv[0]);
              mch_errmsg("\"\n");
              mch_exit(2);
            }
            break;

        }
      }
    }
    /*
     * File name argument.
     */
    else {
      argv_idx = -1;                /* skip to next argument */

      /* Check for only one type of editing. */
      if (parmp->edit_type != EDIT_NONE && parmp->edit_type != EDIT_FILE)
        mainerr(ME_TOO_MANY_ARGS, argv[0]);
      parmp->edit_type = EDIT_FILE;

      /* Add the file to the global argument list. */
      ga_grow(&global_alist.al_ga, 1);
      p = vim_strsave((char_u *)argv[0]);

      if (parmp->diff_mode && os_isdir(p) && GARGCOUNT > 0
          && !os_isdir(alist_name(&GARGLIST[0]))) {
        char_u      *r;

        r = concat_fnames(p, path_tail(alist_name(&GARGLIST[0])), TRUE);
        free(p);
        p = r;
      }

#ifdef USE_FNAME_CASE
      /* Make the case of the file name match the actual file. */
      fname_case(p, 0);
#endif

      alist_add(&global_alist, p,
#if !defined(UNIX)
          parmp->literal ? 2 : 0                /* add buffer nr after exp. */
#else
          2                     /* add buffer number now and use curbuf */
#endif
          );

    }

    /*
     * If there are no more letters after the current "-", go to next
     * argument.  argv_idx is set to -1 when the current argument is to be
     * skipped.
     */
    if (argv_idx <= 0 || argv[0][argv_idx] == NUL) {
      --argc;
      ++argv;
      argv_idx = 1;
    }
  }

  /* If there is a "+123" or "-c" command, set v:swapcommand to the first
   * one. */
  if (parmp->n_commands > 0) {
    p = xmalloc(STRLEN(parmp->commands[0]) + 3);
    sprintf((char *)p, ":%s\r", parmp->commands[0]);
    set_vim_var_string(VV_SWAPCOMMAND, p, -1);
    free(p);
  }
  TIME_MSG("parsing arguments");
}

/*
 * Many variables are in "params" so that we can pass them to invoked
 * functions without a lot of arguments.  "argc" and "argv" are also
 * copied, so that they can be changed. */
static void init_params(mparm_T *paramp, int argc, char **argv)
{
  memset(paramp, 0, sizeof(*paramp));
  paramp->argc = argc;
  paramp->argv = argv;
  paramp->headless = false;
  paramp->want_full_screen = true;
  paramp->use_debug_break_level = -1;
  paramp->window_count = -1;
}

/*
 * Initialize global startuptime file if "--startuptime" passed as an argument.
 */
static void init_startuptime(mparm_T *paramp)
{
  for (int i = 1; i < paramp->argc; i++) {
    if (STRICMP(paramp->argv[i], "--startuptime") == 0 && i + 1 < paramp->argc) {
      time_fd = mch_fopen(paramp->argv[i + 1], "a");
      time_start("--- NVIM STARTING ---");
      break;
    }
  }

  starttime = time(NULL);
}

static void check_and_set_isatty(mparm_T *paramp)
{
  paramp->input_isatty = os_isatty(fileno(stdin));
  paramp->output_isatty = os_isatty(fileno(stdout));
  paramp->err_isatty = os_isatty(fileno(stderr));
  TIME_MSG("window checked");
}
/*
 * Get filename from command line, given that there is one.
 */
static char_u *get_fname(mparm_T *parmp)
{
#if !defined(UNIX)
  /*
   * Expand wildcards in file names.
   */
  if (!parmp->literal) {
    /* Temporarily add '(' and ')' to 'isfname'.  These are valid
     * filename characters but are excluded from 'isfname' to make
     * "gf" work on a file name in parenthesis (e.g.: see vim.h). */
    do_cmdline_cmd((char_u *)":set isf+=(,)");
    alist_expand(NULL, 0);
    do_cmdline_cmd((char_u *)":set isf&");
  }
#endif
  return alist_name(&GARGLIST[0]);
}

/*
 * Decide about window layout for diff mode after reading vimrc.
 */
static void set_window_layout(mparm_T *paramp)
{
  if (paramp->diff_mode && paramp->window_layout == 0) {
    if (diffopt_horizontal())
      paramp->window_layout = WIN_HOR;             /* use horizontal split */
    else
      paramp->window_layout = WIN_VER;             /* use vertical split */
  }
}

/*
 * Read all the plugin files.
 * Only when compiled with +eval, since most plugins need it.
 */
static void load_plugins(void)
{
  if (p_lpl) {
    source_runtime((char_u *)"plugin/**/*.vim", TRUE);
    TIME_MSG("loading plugins");
  }
}

/*
 * "-q errorfile": Load the error file now.
 * If the error file can't be read, exit before doing anything else.
 */
static void handle_quickfix(mparm_T *paramp)
{
  if (paramp->edit_type == EDIT_QF) {
    if (paramp->use_ef != NULL)
      set_string_option_direct((char_u *)"ef", -1,
          paramp->use_ef, OPT_FREE, SID_CARG);
    vim_snprintf((char *)IObuff, IOSIZE, "cfile %s", p_ef);
    if (qf_init(NULL, p_ef, p_efm, TRUE, IObuff) < 0) {
      ui_putc('\n');
      mch_exit(3);
    }
    TIME_MSG("reading errorfile");
  }
}

/*
 * Need to jump to the tag before executing the '-c command'.
 * Makes "vim -c '/return' -t main" work.
 */
static void handle_tag(char_u *tagname)
{
  if (tagname != NULL) {
    swap_exists_did_quit = FALSE;

    vim_snprintf((char *)IObuff, IOSIZE, "ta %s", tagname);
    do_cmdline_cmd(IObuff);
    TIME_MSG("jumping to tag");

    /* If the user doesn't want to edit the file then we quit here. */
    if (swap_exists_did_quit)
      getout(1);
  }
}

// Print a warning if stdout is not a terminal.
// When starting in Ex mode and commands come from a file, set Silent mode.
static void check_tty(mparm_T *parmp)
{
  if (parmp->headless) {
    return;
  }

  // is active input a terminal?
  if (exmode_active) {
    if (!parmp->input_isatty) {
      silent_mode = true;
    }
  } else if (parmp->want_full_screen && (!parmp->err_isatty
        && (!parmp->output_isatty || !parmp->input_isatty))) {

    if (!parmp->output_isatty) {
      mch_errmsg(_("Vim: Warning: Output is not to a terminal\n"));
    }

    if (!parmp->input_isatty) {
      mch_errmsg(_("Vim: Warning: Input is not from a terminal\n"));
    }

    ui_flush();

    if (scriptin[0] == NULL) {
      os_delay(2000L, true);
    }

    TIME_MSG("Warning delay");
  }
}

/*
 * Read text from stdin.
 */
static void read_stdin(void)
{
  int i;

  /* When getting the ATTENTION prompt here, use a dialog */
  swap_exists_action = SEA_DIALOG;
  no_wait_return = TRUE;
  i = msg_didany;
  set_buflisted(TRUE);
  (void)open_buffer(TRUE, NULL, 0);     /* create memfile and read file */
  no_wait_return = FALSE;
  msg_didany = i;
  TIME_MSG("reading stdin");
  check_swap_exists_action();
}

/*
 * Create the requested number of windows and edit buffers in them.
 * Also does recovery if "recoverymode" set.
 */
static void create_windows(mparm_T *parmp)
{
  int dorewind;
  int done = 0;

  /*
   * Create the number of windows that was requested.
   */
  if (parmp->window_count == -1)        /* was not set */
    parmp->window_count = 1;
  if (parmp->window_count == 0)
    parmp->window_count = GARGCOUNT;
  if (parmp->window_count > 1) {
    /* Don't change the windows if there was a command in .vimrc that
     * already split some windows */
    if (parmp->window_layout == 0)
      parmp->window_layout = WIN_HOR;
    if (parmp->window_layout == WIN_TABS) {
      parmp->window_count = make_tabpages(parmp->window_count);
      TIME_MSG("making tab pages");
    } else if (firstwin->w_next == NULL) {
      parmp->window_count = make_windows(parmp->window_count,
          parmp->window_layout == WIN_VER);
      TIME_MSG("making windows");
    } else
      parmp->window_count = win_count();
  } else
    parmp->window_count = 1;

  if (recoverymode) {                   /* do recover */
    msg_scroll = TRUE;                  /* scroll message up */
    ml_recover();
    if (curbuf->b_ml.ml_mfp == NULL)     /* failed */
      getout(1);
    do_modelines(0);                    /* do modelines */
  } else {
    /*
     * Open a buffer for windows that don't have one yet.
     * Commands in the .vimrc might have loaded a file or split the window.
     * Watch out for autocommands that delete a window.
     */
    /*
     * Don't execute Win/Buf Enter/Leave autocommands here
     */
    ++autocmd_no_enter;
    ++autocmd_no_leave;
    dorewind = TRUE;
    while (done++ < 1000) {
      if (dorewind) {
        if (parmp->window_layout == WIN_TABS)
          goto_tabpage(1);
        else
          curwin = firstwin;
      } else if (parmp->window_layout == WIN_TABS) {
        if (curtab->tp_next == NULL)
          break;
        goto_tabpage(0);
      } else {
        if (curwin->w_next == NULL)
          break;
        curwin = curwin->w_next;
      }
      dorewind = FALSE;
      curbuf = curwin->w_buffer;
      if (curbuf->b_ml.ml_mfp == NULL) {
        /* Set 'foldlevel' to 'foldlevelstart' if it's not negative. */
        if (p_fdls >= 0)
          curwin->w_p_fdl = p_fdls;
        /* When getting the ATTENTION prompt here, use a dialog */
        swap_exists_action = SEA_DIALOG;
        set_buflisted(TRUE);

        /* create memfile, read file */
        (void)open_buffer(FALSE, NULL, 0);

        if (swap_exists_action == SEA_QUIT) {
          if (got_int || only_one_window()) {
            /* abort selected or quit and only one window */
            did_emsg = FALSE;               /* avoid hit-enter prompt */
            getout(1);
          }
          /* We can't close the window, it would disturb what
           * happens next.  Clear the file name and set the arg
           * index to -1 to delete it later. */
          setfname(curbuf, NULL, NULL, FALSE);
          curwin->w_arg_idx = -1;
          swap_exists_action = SEA_NONE;
        } else
          handle_swap_exists(NULL);
        dorewind = TRUE;                        /* start again */
      }
      os_breakcheck();
      if (got_int) {
        (void)vgetc();          /* only break the file loading, not the rest */
        break;
      }
    }
    if (parmp->window_layout == WIN_TABS)
      goto_tabpage(1);
    else
      curwin = firstwin;
    curbuf = curwin->w_buffer;
    --autocmd_no_enter;
    --autocmd_no_leave;
  }
}

/*
 * If opened more than one window, start editing files in the other
 * windows.  make_windows() has already opened the windows.
 */
static void edit_buffers(mparm_T *parmp)
{
  int arg_idx;                          /* index in argument list */
  int i;
  int advance = TRUE;
  win_T       *win;

  /*
   * Don't execute Win/Buf Enter/Leave autocommands here
   */
  ++autocmd_no_enter;
  ++autocmd_no_leave;

  /* When w_arg_idx is -1 remove the window (see create_windows()). */
  if (curwin->w_arg_idx == -1) {
    win_close(curwin, TRUE);
    advance = FALSE;
  }

  arg_idx = 1;
  for (i = 1; i < parmp->window_count; ++i) {
    /* When w_arg_idx is -1 remove the window (see create_windows()). */
    if (curwin->w_arg_idx == -1) {
      ++arg_idx;
      win_close(curwin, TRUE);
      advance = FALSE;
      continue;
    }

    if (advance) {
      if (parmp->window_layout == WIN_TABS) {
        if (curtab->tp_next == NULL)            /* just checking */
          break;
        goto_tabpage(0);
      } else {
        if (curwin->w_next == NULL)             /* just checking */
          break;
        win_enter(curwin->w_next, false);
      }
    }
    advance = TRUE;

    /* Only open the file if there is no file in this window yet (that can
     * happen when .vimrc contains ":sall"). */
    if (curbuf == firstwin->w_buffer || curbuf->b_ffname == NULL) {
      curwin->w_arg_idx = arg_idx;
      /* Edit file from arg list, if there is one.  When "Quit" selected
       * at the ATTENTION prompt close the window. */
      swap_exists_did_quit = FALSE;
      (void)do_ecmd(0, arg_idx < GARGCOUNT
          ? alist_name(&GARGLIST[arg_idx]) : NULL,
          NULL, NULL, ECMD_LASTL, ECMD_HIDE, curwin);
      if (swap_exists_did_quit) {
        /* abort or quit selected */
        if (got_int || only_one_window()) {
          /* abort selected and only one window */
          did_emsg = FALSE;             /* avoid hit-enter prompt */
          getout(1);
        }
        win_close(curwin, TRUE);
        advance = FALSE;
      }
      if (arg_idx == GARGCOUNT - 1)
        arg_had_last = TRUE;
      ++arg_idx;
    }
    os_breakcheck();
    if (got_int) {
      (void)vgetc();            /* only break the file loading, not the rest */
      break;
    }
  }

  if (parmp->window_layout == WIN_TABS)
    goto_tabpage(1);
  --autocmd_no_enter;

  /* make the first window the current window */
  win = firstwin;
  /* Avoid making a preview window the current window. */
  while (win->w_p_pvw) {
    win = win->w_next;
    if (win == NULL) {
      win = firstwin;
      break;
    }
  }
  win_enter(win, false);

  --autocmd_no_leave;
  TIME_MSG("editing files in windows");
  if (parmp->window_count > 1 && parmp->window_layout != WIN_TABS)
    win_equal(curwin, FALSE, 'b');      /* adjust heights */
}

/*
 * Execute the commands from --cmd arguments "cmds[cnt]".
 */
static void exe_pre_commands(mparm_T *parmp)
{
  char_u      **cmds = parmp->pre_commands;
  int cnt = parmp->n_pre_commands;
  int i;

  if (cnt > 0) {
    curwin->w_cursor.lnum = 0;     /* just in case.. */
    sourcing_name = (char_u *)_("pre-vimrc command line");
    current_SID = SID_CMDARG;
    for (i = 0; i < cnt; ++i)
      do_cmdline_cmd(cmds[i]);
    sourcing_name = NULL;
    current_SID = 0;
    TIME_MSG("--cmd commands");
  }
}

/*
 * Execute "+", "-c" and "-S" arguments.
 */
static void exe_commands(mparm_T *parmp)
{
  int i;

  /*
   * We start commands on line 0, make "vim +/pat file" match a
   * pattern on line 1.  But don't move the cursor when an autocommand
   * with g`" was used.
   */
  msg_scroll = TRUE;
  if (parmp->tagname == NULL && curwin->w_cursor.lnum <= 1)
    curwin->w_cursor.lnum = 0;
  sourcing_name = (char_u *)"command line";
  current_SID = SID_CARG;
  for (i = 0; i < parmp->n_commands; ++i) {
    do_cmdline_cmd(parmp->commands[i]);
    if (parmp->cmds_tofree[i])
      free(parmp->commands[i]);
  }
  sourcing_name = NULL;
  current_SID = 0;
  if (curwin->w_cursor.lnum == 0)
    curwin->w_cursor.lnum = 1;

  if (!exmode_active)
    msg_scroll = FALSE;

  /* When started with "-q errorfile" jump to first error again. */
  if (parmp->edit_type == EDIT_QF)
    qf_jump(NULL, 0, 0, FALSE);
  TIME_MSG("executing command arguments");
}

/*
 * Source startup scripts.
 */
static void source_startup_scripts(mparm_T *parmp)
{
  int i;

  /*
   * If -u argument given, use only the initializations from that file and
   * nothing else.
   */
  if (parmp->use_vimrc != NULL) {
    if (STRCMP(parmp->use_vimrc, "NONE") == 0
        || STRCMP(parmp->use_vimrc, "NORC") == 0) {
      if (parmp->use_vimrc[2] == 'N')
        p_lpl = FALSE;                      /* don't load plugins either */
    } else {
      if (do_source(parmp->use_vimrc, FALSE, DOSO_NONE) != OK)
        EMSG2(_("E282: Cannot read from \"%s\""), parmp->use_vimrc);
    }
  } else if (!silent_mode) {

    /*
     * Get system wide defaults, if the file name is defined.
     */
#ifdef SYS_VIMRC_FILE
    (void)do_source((char_u *)SYS_VIMRC_FILE, FALSE, DOSO_NONE);
#endif

    /*
     * Try to read initialization commands from the following places:
     * - environment variable VIMINIT
     * - user vimrc file (~/.vimrc)
     * - second user vimrc file ($VIM/.vimrc for Dos)
     * - environment variable EXINIT
     * - user exrc file (~/.exrc)
     * - second user exrc file ($VIM/.exrc for Dos)
     * The first that exists is used, the rest is ignored.
     */
    if (process_env((char_u *)"VIMINIT", TRUE) != OK) {
      if (do_source((char_u *)USR_VIMRC_FILE, TRUE, DOSO_VIMRC) == FAIL
#ifdef USR_VIMRC_FILE2
          && do_source((char_u *)USR_VIMRC_FILE2, TRUE,
            DOSO_VIMRC) == FAIL
#endif
#ifdef USR_VIMRC_FILE3
          && do_source((char_u *)USR_VIMRC_FILE3, TRUE,
            DOSO_VIMRC) == FAIL
#endif
#ifdef USR_VIMRC_FILE4
          && do_source((char_u *)USR_VIMRC_FILE4, TRUE,
            DOSO_VIMRC) == FAIL
#endif
          && process_env((char_u *)"EXINIT", FALSE) == FAIL
          && do_source((char_u *)USR_EXRC_FILE, FALSE, DOSO_NONE) == FAIL) {
#ifdef USR_EXRC_FILE2
        (void)do_source((char_u *)USR_EXRC_FILE2, FALSE, DOSO_NONE);
#endif
      }
    }

    /*
     * Read initialization commands from ".vimrc" or ".exrc" in current
     * directory.  This is only done if the 'exrc' option is set.
     * Because of security reasons we disallow shell and write commands
     * now, except for unix if the file is owned by the user or 'secure'
     * option has been reset in environment of global ".exrc" or ".vimrc".
     * Only do this if VIMRC_FILE is not the same as USR_VIMRC_FILE or
     * SYS_VIMRC_FILE.
     */
    if (p_exrc) {
#if defined(UNIX)
      /* If ".vimrc" file is not owned by user, set 'secure' mode. */
      if (!file_owned(VIMRC_FILE))
#endif
        secure = p_secure;

      i = FAIL;
      if (path_full_compare((char_u *)USR_VIMRC_FILE,
            (char_u *)VIMRC_FILE, FALSE) != kEqualFiles
#ifdef USR_VIMRC_FILE2
          && path_full_compare((char_u *)USR_VIMRC_FILE2,
            (char_u *)VIMRC_FILE, FALSE) != kEqualFiles
#endif
#ifdef USR_VIMRC_FILE3
          && path_full_compare((char_u *)USR_VIMRC_FILE3,
            (char_u *)VIMRC_FILE, FALSE) != kEqualFiles
#endif
#ifdef SYS_VIMRC_FILE
          && path_full_compare((char_u *)SYS_VIMRC_FILE,
            (char_u *)VIMRC_FILE, FALSE) != kEqualFiles
#endif
         )
        i = do_source((char_u *)VIMRC_FILE, TRUE, DOSO_VIMRC);

      if (i == FAIL) {
#if defined(UNIX)
        /* if ".exrc" is not owned by user set 'secure' mode */
        if (!file_owned(EXRC_FILE))
          secure = p_secure;
        else
          secure = 0;
#endif
        if (       path_full_compare((char_u *)USR_EXRC_FILE,
              (char_u *)EXRC_FILE, FALSE) != kEqualFiles
#ifdef USR_EXRC_FILE2
            && path_full_compare((char_u *)USR_EXRC_FILE2,
              (char_u *)EXRC_FILE, FALSE) != kEqualFiles
#endif
           )
          (void)do_source((char_u *)EXRC_FILE, FALSE, DOSO_NONE);
      }
    }
    if (secure == 2)
      need_wait_return = TRUE;
    secure = 0;
  }
  TIME_MSG("sourcing vimrc file(s)");
}

/*
 * Setup to start using the GUI.  Exit with an error when not available.
 */
static void main_start_gui(void)
{
  mch_errmsg(_(e_nogvim));
  mch_errmsg("\n");
  mch_exit(2);
}


/*
 * Get an environment variable, and execute it as Ex commands.
 * Returns FAIL if the environment variable was not executed, OK otherwise.
 */
int
process_env (
    char_u *env,
    int is_viminit             /* when TRUE, called for VIMINIT */
)
{
  char_u      *initstr;
  char_u      *save_sourcing_name;
  linenr_T save_sourcing_lnum;
  scid_T save_sid;

  initstr = (char_u *)os_getenv((char *)env);
  if (initstr != NULL && *initstr != NUL) {
    if (is_viminit)
      vimrc_found(NULL, NULL);
    save_sourcing_name = sourcing_name;
    save_sourcing_lnum = sourcing_lnum;
    sourcing_name = env;
    sourcing_lnum = 0;
    save_sid = current_SID;
    current_SID = SID_ENV;
    do_cmdline_cmd(initstr);
    sourcing_name = save_sourcing_name;
    sourcing_lnum = save_sourcing_lnum;
    current_SID = save_sid;;
    return OK;
  }
  return FAIL;
}

#ifdef UNIX
/// Checks if user owns file.
/// Use both uv_fs_stat() and uv_fs_lstat() through os_fileinfo() and
/// os_fileinfo_link() respectively for extra security.
static bool file_owned(const char *fname)
{
  uid_t uid = getuid();
  FileInfo file_info;
  bool file_owned = os_fileinfo(fname, &file_info)
                    && file_info.stat.st_uid == uid;
  bool link_owned = os_fileinfo_link(fname, &file_info)
                    && file_info.stat.st_uid == uid;
  return file_owned && link_owned;
}
#endif

/// Prints the following then exits:
/// - An error message main_errors[n]
/// - A string str if not null
///
/// @param n    error number represented by an ME_* macro
/// @param str  string to append to the primary error message, or NULL
static void mainerr(int n, const char *str)
{
  signal_stop();              // kill us with CTRL-C here, if you like

  mch_errmsg(_(main_errors[n]));
  if (str != NULL) {
    mch_errmsg(": \"");
    mch_errmsg(str);
    mch_errmsg("\"");
  }
  mch_errmsg(_("\nMore info with \"nvim -h\"\n"));

  mch_exit(1);
}


/// Prints help message and exits; used for 'nvim -h' & 'nvim --help'
static void usage(void)
{
  signal_stop();              // kill us with CTRL-C here, if you like

  mch_msg(_("Usage:\n"));
  mch_msg(_("  nvim [arguments] [file ...]      Edit specified file(s)\n"));
  mch_msg(_("  nvim [arguments] -               Read text from stdin\n"));
  mch_msg(_("  nvim [arguments] -t <tag>        Edit file where tag is defined\n"));
  mch_msg(_("  nvim [arguments] -q [errorfile]  Edit file with first error\n"));
  mch_msg(_("\nArguments:\n"));
  mch_msg(_("  --                    Only file names after this\n"));
#if !defined(UNIX)
  mch_msg(_("  --literal             Don't expand wildcards\n"));
#endif
  mch_msg(_("  -e                    Ex mode\n"));
  mch_msg(_("  -E                    Improved Ex mode\n"));
  mch_msg(_("  -s                    Silent (batch) mode (only for ex mode)\n"));
  mch_msg(_("  -d                    Diff mode\n"));
  mch_msg(_("  -R                    Readonly mode\n"));
  mch_msg(_("  -Z                    Restricted mode\n"));
  mch_msg(_("  -m                    Modifications (writing files) not allowed\n"));
  mch_msg(_("  -M                    Modifications in text not allowed\n"));
  mch_msg(_("  -b                    Binary mode\n"));
  mch_msg(_("  -l                    Lisp mode\n"));
  mch_msg(_("  -V[N][file]           Be verbose [level N][log messages to file]\n"));
  mch_msg(_("  -D                    Debugging mode\n"));
  mch_msg(_("  -n                    No swap file, use memory only\n"));
  mch_msg(_("  -r                    List swap files and exit\n"));
  mch_msg(_("  -r <file>             Recover crashed session\n"));
  mch_msg(_("  -A                    Start in Arabic mode\n"));
  mch_msg(_("  -F                    Start in Farsi mode\n"));
  mch_msg(_("  -H                    Start in Hebrew mode\n"));
  mch_msg(_("  -T <terminal>         Set terminal type to <terminal>\n"));
  mch_msg(_("  -u <nvimrc>           Use <nvimrc> instead of any .nvimrc\n"));
  mch_msg(_("  --noplugin            Don't load plugin scripts\n"));
  mch_msg(_("  -p[N]                 Open N tab pages (default: one for each file)\n"));
  mch_msg(_("  -o[N]                 Open N windows (default: one for each file)\n"));
  mch_msg(_("  -O[N]                 Like -o but split vertically\n"));
  mch_msg(_("  +                     Start at end of file\n"));
  mch_msg(_("  +<lnum>               Start at line <lnum>\n"));
  mch_msg(_("  --cmd <command>       Execute <command> before loading any nvimrc\n"));
  mch_msg(_("  -c <command>          Execute <command> after loading the first file\n"));
  mch_msg(_("  -S <session>          Source file <session> after loading the first file\n"));
  mch_msg(_("  -s <scriptin>         Read Normal mode commands from file <scriptin>\n"));
  mch_msg(_("  -w <scriptout>        Append all typed commands to file <scriptout>\n"));
  mch_msg(_("  -W <scriptout>        Write all typed commands to file <scriptout>\n"));
  mch_msg(_("  --startuptime <file>  Write startup timing messages to <file>\n"));
  mch_msg(_("  -i <nviminfo>         Use <nviminfo> instead of .nviminfo\n"));
  mch_msg(_("  --api-info            Dump API metadata serialized to msgpack and exit\n"));
  mch_msg(_("  --embed               Use stdin/stdout as a msgpack-rpc channel\n"));
  mch_msg(_("  --headless            Don't start a user interface\n"));
  mch_msg(_("  --version             Print version information and exit\n"));
  mch_msg(_("  -h | --help           Print this help message and exit\n"));

  mch_exit(0);
}


/*
 * Check the result of the ATTENTION dialog:
 * When "Quit" selected, exit Vim.
 * When "Recover" selected, recover the file.
 */
static void check_swap_exists_action(void)
{
  if (swap_exists_action == SEA_QUIT)
    getout(1);
  handle_swap_exists(NULL);
}
