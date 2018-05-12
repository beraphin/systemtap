// -*- C++ -*-
// Copyright (C) 2005-2016 Red Hat Inc.
//
// This file is part of systemtap, and is free software.  You can
// redistribute it and/or modify it under the terms of the GNU General
// Public License (GPL); either version 2, or (at your option) any
// later version.

#ifndef SESSION_H
#define SESSION_H

#include "config.h"
#if ENABLE_NLS
#include <libintl.h>
#include <locale.h>
#endif

#include <list>
#include <string>
#include <vector>
#include <iostream>
#include <sstream>
#include <map>
#include <set>
#include <stdexcept>

extern "C" {
#include <signal.h>
#include <elfutils/libdw.h>
}

#include "privilege.h"
#include "util.h"
#include "stringtable.h"

/* statistical operations used with a global */
#define STAT_OP_NONE      1 << 0
#define STAT_OP_COUNT     1 << 1
#define STAT_OP_SUM       1 << 2
#define STAT_OP_MIN       1 << 3
#define STAT_OP_MAX       1 << 4
#define STAT_OP_AVG       1 << 5
#define STAT_OP_VARIANCE  1 << 6

// forward decls for all referenced systemtap types
class stap_hash;
class match_node;
struct stapfile;
struct vardecl;
struct token;
struct functiondecl;
struct derived_probe;
struct be_derived_probe_group;
struct generic_kprobe_derived_probe_group;
struct hwbkpt_derived_probe_group;
struct perf_derived_probe_group;
struct uprobe_derived_probe_group;
struct utrace_derived_probe_group;
struct itrace_derived_probe_group;
struct task_finder_derived_probe_group;
struct timer_derived_probe_group;
struct netfilter_derived_probe_group;
struct profile_derived_probe_group;
struct mark_derived_probe_group;
struct tracepoint_derived_probe_group;
struct hrtimer_derived_probe_group;
struct procfs_derived_probe_group;
struct dynprobe_derived_probe_group;
struct python_derived_probe_group;
struct embeddedcode;
struct stapdfa;
class translator_output;
struct unparser;
struct semantic_error;
struct module_cache;
struct update_visitor;
struct compile_server_cache;

// XXX: a generalized form of this descriptor could be associated with
// a vardecl instead of out here at the systemtap_session level.
struct statistic_decl
{
  statistic_decl()
    : type(none),
      linear_low(0), linear_high(0), linear_step(0), bit_shift(0), stat_ops(0)
  {}
  enum { none, linear, logarithmic } type;
  int64_t linear_low;
  int64_t linear_high;
  int64_t linear_step;
  int bit_shift;
  int stat_ops;
  bool operator==(statistic_decl const & other)
  {
    return type == other.type
      && linear_low == other.linear_low
      && linear_high == other.linear_high
      && linear_step == other.linear_step;
  }
};

struct macrodecl; // defined in parse.h

struct parse_error: public std::runtime_error
{
  const token* tok;
  bool skip_some;
  const parse_error *chain;
  const std::string errsrc;
  ~parse_error () throw () {}
  parse_error (const std::string& src, const std::string& msg):
    runtime_error (msg), tok (0), skip_some (true), chain(0), errsrc(src) {}
  parse_error (const std::string& src, const std::string& msg, const token* t):
    runtime_error (msg), tok (t), skip_some (true), chain(0), errsrc(src) {}
  parse_error (const std::string& src, const std::string& msg, bool skip):
    runtime_error (msg), tok (0), skip_some (skip), chain(0), errsrc(src) {}

  std::string errsrc_chain(void) const
    {
      return errsrc + (chain ? "|" + chain->errsrc_chain() : "");
    }
};

struct systemtap_session
{
private:
  // disable implicit constructors by not implementing these
  systemtap_session (const systemtap_session& other);
  systemtap_session& operator= (const systemtap_session& other);

  // copy constructor used by ::clone()
  systemtap_session (const systemtap_session& other,
                     const std::string& arch,
                     const std::string& kern);

public:
  systemtap_session ();
  ~systemtap_session ();

  // To reset the tmp_dir
  void create_tmp_dir();
  void remove_tmp_dir();
  void reset_tmp_dir();

  // NB: It is very important for all of the above (and below) fields
  // to be cleared in the systemtap_session ctor (session.cxx).
  void setup_kernel_release (const char* kstr);
  void insert_loaded_modules ();

  // command line parsing
  int  parse_cmdline (int argc, char * const argv []);
  bool parse_cmdline_runtime (const std::string& opt_runtime);
  std::string version_string ();
  void version ();
  void usage (int exitcode);
  void check_options (int argc, char * const argv []);
  static const char* morehelp;

  // NB: It is very important for all of the above (and below) fields
  // to be cleared in the systemtap_session ctor (session.cxx).

  // command line args
  std::string script_file; // FILE
  std::string cmdline_script; // -e PROGRAM
  std::vector<std::string> additional_scripts; // -E SCRIPT
  bool have_script;
  std::vector<std::string> include_path;
  int include_arg_start;
  std::vector<std::string> c_macros;
  std::vector<std::string> args;
  std::vector<bool> used_args;
  std::vector<std::string> kbuildflags; // -B var=val
  std::vector<std::string> globalopts; // -G var=val
  std::vector<std::string> modinfos; // --modinfo tag=value

  std::string release;
  std::string kernel_release;
  std::string kernel_base_release;
  std::string kernel_build_tree;
  std::string kernel_source_tree;
  std::vector<std::string> kernel_extra_cflags; 
  std::map<interned_string,interned_string> kernel_config;
  std::set<interned_string> kernel_exports;
  std::set<interned_string> kernel_functions;
  int parse_kernel_config ();
  int parse_kernel_exports ();
  int parse_kernel_functions ();

  std::string sysroot;
  std::map<std::string,std::string> sysenv;
  bool update_release_sysroot;
  std::string machine;
  std::string architecture;
  bool native_build;
  std::string runtime_path;
  bool runtime_specified;
  std::string data_path;
  std::string module_name;
  const std::string module_filename() const;
  std::string stapconf_name;
  std::string output_file;
  std::string size_option;
  std::string cmd;
  std::string cmd_file();
  std::string compatible; // use (strverscmp(s.compatible.c_str(), "N.M") >= 0)
  int target_pid;
  int last_pass;
  unsigned perpass_verbose[5];
  unsigned verbose;
  bool timing;
  bool save_module;
  bool save_uprobes;
  bool modname_given;
  bool keep_tmpdir;
  bool guru_mode;
  bool bulk_mode;
  bool unoptimized;
  bool suppress_warnings;
  bool panic_warnings;
  int buffer_size;
  bool tapset_compile_coverage;
  bool need_uprobes;
  bool need_unwind;
  bool need_symbols;
  bool need_lines;
  std::string uprobes_path;
  std::string uprobes_hash;
  bool load_only; // flight recorder mode
  privilege_t privilege;
  bool privilege_set;
  bool systemtap_v_check;
  bool tmpdir_opt_set;
  bool read_stdin;
  bool monitor;
  int monitor_interval;
  int timeout; // in ms

  enum
   { dump_none,               // no dumping requested
     dump_probe_types,        // dump standard tapset probes
     dump_probe_aliases,      // dump tapset probe aliases
     dump_functions,          // dump tapset functions
     dump_matched_probes,     // dump matching probes (-l)
     dump_matched_probes_vars // dump matching probes and their variables (-L)
   } dump_mode;

  // Pattern to match against in listing mode (-l/-L)
  std::string dump_matched_pattern;

  int download_dbinfo;
  bool suppress_handler_errors;
  bool suppress_time_limits;
  bool color_errors;
  bool interactive_mode;
  bool pass_1a_complete;

  enum { color_never, color_auto, color_always } color_mode;
  enum { prologue_searching_never, prologue_searching_auto, prologue_searching_always } prologue_searching_mode;

  enum { kernel_runtime, dyninst_runtime, bpf_runtime } runtime_mode;
  bool runtime_usermode_p() const { return runtime_mode == dyninst_runtime; }

  // NB: It is very important for all of the above (and below) fields
  // to be cleared in the systemtap_session ctor (session.cxx).

  // Client/server
#if HAVE_NSS
  static bool NSPR_Initialized; // only once for all sessions
  void NSPR_init ();
#endif
  bool client_options;
  std::string client_options_disallowed_for_unprivileged;
  std::vector<std::string> server_status_strings;
  std::vector<std::string> specified_servers;
  std::string server_trust_spec;
  std::vector<std::string> server_args;
  std::string winning_server;
  compile_server_cache* server_cache;
  std::vector<std::string> mok_fingerprints;
  std::string auto_privilege_level_msg;
  std::vector<std::string> auto_server_msgs;

  bool modules_must_be_signed();
  void get_mok_info();

  // NB: It is very important for all of the above (and below) fields
  // to be cleared in the systemtap_session ctor (session.cxx).

  // Mechanism for retrying compilation with a compile server should it fail due
  // to lack of resources on the local host.
  // Once it has been decided not to try the server (e.g. syntax error),
  // that decision cannot be changed.
  int try_server_status;
  bool use_server_on_error;

  enum { try_server_unset, dont_try_server, do_try_server };
  void init_try_server ();
  void set_try_server (int t = do_try_server);
  bool try_server () const { return try_server_status == do_try_server; }

  // HTTP client/server
  std::vector<std::string> http_servers;

  // NB: It is very important for all of the above (and below) fields
  // to be cleared in the systemtap_session ctor (session.cxx).

  // Remote execution
  std::vector<std::string> remote_uris;
  bool use_remote_prefix;
  typedef std::map<std::pair<std::string, std::string>, systemtap_session*> session_map_t;
  session_map_t subsessions;
  systemtap_session* clone(const std::string& arch, const std::string& release);

  // NB: It is very important for all of the above (and below) fields
  // to be cleared in the systemtap_session ctor (session.cxx).

  // Cache data
  bool use_cache;               // control all caching
  bool use_script_cache;        // control caching of pass-3/4 output
  bool poison_cache;            // consider the cache to be write-only
  std::string cache_path;       // usually ~/.systemtap/cache
  std::string hash_path;        // path to the cached script module
  std::string stapconf_path;    // path to the cached stapconf
  stap_hash *base_hash;         // hash common to all caching

  // Skip bad $ vars
  bool skip_badvars;

  // NB: It is very important for all of the above (and below) fields
  // to be cleared in the systemtap_session ctor (session.cxx).

  // temporary directory for module builds etc.
  // hazardous - it is "rm -rf"'d at exit
  std::string tmpdir;
  std::string translated_source; // C source code

  match_node* pattern_root;
  void register_library_aliases();

  // data for various preprocessor library macros
  std::map<std::string, macrodecl*> library_macros;

  // parse trees for the various script files
  std::vector<stapfile*> user_files;
  std::vector<stapfile*> library_files;

  // filters to run over all code before symbol resolution
  //   e.g. @cast expansion
  std::vector<update_visitor*> code_filters;

  // resolved globals/functions/probes for the run as a whole
  std::vector<stapfile*> files;
  std::vector<vardecl*> globals;
  std::map<std::string,functiondecl*> functions;
  std::map<std::string,unsigned> overload_count;
  // probe counter name -> probe associated with counter
  std::vector<std::pair<std::string,std::string> > perf_counters;
  std::vector<derived_probe*> probes; // see also *_probes groups below
  std::vector<embeddedcode*> embeds;
  std::map<interned_string, statistic_decl> stat_decls;
  // track things that are removed
  std::vector<vardecl*> unused_globals;
  std::vector<derived_probe*> unused_probes; // see also *_probes groups below
  std::vector<functiondecl*> unused_functions;

  // resolved/compiled regular expressions for the run
  std::map<std::string, stapdfa*> dfas;
  unsigned dfa_counter; // used to give unique names
  unsigned dfa_maxmap;  // used for subexpression-tracking data structure
  unsigned dfa_maxtag;  // ditto
  bool need_tagged_dfa; // triggered by /* pragma:tagged_dfa */

  // Every probe in these groups must also appear in the
  // session.probes vector.
  be_derived_probe_group* be_derived_probes;
  generic_kprobe_derived_probe_group* generic_kprobe_derived_probes;
  hwbkpt_derived_probe_group* hwbkpt_derived_probes;
  perf_derived_probe_group* perf_derived_probes;
  uprobe_derived_probe_group* uprobe_derived_probes;
  utrace_derived_probe_group* utrace_derived_probes;
  itrace_derived_probe_group* itrace_derived_probes;
  task_finder_derived_probe_group* task_finder_derived_probes;
  timer_derived_probe_group* timer_derived_probes;
  netfilter_derived_probe_group* netfilter_derived_probes;
  profile_derived_probe_group* profile_derived_probes;
  mark_derived_probe_group* mark_derived_probes;
  tracepoint_derived_probe_group* tracepoint_derived_probes;
  hrtimer_derived_probe_group* hrtimer_derived_probes;
  procfs_derived_probe_group* procfs_derived_probes;
  dynprobe_derived_probe_group* dynprobe_derived_probes;
  python_derived_probe_group* python_derived_probes;

  // NB: It is very important for all of the above (and below) fields
  // to be cleared in the systemtap_session ctor (session.cxx).

  // unparser data
  translator_output* op;
  std::vector<translator_output*> auxiliary_outputs;
  unparser* up;

  // some symbol addresses
  // XXX: these belong elsewhere; perhaps the dwflpp instance
  Dwarf_Addr sym_kprobes_text_start;
  Dwarf_Addr sym_kprobes_text_end;
  Dwarf_Addr sym_stext;

  // List of libdwfl module names to extract symbol/unwind data for.
  std::set<std::string> unwindsym_modules;
  bool unwindsym_ldd;
  struct module_cache* module_cache;
  std::vector<std::string> build_ids;

  // Secret benchmarking options
  unsigned long benchmark_sdt_loops;
  unsigned long benchmark_sdt_threads;

  // NB: It is very important for all of the above (and below) fields
  // to be cleared in the systemtap_session ctor (session.cxx).

  std::set<std::string> seen_warnings;
  int suppressed_warnings;
  std::map<std::string, int> seen_errors; // NB: can change to a set if threshold is 1
  int suppressed_errors;
  int warningerr_count; // see comment in systemtap_session::print_error

  // Returns number of critical errors (not counting those part of warnings)
  unsigned num_errors ()
    {
      return (seen_errors.size() // all the errors we've encountered
        - warningerr_count       // except those considered warningerrs
        + (panic_warnings ? seen_warnings.size() : 0)); // plus warnings if -W given
    }

  std::set<std::string> rpms_checked; // enlisted by filename+rpm_type
  std::set<std::string> rpms_to_install; // resulting rpms

  translator_output* op_create_auxiliary(bool trailer_p = false);

  int target_namespaces_pid;

  const token* last_token;
  void print_token (std::ostream& o, const token* tok);
  void print_error (const semantic_error& e);
  std::string build_error_msg (const semantic_error& e);
  void print_error_source (std::ostream&, std::string&, const token* tok);
  void print_error_details (std::ostream&, std::string&, const semantic_error&);
  void print_error (const parse_error &pe,
                    const token* tok,
                    const std::string &input_name,
                    bool is_warningerr = false);
  std::string build_error_msg (const parse_error &pe,
                               const token* tok,
                               const std::string &input_name);
  void print_warning (const std::string& w, const token* tok = 0);
  void printscript(std::ostream& o);
  void report_suppression();

  // NB: It is very important for all of the above (and below) fields
  // to be cleared in the systemtap_session ctor (session.cxx).

  std::string colorize(const std::string& str, const std::string& type);
  std::string colorize(const token* tok);
  std::string parse_stap_color(const std::string& type);

  // Some automatic options settings require explanation.
  void enable_auto_server (const std::string &message);
  void explain_auto_options();

  bool is_user_file (const std::string& name);
  bool is_primary_probe (derived_probe *dp);

  void clear_script_data();

  // NB: It is very important for all of the above (and below) fields
  // to be cleared in the systemtap_session ctor (session.cxx).
};

struct exit_exception: public std::runtime_error
{
  int rc;
  exit_exception (int rc):
    runtime_error (_F("early exit requested, rc=%d", rc)), rc(rc) {}
};


// global counter of SIGINT/SIGTERM's received
extern int pending_interrupts;

// Interrupt exception subclass for catching
// interrupts (i.e. ctrl-c).
struct interrupt_exception: public std::runtime_error
{
  interrupt_exception ():
    runtime_error (_("interrupt received")){}
};

void assert_no_interrupts();

#endif // SESSION_H

/* vim: set sw=2 ts=8 cino=>4,n-2,{2,^-2,t0,(0,u0,w1,M1 : */
