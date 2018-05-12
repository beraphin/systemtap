// session functions
// Copyright (C) 2010-2017 Red Hat Inc.
//
// This file is part of systemtap, and is free software.  You can
// redistribute it and/or modify it under the terms of the GNU General
// Public License (GPL); either version 2, or (at your option) any
// later version.

#include "config.h"
#include "session.h"
#include "cache.h"
#include "stapregex.h"
#include "elaborate.h"
#include "translate.h"
#include "buildrun.h"
#include "coveragedb.h"
#include "hash.h"
#include "setupdwfl.h"
#include "task_finder.h"
#include "csclient.h"
#include "rpm_finder.h"
#include "util.h"
#include "cmdline.h"
#include "git_version.h"
#include "version.h"
#include "stringtable.h"
#include "tapsets.h"

#include <cerrno>
#include <cstdlib>
#include <thread>

extern "C" {
#include <getopt.h>
#include <limits.h>
#include <grp.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/resource.h>
#include <elfutils/libdwfl.h>
#include <elfutils/version.h>
#include <unistd.h>
#include <sys/wait.h>
#include <wordexp.h>
}

#if HAVE_NSS
extern "C" {
#include <nspr.h>
}
#endif

#include <string>

using namespace std;

/* getopt variables */
extern int optind;

#define PATH_TBD string("__TBD__")

#if HAVE_NSS
bool systemtap_session::NSPR_Initialized = false;
#endif

systemtap_session::systemtap_session ():
  // NB: pointer members must be manually initialized!
  // NB: don't forget the copy constructor too!
  runtime_mode(kernel_runtime),
  base_hash(0),
  pattern_root(new match_node),
  dfa_counter (0),
  dfa_maxmap (0),
  dfa_maxtag (0),
  need_tagged_dfa (false),
  be_derived_probes(0),
  generic_kprobe_derived_probes(0),
  hwbkpt_derived_probes(0),
  perf_derived_probes(0),
  uprobe_derived_probes(0),
  utrace_derived_probes(0),
  itrace_derived_probes(0),
  task_finder_derived_probes(0),
  timer_derived_probes(0),
  netfilter_derived_probes(0),
  profile_derived_probes(0),
  mark_derived_probes(0),
  tracepoint_derived_probes(0),
  hrtimer_derived_probes(0),
  procfs_derived_probes(0),
  dynprobe_derived_probes(0),
  python_derived_probes(0),
  op (0), up (0),
  sym_kprobes_text_start (0),
  sym_kprobes_text_end (0),
  sym_stext (0),
  module_cache (0),
  benchmark_sdt_loops(0),
  benchmark_sdt_threads(0),
  suppressed_warnings(0),
  suppressed_errors(0),
  warningerr_count(0),
  target_namespaces_pid(0),
  last_token (0)
{
  struct utsname buf;
  (void) uname (& buf);
  kernel_release = string (buf.release);
  release = kernel_release;
  kernel_build_tree = "/lib/modules/" + kernel_release + "/build";
  architecture = machine = normalize_machine(buf.machine);

  for (unsigned i=0; i<5; i++) perpass_verbose[i]=0;
  verbose = 0;

  have_script = false;
  runtime_specified = false;
  include_arg_start = -1;
  timing = false;
  guru_mode = false;
  bulk_mode = false;
  unoptimized = false;
  suppress_warnings = false;
  panic_warnings = false;
  dump_mode = systemtap_session::dump_none;
  dump_matched_pattern = "";

#ifdef ENABLE_PROLOGUES
  prologue_searching_mode = prologue_searching_always;
#else
  prologue_searching_mode = prologue_searching_auto;
#endif

  buffer_size = 0;
  last_pass = 5;
  module_name = "stap_" + lex_cast(getpid());
  stapconf_name = "stapconf_" + lex_cast(getpid()) + ".h";
  output_file = ""; // -o FILE
  tmpdir_opt_set = false;
  monitor = false;
  monitor_interval = 1;
  read_stdin = false;
  save_module = false;
  save_uprobes = false;
  modname_given = false;
  keep_tmpdir = false;
  cmd = "";
  target_pid = 0;
  use_cache = true;
  use_script_cache = true;
  poison_cache = false;
  tapset_compile_coverage = false;
  need_uprobes = false;
  need_unwind = false;
  need_symbols = false;
  need_lines = false;
  uprobes_path = "";
  load_only = false;
  skip_badvars = false;
  privilege = pr_stapdev;
  privilege_set = false;
  compatible = VERSION; // XXX: perhaps also process GIT_SHAID if available?
  unwindsym_ldd = false;
  client_options = false;
  server_cache = NULL;
  auto_privilege_level_msg = "";
  auto_server_msgs.clear ();
  use_server_on_error = false;
  try_server_status = try_server_unset;
  use_remote_prefix = false;
  systemtap_v_check = false;
  download_dbinfo = 0;
  suppress_handler_errors = false;
  native_build = true; // presumed
  sysroot = "";
  update_release_sysroot = false;
  suppress_time_limits = false;
  target_namespaces_pid = 0;
  color_mode = color_auto;
  color_errors = isatty(STDERR_FILENO) // conditions for coloring when
    && strcmp(getenv("TERM") ?: "notdumb", "dumb"); // on auto
  interactive_mode = false;
  pass_1a_complete = false;
  timeout = 0;

  // PR12443: put compiled-in / -I paths in front, to be preferred during 
  // tapset duplicate-file elimination
  const char* s_p = getenv ("SYSTEMTAP_TAPSET");
  if (s_p != NULL)
  {
    include_path.push_back (s_p);
  }
  else
  {
    include_path.push_back (string(PKGDATADIR) + "/tapset");
  }

  /*  adding in the XDG_DATA_DIRS variable path,
   *  this searches in conjunction with SYSTEMTAP_TAPSET
   *  to locate stap scripts, either can be disabled if 
   *  needed using env $PATH=/dev/null where $PATH is the 
   *  path you want disabled
   */  
  const char* s_p1 = getenv ("XDG_DATA_DIRS");
  if ( s_p1 != NULL )
  {
    vector<string> dirs;
    tokenize(s_p1, dirs, ":");
    for(vector<string>::iterator i = dirs.begin(); i != dirs.end(); ++i)
    {
      include_path.push_back(*i + "/systemtap/tapset");
    }
  }

  const char* s_r = getenv ("SYSTEMTAP_RUNTIME");
  if (s_r != NULL)
    runtime_path = s_r;
  else
    runtime_path = string(PKGDATADIR) + "/runtime";

  const char* s_d = getenv ("SYSTEMTAP_DIR");
  if (s_d != NULL)
    data_path = s_d;
  else
    data_path = get_home_directory() + string("/.systemtap");
  if (create_dir(data_path.c_str()) == 1)
    {
      const char* e = strerror (errno);
        print_warning("failed to create systemtap data directory \"" + data_path + "\" " + e + ", disabling cache support.");
      use_cache = use_script_cache = false;
    }

  if (use_cache)
    {
      cache_path = data_path + "/cache";
      if (create_dir(cache_path.c_str()) == 1)
        {
	  const char* e = strerror (errno);
            print_warning("failed to create cache directory (\" " + cache_path + " \") " + e + ", disabling cache support.");
	  use_cache = use_script_cache = false;
	}
    }

  const char* s_tc = getenv ("SYSTEMTAP_COVERAGE");
  if (s_tc != NULL)
    tapset_compile_coverage = true;

  const char* s_kr = getenv ("SYSTEMTAP_RELEASE");
  if (s_kr != NULL) {
    setup_kernel_release(s_kr);
  }
}

systemtap_session::systemtap_session (const systemtap_session& other,
                                      const string& arch,
                                      const string& kern):
  // NB: pointer members must be manually initialized!
  // NB: this needs to consider everything that the base ctor does,
  //     plus copying any wanted implicit fields (strings, vectors, etc.)
  runtime_mode(other.runtime_mode),
  base_hash(0),
  pattern_root(new match_node),
  user_files (other.user_files),
  dfa_counter(0),
  dfa_maxmap(0),
  dfa_maxtag (0),
  need_tagged_dfa(other.need_tagged_dfa),
  be_derived_probes(0),
  generic_kprobe_derived_probes(0),
  hwbkpt_derived_probes(0),
  perf_derived_probes(0),
  uprobe_derived_probes(0),
  utrace_derived_probes(0),
  itrace_derived_probes(0),
  task_finder_derived_probes(0),
  timer_derived_probes(0),
  netfilter_derived_probes(0),
  profile_derived_probes(0),
  mark_derived_probes(0),
  tracepoint_derived_probes(0),
  hrtimer_derived_probes(0),
  procfs_derived_probes(0),
  dynprobe_derived_probes(0),
  python_derived_probes(0),
  op (0), up (0),
  sym_kprobes_text_start (0),
  sym_kprobes_text_end (0),
  sym_stext (0),
  module_cache (0),
  benchmark_sdt_loops(other.benchmark_sdt_loops),
  benchmark_sdt_threads(other.benchmark_sdt_threads),
  suppressed_warnings(0),
  suppressed_errors(0),
  warningerr_count(0),
  target_namespaces_pid(0),
  last_token (0)
{
  release = kernel_release = kern;
  kernel_build_tree = "/lib/modules/" + kernel_release + "/build";
  kernel_extra_cflags = other.kernel_extra_cflags;
  architecture = machine = normalize_machine(arch);
  setup_kernel_release(kern.c_str());
  native_build = false; // assumed; XXX: could be computed as in check_options()

  // These are all copied in the same order as the default ctor did above.

  copy(other.perpass_verbose, other.perpass_verbose + 5, perpass_verbose);
  verbose = other.verbose;

  have_script = other.have_script;
  runtime_specified = other.runtime_specified;
  include_arg_start = other.include_arg_start;
  timing = other.timing;
  guru_mode = other.guru_mode;
  bulk_mode = other.bulk_mode;
  unoptimized = other.unoptimized;
  suppress_warnings = other.suppress_warnings;
  panic_warnings = other.panic_warnings;
  dump_mode = other.dump_mode;
  dump_matched_pattern = other.dump_matched_pattern;

  prologue_searching_mode = other.prologue_searching_mode;

  buffer_size = other.buffer_size;
  last_pass = other.last_pass;
  module_name = other.module_name;
  stapconf_name = other.stapconf_name;
  output_file = other.output_file; // XXX how should multiple remotes work?
  tmpdir_opt_set = false;
  monitor = other.monitor;
  monitor_interval = other.monitor_interval;
  save_module = other.save_module;
  save_uprobes = other.save_uprobes;
  modname_given = other.modname_given;
  keep_tmpdir = other.keep_tmpdir;
  cmd = other.cmd;
  target_pid = other.target_pid; // XXX almost surely nonsense for multiremote
  use_cache = other.use_cache;
  use_script_cache = other.use_script_cache;
  poison_cache = other.poison_cache;
  tapset_compile_coverage = other.tapset_compile_coverage;
  need_uprobes = false;
  need_unwind = false;
  need_symbols = false;
  need_lines = false;
  uprobes_path = "";
  load_only = other.load_only;
  skip_badvars = other.skip_badvars;
  privilege = other.privilege;
  privilege_set = other.privilege_set;
  compatible = other.compatible;
  unwindsym_ldd = other.unwindsym_ldd;
  client_options = other.client_options;
  server_cache = NULL;
  use_server_on_error = other.use_server_on_error;
  try_server_status = other.try_server_status;
  use_remote_prefix = other.use_remote_prefix;
  systemtap_v_check = other.systemtap_v_check;
  download_dbinfo = other.download_dbinfo;
  suppress_handler_errors = other.suppress_handler_errors;
  sysroot = other.sysroot;
  update_release_sysroot = other.update_release_sysroot;
  sysenv = other.sysenv;
  suppress_time_limits = other.suppress_time_limits;
  color_errors = other.color_errors;
  color_mode = other.color_mode;
  interactive_mode = other.interactive_mode;
  pass_1a_complete = other.pass_1a_complete;
  timeout = other.timeout;

  include_path = other.include_path;
  runtime_path = other.runtime_path;

  // NB: assuming that "other" created these already
  data_path = other.data_path;
  cache_path = other.cache_path;

  tapset_compile_coverage = other.tapset_compile_coverage;


  // These are fields that were left to their default ctor, but now we want to
  // copy them from "other".  In the same order as declared...
  script_file = other.script_file;
  cmdline_script = other.cmdline_script;
  additional_scripts = other.additional_scripts;
  c_macros = other.c_macros;
  args = other.args;
  kbuildflags = other.kbuildflags;

  globalopts = other.globalopts;
  modinfos = other.modinfos;

  client_options_disallowed_for_unprivileged = other.client_options_disallowed_for_unprivileged;
  server_status_strings = other.server_status_strings;
  specified_servers = other.specified_servers;
  server_trust_spec = other.server_trust_spec;
  server_args = other.server_args;
  mok_fingerprints = other.mok_fingerprints;

  // HTTP client/server
  http_servers = other.http_servers;

  unwindsym_modules = other.unwindsym_modules;
  auto_privilege_level_msg = other.auto_privilege_level_msg;
  auto_server_msgs = other.auto_server_msgs;

  create_tmp_dir();
}

systemtap_session::~systemtap_session ()
{
  remove_tmp_dir();
  delete_map(subsessions);
  delete pattern_root;
}

const string
systemtap_session::module_filename() const
{
  const char *suffix;
  switch (runtime_mode)
    {
    case kernel_runtime:
      suffix = ".ko";
      break;
    case dyninst_runtime:
      suffix = ".so";
      break;
    case bpf_runtime:
      suffix = ".bo";
      break;
    default:
      abort();
    }
  return module_name + suffix;
}

#if HAVE_NSS
void
systemtap_session::NSPR_init ()
{
  if (! NSPR_Initialized)
    {
      PR_Init (PR_SYSTEM_THREAD, PR_PRIORITY_NORMAL, 1);
      NSPR_Initialized = true;
    }
}
#endif // HAVE_NSS

systemtap_session*
systemtap_session::clone(const string& arch, const string& release)
{
  const string norm_arch = normalize_machine(arch);
  if (this->architecture == norm_arch && this->kernel_release == release)
    return this;

  systemtap_session*& s = subsessions[make_pair(norm_arch, release)];
  if (!s)
    s = new systemtap_session(*this, norm_arch, release);
  return s;
}


string
systemtap_session::version_string ()
{
  string elfutils_version1;
#ifdef _ELFUTILS_VERSION
  elfutils_version1 = "0." + lex_cast(_ELFUTILS_VERSION); /* BUILD */
#endif
  string elfutils_version2 = dwfl_version(NULL); /* RUN */

  if (elfutils_version1 != elfutils_version2)
    elfutils_version2 += string("/") + elfutils_version1;   /* RUN/BUILD */

  return string (VERSION) + "/" + elfutils_version2 + ", " + STAP_EXTENDED_VERSION;
}

void
systemtap_session::version ()
{
  // PRERELEASE
  cout << _F("Systemtap translator/driver (version %s)\n"
             "Copyright (C) 2005-2017 Red Hat, Inc. and others\n"
             "This is free software; see the source for copying conditions.\n",
             version_string().c_str());
  cout << _F("tested kernel versions: %s ... %s\n", "2.6.18", "4.11");
  
  cout << _("enabled features:")
#ifdef HAVE_AVAHI
       << " AVAHI"
#endif
#ifdef HAVE_BOOST_UTILITY_STRING_REF_HPP
       << " BOOST_STRING_REF"
#endif
#ifdef HAVE_DYNINST
       << " DYNINST"
#endif
#ifdef HAVE_JAVA
       << " JAVA"
#endif
#ifdef HAVE_PYTHON2_PROBES
       << " PYTHON2"
#endif
#ifdef HAVE_PYTHON3_PROBES
       << " PYTHON3"
#endif
#ifdef HAVE_LIBRPM
       << " LIBRPM"
#endif
#ifdef HAVE_LIBSQLITE3
       << " LIBSQLITE3"
#endif
#ifdef HAVE_LIBVIRT
       << " LIBVIRT"
#endif
#ifdef HAVE_LIBXML2
       << " LIBXML2"
#endif
#ifdef ENABLE_NLS
       << " NLS"
#endif
#ifdef HAVE_NSS
       << " NSS"
#endif
#ifdef ENABLE_PROLOGUES
       << " PROLOGUES"
#endif
#ifdef HAVE_LIBREADLINE
       << " READLINE"
#endif
       << endl;
}

void
systemtap_session::usage (int exitcode)
{
  // For error cases, just suggest --help, so we don't obscure
  // the actual error message with all the help text.
  if (exitcode != EXIT_SUCCESS)
    {
      clog << _("Try '--help' for more information.") << endl;
      throw exit_exception(exitcode);
    }

  version ();
  cout
    << endl
    << _F(
     "Usage: stap [options] FILE                    Run script in file.\n"
     "   or: stap [options] -                       Run script on stdin.\n"
     "   or: stap [options] -e SCRIPT               Run given script.\n"
     "   or: stap [options] -l PROBE                List matching probes.\n"
     "   or: stap [options] -L PROBE                List matching probes and local variables.\n"
     "   or: stap [options] --dump-probe-types      List available probe types.\n"
     "   or: stap [options] --dump-probe-aliases    List available probe aliases.\n"
     "   or: stap [options] --dump-functions        List available functions.\n\n"
     "Options (in %s/rc and on command line):\n"
     "   --         end of translator options, script options follow\n"
     "   -h --help  show help\n"
     "   -V --version\n"
     "              show version\n"
     "   -p NUM     stop after pass NUM 1-5, instead of %d\n"
     "              (parse, elaborate, translate, compile, run)\n"
     "   -v         add verbosity to all passes\n"
     "   --vp {N}+  add per-pass verbosity [", data_path.c_str(), last_pass);
  for (unsigned i=0; i<5; i++)
    cout << (perpass_verbose[i] <= 9 ? perpass_verbose[i] : 9);
  cout 
    << "]" << endl;
    cout << _F("   -k         keep temporary directory\n"
     "   -u         unoptimized translation %s\n"
     "   -w         suppress warnings %s\n"
     "   -W         turn warnings into errors %s\n"
     "   -g         guru mode %s\n"
     "   -P         prologue-searching for function probes %s\n"
     "   -b         bulk (percpu file) mode %s\n"
#ifdef HAVE_LIBREADLINE
     "   -i --interactive\n"
     "              interactive mode %s\n"
#endif
     "   -s NUM     buffer size in megabytes, instead of %d\n"
     "   -I DIR     look in DIR for additional .stp script files", (unoptimized ? _(" [set]") : ""),
         (suppress_warnings ? _(" [set]") : ""), (panic_warnings ? _(" [set]") : ""),
         (guru_mode ? _(" [set]") : ""), (prologue_searching_mode == prologue_searching_always ? _(" [set]") : ""),
         (bulk_mode ? _(" [set]") : ""),
#ifdef HAVE_LIBREADLINE
         (interactive_mode ? _(" [set]") : ""),
#endif
	 buffer_size);
  if (include_path.size() == 0)
    cout << endl;
  else
    cout << _(", in addition to") << endl;
  for (unsigned i=0; i<include_path.size(); i++)
    cout << "              " << include_path[i].c_str() << endl;
  cout
    << _F("   -D NM=VAL  emit macro definition into generated C code\n"
    "   -B NM=VAL  pass option to kbuild make\n"
    "   --modinfo NM=VAL\n"
    "              include a MODULE_INFO(NM,VAL) in the generated C code\n"
    "   -G VAR=VAL set global variable to value\n"
    //TRANSLATORS: translating 'runtime' is not advised 
    "   -R DIR     look in DIR for runtime, instead of\n"
    "              %s\n"
    "   -r DIR     cross-compile to kernel with given build tree; or else\n"
    "   -r RELEASE cross-compile to kernel /lib/modules/RELEASE/build, instead of\n"
    "              %s\n" 
    "   -a ARCH    cross-compile to given architecture, instead of %s\n"
    "   -m MODULE  set probe module name, instead of \n"
    "              %s\n"
    "   -o FILE    send script output to file, instead of stdout. This supports\n" 
    "              strftime(3) formats for FILE\n"
    "   -E SCRIPT  run the SCRIPT in addition to the main script specified\n"
    "              through -e or a script file\n"
    "   -c CMD     start the probes, run CMD, and exit when it finishes\n"
    "   -x PID     sets target() to PID\n"
    "   -F         run as on-file flight recorder with -o.\n"
    "              run as on-memory flight recorder without -o.\n"
    "   -S size[,n] set maximum of the size and the number of files.\n"
    "   -d OBJECT  add unwind/symbol data for OBJECT file", runtime_path.c_str(), kernel_build_tree.c_str(), architecture.c_str(), module_name.c_str());
  if (unwindsym_modules.size() == 0)
    cout << endl;
  else
    cout << _(", in addition to") << endl;
  {
    vector<string> syms (unwindsym_modules.begin(), unwindsym_modules.end());
    for (unsigned i=0; i<syms.size(); i++)
      cout << "              " << syms[i].c_str() << endl;
  }
  cout
    << _F("   --ldd      add unwind/symbol data for referenced user-space objects.\n"
    "   --all-modules\n"
    "              add unwind/symbol data for all loaded kernel objects.\n"
    "   -t         collect probe timing information\n"
    "   -T TIME    terminate the script after TIME seconds\n"
#ifdef HAVE_LIBSQLITE3
    "   -q         generate information on tapset coverage\n"
#endif /* HAVE_LIBSQLITE3 */
    "   --runtime=MODE\n"
    "              set the pass-5 runtime mode, instead of kernel\n"
#ifdef HAVE_DYNINST
    "   --dyninst\n"
    "              shorthand for --runtime=dyninst\n"
#endif /* HAVE_DYNINST */
    "   --prologue-searching[=WHEN]\n"
    "              prologue-searching for function probes\n"
    "   --privilege=PRIVILEGE_LEVEL\n"
    "              check the script for constructs not allowed at the given privilege level\n"
    "   --unprivileged\n"
    "              equivalent to --privilege=stapusr\n"
    "   --compatible=VERSION\n"
    "              suppress incompatible language/tapset changes beyond VERSION,\n"
    "              instead of %s\n"
    "   --check-version\n"
    "              displays warnings where a syntax element may be \n"
    "              version dependent\n"
    "   --skip-badvars\n"
    "              substitute zero for bad context $variables\n"
    "   --suppress-handler-errors\n"
    "              catch all runtime errors, quietly skip probe handlers\n"
    "   --use-server[=SERVER-SPEC]\n"
    "              specify systemtap compile-servers\n"
    "   --list-servers[=PROPERTIES]\n"
    "              report on the status of the specified compile-servers:\n"
    "              all,specified,online,trusted,signer,compatible\n"
#if HAVE_NSS
    "   --trust-servers[=TRUST-SPEC]\n"
    "              add/revoke trust of specified compile-servers:\n"
    "              ssl,signer,all-users,revoke,no-prompt\n"
    "   --use-server-on-error[=yes/no]\n"
    "              retry compilation using a compile server upon compilation error\n"
#endif
#ifdef HAVE_HTTP_SUPPORT
    "   --use-http-server=SERVER-SPEC\n"
    "              specify systemtap http compile server\n"
#endif
    "   --remote=HOSTNAME\n"
    "              run pass 5 on the specified ssh host.\n"
    "              may be repeated for targeting multiple hosts.\n"
    "   --remote-prefix\n"
    "              prefix each line of remote output with a host index.\n"
    "   --tmpdir=NAME\n"
    "              specify name of temporary directory to be used.\n"
    "   --download-debuginfo[=OPTION]\n"
    "              automatically download debuginfo using ABRT.\n"
    "              yes,no,ask,<timeout value>\n"
    "   --dump-probe-types\n"
    "              show a list of available probe types.\n"
    "   --sysroot=DIR\n"
    "              specify sysroot directory where target files (executables,\n"    "              libraries, etc.) are located.\n"
    "   --sysenv=VAR=VALUE\n"
    "              provide an alternate value for an environment variable\n"
    "              where the value on a remote system differs.  Path\n"
    "              variables (e.g. PATH, LD_LIBRARY_PATH) are assumed to be\n"
    "              relative to the sysroot.\n"
    "   --suppress-time-limits\n"
    "              disable -DSTP_OVERLOAD, -DMAXACTION, and -DMAXTRYACTION limits\n"
    "   --save-uprobes\n"
    "              save uprobes.ko to current directory if it is built from source\n"
    "   --target-namesapce=PID\n"
    "              sets the target namespaces pid to PID\n"
#if HAVE_MONITOR_LIBS
    "   --monitor=INTERVAL\n"
    "              enables monitor interfaces\n"
#endif
    , compatible.c_str()) << endl
  ;

  time_t now;
  time (& now);
  struct tm* t = localtime (& now);
  if (t && t->tm_mon*3 + t->tm_mday*173 == 0xb6)
    cout << morehelp << endl;

  throw exit_exception (exitcode);
}

int
systemtap_session::parse_cmdline (int argc, char * const argv [])
{
  client_options_disallowed_for_unprivileged = "";
  std::set<std::string> additional_unwindsym_modules;
  struct rlimit our_rlimit;
  while (true)
    {
      char * num_endptr;
      int grc = getopt_long (argc, argv, STAP_SHORT_OPTIONS, stap_long_options, NULL);

      // NB: when adding new options, consider very carefully whether they
      // should be restricted from stap clients (after --client-options)!

      if (grc < 0)
        break;
      switch (grc)
        {
        case 'V':
          version ();
          throw exit_exception (EXIT_SUCCESS);

        case 'v':
	  server_args.push_back (string ("-") + (char)grc);
          for (unsigned i=0; i<5; i++)
            perpass_verbose[i] ++;
	  verbose ++;
	  break;

        case 'G':
          // Make sure the global option is only composed of the
          // following chars: [_=a-zA-Z0-9.-]
          assert(optarg);
          assert_regexp_match("-G parameter", optarg, "^[a-z_][a-z0-9_]*=[a-z0-9_.-]+$");
          globalopts.push_back (string(optarg));
          break;

        case 't':
	  server_args.push_back (string ("-") + (char)grc);
	  timing = true;
	  break;

        case 'w':
	  server_args.push_back (string ("-") + (char)grc);
	  suppress_warnings = true;
	  break;

        case 'W':
	  server_args.push_back (string ("-") + (char)grc);
	  panic_warnings = true;
	  break;

        case 'p':
          assert(optarg);
          last_pass = (int)strtoul(optarg, &num_endptr, 10);
          if (*num_endptr != '\0' || last_pass < 1 || last_pass > 5)
            {
              cerr << _("Invalid pass number (should be 1-5).") << endl;
              return 1;
            }
	  server_args.push_back (string ("-") + (char)grc + optarg);
          break;

        case 'I':
          assert(optarg);
	  if (client_options)
	    client_options_disallowed_for_unprivileged += client_options_disallowed_for_unprivileged.empty () ? "-I" : ", -I";
	  if (include_arg_start == -1)
	    include_arg_start = include_path.size ();
          include_path.push_back (string (optarg));
          break;

        case 'd':
          assert(optarg);
	  server_args.push_back (string ("-") + (char)grc + optarg);
          {
            // Make sure an empty data object wasn't specified (-d "")
            if (strlen (optarg) == 0)
            {
              cerr << _("Data object (-d) cannot be empty.") << endl;
              return 1;
            }
            // At runtime user module names are resolved through their
            // canonical (absolute) path, or else it's a kernel module name.
	    // The sysroot option might change the path to a user module.
            additional_unwindsym_modules.insert (resolve_path (optarg));
            // NB: we used to enable_vma_tracker() here for PR10228, but now
            // we'll leave that to pragma:vma functions which actually use it.
            break;
          }

        case 'e':
          assert(optarg);
	  if (have_script)
           {
             cerr << _("Only one script can be given on the command line.")
                  << endl;
              return 1;
           }
	  server_args.push_back (string ("-") + (char)grc + optarg);
          cmdline_script = string (optarg);
          have_script = true;
          break;

        case 'E':
          assert(optarg);
          server_args.push_back (string("-") + (char)grc + optarg);
          additional_scripts.push_back(string (optarg));
          // don't set have_script to true, since this script is meant to be
          // given in addition to a script/script_file.
          break;

        case 'o':
          assert(optarg);
          // NB: client_options not a problem, since pass 1-4 does not use output_file.
	  server_args.push_back (string ("-") + (char)grc + optarg);
          output_file = string (optarg);
          break;

        case 'R':
          assert(optarg);
          if (client_options) { cerr << _F("ERROR: %s invalid with %s", "-R", "--client-options") << endl; return 1; }
	  runtime_specified = true;
          runtime_path = string (optarg);
          break;

        case 'm':
          assert(optarg);
	  if (client_options)
	    client_options_disallowed_for_unprivileged += client_options_disallowed_for_unprivileged.empty () ? "-m" : ", -m";
          module_name = string (optarg);
	  save_module = true;
	  modname_given = true;
	  {
	    // If the module name ends with '.ko', chop it off since
	    // modutils doesn't like modules named 'foo.ko.ko'.
	    if (endswith(module_name, ".ko") || endswith(module_name, ".so"))
	      {
		module_name.erase(module_name.size() - 3);
		cerr << _F("Truncating module name to '%s'", module_name.c_str()) << endl;
	      }

	    // Make sure an empty module name wasn't specified (-m "")
	    if (module_name.empty())
	    {
		cerr << _("Module name cannot be empty.") << endl;
		return 1;
	    }

	    // Make sure the module name is only composed of the
	    // following chars: [a-z0-9_]
            assert_regexp_match("-m parameter", module_name, "^[a-z0-9_]+$");

	    // Make sure module name isn't too long.
	    if (module_name.size() >= (MODULE_NAME_LEN - 1))
	      {
		module_name.resize(MODULE_NAME_LEN - 1);
		cerr << _F("Truncating module name to '%s'", module_name.c_str()) << endl;
	      }
	  }

	  server_args.push_back (string ("-") + (char)grc + optarg);
	  use_script_cache = false;
          break;

        case 'r':
          assert(optarg);
          if (client_options) // NB: no paths!
	    // Note that '-' must come last in a regex bracket expression.
            assert_regexp_match("-r parameter from client", optarg, "^[a-z0-9_.+-]+$");
	  server_args.push_back (string ("-") + (char)grc + optarg);
          setup_kernel_release(optarg);
          break;

        case 'a':
          assert(optarg);
          assert_regexp_match("-a parameter", optarg, "^[a-z0-9_-]+$");
	  server_args.push_back (string ("-") + (char)grc + optarg);
          architecture = string(optarg);
          break;

        case 'k':
          if (client_options) { cerr << _F("ERROR: %s invalid with %s", "-k", "--client-options") << endl; return 1; } 
          keep_tmpdir = true;
          use_script_cache = false; /* User wants to keep a usable build tree. */
          break;

        case 'g':
	  server_args.push_back (string ("-") + (char)grc);
          guru_mode = true;
          break;

	case 'i':
	case LONG_OPT_INTERACTIVE:
#ifdef HAVE_LIBREADLINE
          if (client_options) { cerr << _F("ERROR: %s invalid with %s", "-i", "--client-options") << endl; return 1; } 
	  interactive_mode = true;
#endif
	  break;

        case 'P':
	  server_args.push_back (string ("-") + (char)grc);
          prologue_searching_mode = prologue_searching_always;
          break;

        case 'b':
	  server_args.push_back (string ("-") + (char)grc);
          bulk_mode = true;
          break;

	case 'u':
	  server_args.push_back (string ("-") + (char)grc);
	  unoptimized = true;
	  break;

        case 's':
          assert(optarg);
          buffer_size = (int) strtoul (optarg, &num_endptr, 10);
          if (*num_endptr != '\0' || buffer_size < 1 || buffer_size > 4095)
            {
              cerr << _("Invalid buffer size (should be 1-4095).") << endl;
	      return 1;
            }
	  server_args.push_back (string ("-") + (char)grc + optarg);
          break;

	case 'c':
          assert(optarg);
	  cmd = string (optarg);
          if (cmd == "")
            {
              // This would mess with later code deciding to pass -c
              // through to staprun
              cerr << _("Empty CMD string invalid.") << endl;
              return 1;
            }
	  server_args.push_back (string ("-") + (char)grc + optarg);
	  break;

	case 'x':
          assert(optarg);
	  target_pid = (int) strtoul(optarg, &num_endptr, 10);
	  if (*num_endptr != '\0')
	    {
	      cerr << _("Invalid target process ID number.") << endl;
	      return 1;
	    }
	  server_args.push_back (string ("-") + (char)grc + optarg);
	  break;

	case 'D':
          assert(optarg);
          assert_regexp_match ("-D parameter", optarg, "^[a-z_][a-z_0-9]*(=-?[a-z_0-9]+)?$");
	  if (client_options)
	    client_options_disallowed_for_unprivileged += client_options_disallowed_for_unprivileged.empty () ? "-D" : ", -D";
	  server_args.push_back (string ("-") + (char)grc + optarg);
	  c_macros.push_back (string (optarg));
	  break;

	case 'S':
          assert(optarg);
          assert_regexp_match ("-S parameter", optarg, "^[0-9]+(,[0-9]+)?$");
	  server_args.push_back (string ("-") + (char)grc + optarg);
	  size_option = string (optarg);
	  break;

        case 'T':
          assert(optarg);
          timeout = (int) 1000*strtod(optarg, &num_endptr); // convert to ms
          if (*num_endptr != '\0' || timeout < 1)
            {
              cerr << _("Invalid timeout interval.") << endl;
              return 1;
            }
          break;

	case 'q':
          if (client_options) { cerr << _F("ERROR: %s invalid with %s", "-q", "--client-options") << endl; return 1; } 
	  server_args.push_back (string ("-") + (char)grc);
	  tapset_compile_coverage = true;
	  break;

        case 'h':
          usage (0);
          break;

        case 'L':
          if (dump_mode)
            {
              cerr << _("ERROR: only one of the -l/-L/--dump-* "
                        "switches may be specified") << endl;
              return 1;
            }
          assert(optarg);
          server_args.push_back (string ("-") + (char)grc + optarg);
          dump_mode = systemtap_session::dump_matched_probes_vars;
          dump_matched_pattern = optarg;
          unoptimized = true; // This causes retention of vars for listing
          suppress_warnings = true;
          break;

        case 'l':
          if (dump_mode)
            {
              cerr << _("ERROR: only one of the -l/-L/--dump-* "
                        "switches may be specified") << endl;
              return 1;
            }
          assert(optarg);
          server_args.push_back (string ("-") + (char)grc + optarg);
          dump_mode = systemtap_session::dump_matched_probes;
          dump_matched_pattern = optarg;
          suppress_warnings = true;
          break;

        case 'F':
	  server_args.push_back (string ("-") + (char)grc);
          load_only = true;
	  break;

	case 'B':
          if (client_options) { cerr << _F("ERROR: %s invalid with %s", "-B", "--client-options") << endl; return 1; } 
          assert(optarg);
	  server_args.push_back (string ("-") + (char)grc + optarg);
          kbuildflags.push_back (string (optarg));
	  break;

	case LONG_OPT_VERSION:
	  version ();
	  throw exit_exception (EXIT_SUCCESS);

	case LONG_OPT_VERBOSE_PASS:
	  {
            assert(optarg);
	    bool ok = true;
	    if (strlen(optarg) < 1 || strlen(optarg) > 5)
	      ok = false;
	    if (ok)
	      {
		for (unsigned i=0; i<strlen(optarg); i++)
		  if (isdigit (optarg[i]))
		    perpass_verbose[i] += optarg[i]-'0';
		  else
		    ok = false;
	      }
	    if (! ok)
	      {
		cerr << _("Invalid --vp argument: it takes 1 to 5 digits.") << endl;
		return 1;
	      }
	    // NB: we don't do this: last_pass = strlen(optarg);
	    server_args.push_back ("--vp=" + string(optarg));
	    break;
	  }

	case LONG_OPT_SKIP_BADVARS:
	  server_args.push_back ("--skip-badvars");
	  skip_badvars = true;
	  break;

	case LONG_OPT_PRIVILEGE:
	  {
	    // We allow only multiple privilege-setting options if they all specify the same
	    // privilege level. The server also expects and depends on this behaviour when
	    // examining the client-side options passed to it.
	    privilege_t newPrivilege;
	    assert(optarg);
	    if (strcmp (optarg, "stapdev") == 0)
	      newPrivilege = pr_stapdev;
	    else if (strcmp (optarg, "stapsys") == 0)
	      newPrivilege = pr_stapsys;
	    else if (strcmp (optarg, "stapusr") == 0)
	      newPrivilege = pr_stapusr;
	    else
	      {
		cerr << _F("Invalid argument '%s' for --privilege.", optarg) << endl;
		return 1;
	      }
	    if (privilege_set && newPrivilege != privilege)
	      {
		cerr << _("Privilege level may be set only once.") << endl;
		return 1;
	      }
	    privilege = newPrivilege;
	    privilege_set = true;
	    server_args.push_back ("--privilege=" + string(optarg));
	  }
	  /* NB: for server security, it is essential that once this flag is
	     set, no future flag be able to unset it. */
	  break;

	case LONG_OPT_UNPRIVILEGED:
	  // We allow only multiple privilege-setting options if they all specify the same
	  // privilege level. The server also expects and depends on this behaviour when
	  // examining the client-side options passed to it.
	  if (privilege_set && pr_unprivileged != privilege)
	    {
	      cerr << _("Privilege level may be set only once.") << endl;
	      return 1;
	    }
	  privilege = pr_unprivileged;
	  privilege_set = true;
	  server_args.push_back ("--unprivileged");
	  /* NB: for server security, it is essential that once this flag is
	     set, no future flag be able to unset it. */
	  break;

	case LONG_OPT_CLIENT_OPTIONS:
	  client_options = true;
	  break;

	case LONG_OPT_TMPDIR:
	  if (client_options) {
	    cerr << _F("ERROR: %s is invalid with %s", "--tmpdir", "--client-options") << endl;
	    return 1;
	  }
	  tmpdir_opt_set = true;
	  tmpdir = optarg;
	  break;

	case LONG_OPT_DOWNLOAD_DEBUGINFO:
	  if(optarg)
	    {
	      if(strcmp(optarg, "no") == 0)
		download_dbinfo = 0; //Disable feature
	      else if (strcmp(optarg, "yes") == 0)
		download_dbinfo = INT_MAX; //Enable, No Timeout
	      /* NOTE: Timeout and Asking for Confirmation features below are not supported yet by abrt
	       * in version abrt-2.0.3-1.fc15.x86_64, Bugzilla: BZ730107 (timeout), BZ726192 ('-y') */
	      else if(atoi(optarg) > 0)
		download_dbinfo = atoi(optarg); //Enable, Set timeout to optarg
	      else if (strcmp(optarg, "ask") == 0)
		download_dbinfo = -1; //Enable, Ask for confirmation
	      else
		{
		  cerr << _F("ERROR: %s is not a valid value. Use 'yes', 'no', 'ask' or a timeout value.", optarg) << endl;
		  return 1;
		}
	    }
	  else
	    download_dbinfo = INT_MAX; //Enable, No Timeout
	  break;

	case LONG_OPT_USE_SERVER:
	  if (client_options) {
	    cerr << _F("ERROR: %s is invalid with %s", "--use-server", "--client-options") << endl;
	    return 1;
	  }
	  if (optarg)
	    specified_servers.push_back (optarg);
	  else
	    specified_servers.push_back ("");
	  break;

	case LONG_OPT_USE_SERVER_ON_ERROR:
	  if (client_options) {
	    cerr << _F("ERROR: %s is invalid with %s", "--use-server-on-error", "--client-options") << endl;
	    return 1;
	  }
	  if (optarg)
	    {
	      string arg = optarg;
	      for (unsigned i = 0; i < arg.size (); ++i)
		arg[i] = tolower (arg[i]);
	      if (arg == "yes" || arg == "ye" || arg == "y")
		use_server_on_error = true;
	      else if (arg == "no" || arg == "n")
		use_server_on_error = false;
	      else
		cerr << _F("Invalid argument '%s' for --use-server-on-error.", optarg) << endl;
	    }
	  else
	    use_server_on_error = true;
	  break;

	case LONG_OPT_LIST_SERVERS:
	  if (client_options) {
	    cerr << _F("ERROR: %s is invalid with %s", "--list-servers", "--client-options") << endl;
	    return 1;
	  }
	  if (optarg)
	    server_status_strings.push_back (optarg);
	  else
	    server_status_strings.push_back ("");
	  break;

	case LONG_OPT_TRUST_SERVERS:
	  if (client_options) {
	    cerr << _F("ERROR: %s is invalid with %s", "--trust-servers", "--client-options") << endl;
	    return 1;
	  }
	  if (optarg)
	    server_trust_spec = optarg;
	  else
	    server_trust_spec = "ssl";
	  break;


#ifdef HAVE_HTTP_SUPPORT
	case LONG_OPT_USE_HTTP_SERVER:
	  if (client_options) {
	    cerr << _F("ERROR: %s is invalid with %s", "--use-http-server", "--client-options") << endl;
	    return 1;
	  }
	  http_servers.push_back (optarg);
	  break;
#endif

	case LONG_OPT_HELP:
	  usage (0);
	  break;

	  // The caching options should not be available to server clients
	case LONG_OPT_DISABLE_CACHE:
	  if (client_options) {
	    cerr << _F("ERROR: %s is invalid with %s", "--disable-cache", "--client-options") << endl;
	    return 1;
	  }
	  use_cache = use_script_cache = false;
	  break;

	case LONG_OPT_POISON_CACHE:
	  if (client_options) {
	    cerr << _F("ERROR: %s is invalid with %s", "--poison-cache", "--client-options") << endl;
	    return 1;
	  }
	  poison_cache = true;
	  break;

	case LONG_OPT_CLEAN_CACHE:
	  if (client_options) {
	    cerr << _F("ERROR: %s is invalid with %s", "--clean-cache", "--client-options") << endl;
	    return 1;
	  }
	  clean_cache(*this);
	  throw exit_exception(EXIT_SUCCESS);

	case LONG_OPT_COMPATIBLE:
          assert(optarg);
	  server_args.push_back ("--compatible=" + string(optarg));
          if (strverscmp(optarg, VERSION) > 0) {
            cerr << _F("ERROR: systemtap version %s cannot be compatible with future version %s", VERSION, optarg)
                 << endl;
            return 1;
          }
	  compatible = optarg;
	  break;

	case LONG_OPT_LDD:
	  if (client_options) {
	    cerr << _F("ERROR: %s is invalid with %s", "--ldd", "--client-options") << endl;
	    return 1;
	  }
	  unwindsym_ldd = true;
	  break;

	case LONG_OPT_ALL_MODULES:
	  if (client_options) {
	    cerr << _F("ERROR: %s is invalid with %s", "--all-modules", "--client-options") << endl;
	    return 1;
	  }
	  insert_loaded_modules();
	  break;

	case LONG_OPT_REMOTE:
	  if (client_options) {
	    cerr << _F("ERROR: %s is invalid with %s", "--remote", "--client-options") << endl;
	    return 1;
	  }

	  remote_uris.push_back(optarg);
	  break;

	case LONG_OPT_REMOTE_PREFIX:
	  if (client_options) {
	    cerr << _F("ERROR: %s is invalid with %s", "--remote-prefix", "--client-options") << endl;
	    return 1;
	  }

	  use_remote_prefix = true;
	  break;

	case LONG_OPT_CHECK_VERSION:
	  server_args.push_back ("--check-version");
	  systemtap_v_check = true;
	  break;

        case LONG_OPT_DUMP_PROBE_TYPES:
          if (dump_mode)
            {
              cerr << _("ERROR: only one of the -l/-L/--dump-* "
                        "switches may be specified") << endl;
              return 1;
            }
          server_args.push_back ("--dump-probe-types");
          dump_mode = systemtap_session::dump_probe_types;
          break;

        case LONG_OPT_DUMP_PROBE_ALIASES:
          if (dump_mode)
            {
              cerr << _("ERROR: only one of the -l/-L/--dump-* "
                        "switches may be specified") << endl;
              return 1;
            }
          server_args.push_back ("--dump-probe-aliases");
          suppress_warnings = true;
          dump_mode = systemtap_session::dump_probe_aliases;
          break;

        case LONG_OPT_DUMP_FUNCTIONS:
          if (dump_mode)
            {
              cerr << _("ERROR: only one of the -l/-L/--dump-* "
                        "switches may be specified") << endl;
              return 1;
            }
          server_args.push_back ("--dump-functions");
          suppress_warnings = true;
          dump_mode = systemtap_session::dump_functions;
          unoptimized = true; // Keep unused functions (which is all of them)
          break;

	case LONG_OPT_SUPPRESS_HANDLER_ERRORS:
	  suppress_handler_errors = true;
          c_macros.push_back (string ("STAP_SUPPRESS_HANDLER_ERRORS"));
	  break;

	case LONG_OPT_MODINFO:
	  // Make sure the global option is only composed of the
	  // following chars: [_=a-zA-Z0-9]
	  if (client_options) {
	    cerr << _F("ERROR: %s is invalid with %s", "--modinfo", "--client-options") << endl;
	    return 1;
	  }
          assert(optarg);
	  assert_regexp_match("--modinfo parameter", optarg, "^[a-z_][a-z0-9_]*=.+$");
	  modinfos.push_back (string(optarg));
	  break;

	case LONG_OPT_RLIMIT_AS:
          assert(optarg);
	  if(getrlimit(RLIMIT_AS, & our_rlimit))
	    cerr << _F("Unable to obtain resource limits for rlimit-as : %s", strerror (errno)) << endl;
	  if (strlen(optarg) == 0) {
	    cerr << _("An --rlimit-as option value must be specified.") << endl;
	    return 1;
	  }
	  our_rlimit.rlim_max = our_rlimit.rlim_cur = strtoul (optarg, &num_endptr, 0);
	  if(*num_endptr) {
	    cerr << _F("Unable to convert rlimit-as resource limit '%s'.", optarg) << endl;
	    return 1;
	  }
	  if(setrlimit (RLIMIT_AS, & our_rlimit)) {
	    int saved_errno = errno;
	    cerr << _F("Unable to set resource limits for rlimit-as : %s", strerror (errno)) << endl;
	    if (saved_errno != EPERM)
	      return 1;
	  }

          /* Disable core dumps, since exhaustion results in uncaught bad_alloc etc. exceptions */
	  our_rlimit.rlim_max = our_rlimit.rlim_cur = 0;
	  (void) setrlimit (RLIMIT_CORE, & our_rlimit);
	  break;

	case LONG_OPT_RLIMIT_CPU:
          assert(optarg);
	  if(getrlimit(RLIMIT_CPU, & our_rlimit))
	    cerr << _F("Unable to obtain resource limits for rlimit-cpu : %s", strerror (errno)) << endl;
	  if (strlen(optarg) == 0) {
	    cerr << _("An --rlimit-cpu option value must be specified.") << endl;
	    return 1;
	  }
	  our_rlimit.rlim_max = our_rlimit.rlim_cur = strtoul (optarg, &num_endptr, 0);
	  if(*num_endptr) {
	    cerr << _F("Unable to convert resource limit '%s' for rlimit-cpu", optarg) << endl;
	    return 1;
	  }
	  if(setrlimit (RLIMIT_CPU, & our_rlimit)) {
	    int saved_errno = errno;
	    cerr << _F("Unable to set resource limits for rlimit-cpu : %s", strerror (errno)) << endl;
	    if (saved_errno != EPERM)
	      return 1;
	  }
	  break;

	case LONG_OPT_RLIMIT_NPROC:
          assert(optarg);
	  if(getrlimit(RLIMIT_NPROC, & our_rlimit))
	    cerr << _F("Unable to obtain resource limits for rlimit-nproc : %s", strerror (errno)) << endl;
	  if (strlen(optarg) == 0) {
	    cerr << _("An --rlimit-nproc option value must be specified.") << endl;
	    return 1;
	  }
	  our_rlimit.rlim_max = our_rlimit.rlim_cur = strtoul (optarg, &num_endptr, 0);
	  if(*num_endptr) {
	    cerr << _F("Unable to convert resource limit '%s' for rlimit-nproc", optarg) << endl;
	    return 1;
	  }
	  if(setrlimit (RLIMIT_NPROC, & our_rlimit)) {
	    int saved_errno = errno;
	    cerr << _F("Unable to set resource limits for rlimit-nproc : %s", strerror (errno)) << endl;
	    if (saved_errno != EPERM)
	      return 1;
	  }
	  break;

	case LONG_OPT_RLIMIT_STACK:
          assert(optarg);
	  if(getrlimit(RLIMIT_STACK, & our_rlimit))
	    cerr << _F("Unable to obtain resource limits for rlimit-stack : %s", strerror (errno)) << endl;
	  if (strlen(optarg) == 0) {
	    cerr << _("An --rlimit-stack option value must be specified.") << endl;
	    return 1;
	  }
	  our_rlimit.rlim_max = our_rlimit.rlim_cur = strtoul (optarg, &num_endptr, 0);
	  if(*num_endptr) {
	    cerr << _F("Unable to convert resource limit '%s' for rlimit-stack", optarg) << endl;
	    return 1;
	  }
	  if(setrlimit (RLIMIT_STACK, & our_rlimit)) {
	    int saved_errno = errno;
	    cerr << _F("Unable to set resource limits for rlimit-stack : %s", strerror (errno)) << endl;
	    if (saved_errno != EPERM)
	      return 1;
	  }

          /* Disable core dumps, since exhaustion results in SIGSEGV */
	  our_rlimit.rlim_max = our_rlimit.rlim_cur = 0;
	  (void) setrlimit (RLIMIT_CORE, & our_rlimit);
	  break;

	case LONG_OPT_RLIMIT_FSIZE:
          assert(optarg);
	  if(getrlimit(RLIMIT_FSIZE, & our_rlimit))
	    cerr << _F("Unable to obtain resource limits for rlimit-fsize : %s", strerror (errno)) << endl;
	  if (strlen(optarg) == 0) {
	    cerr << _("An --rlimit-fsize option value must be specified.") << endl;
	    return 1;
	  }
	  our_rlimit.rlim_max = our_rlimit.rlim_cur = strtoul (optarg, &num_endptr, 0);
	  if(*num_endptr) {
	    cerr << _F("Unable to convert resource limit '%s' for rlimit-fsize", optarg) << endl;
	    return 1;
	  }
	  if(setrlimit (RLIMIT_FSIZE, & our_rlimit)) {
	    int saved_errno = errno;
	    cerr << _F("Unable to set resource limits for rlimit-fsize : %s", strerror (errno)) << endl;
	    if (saved_errno != EPERM)
	      return 1;
	  }
	  break;

	case LONG_OPT_SYSROOT:
	  if (client_options) {
	      cerr << _F("ERROR: %s invalid with %s", "--sysroot", "--client-options") << endl;
	      return 1;
	  } else if (!sysroot.empty()) {
	      cerr << "ERROR: multiple --sysroot options not supported" << endl;
	      return 1;
	  } else {
	      char *spath;
	      assert(optarg);
	      spath = canonicalize_file_name (optarg);
	      if (spath == NULL) {
		  cerr << _F("ERROR: %s is an invalid directory for --sysroot", optarg) << endl;
		  return 1;
	      }

	      sysroot = string(spath);
	      free (spath);
	      if (sysroot[sysroot.size() - 1] != '/')
		  sysroot.append("/");

	      break;
	  }

	case LONG_OPT_SYSENV:
	  if (client_options) {
	      cerr << _F("ERROR: %s invalid with %s", "--sysenv", "--client-options") << endl;
	      return 1;
	  } else {
	      string sysenv_str = optarg;
	      string value;
	      size_t pos;
	      if (sysroot.empty()) {
		  cerr << "ERROR: --sysenv must follow --sysroot" << endl;
		  return 1;
	      }

	      pos = sysenv_str.find("=");
	      if (pos == string::npos) {
		  cerr << _F("ERROR: %s is an invalid argument for --sysenv", optarg) << endl;
		  return 1;
	      }

	      value = sysenv_str.substr(pos + 1);
	      sysenv[sysenv_str.substr(0, pos)] = value;

	      break;
	  }

	case LONG_OPT_SUPPRESS_TIME_LIMITS: //require guru_mode to use
	  if (guru_mode == false)
	    {
	      cerr << _F("ERROR %s requires guru mode (-g)", "--suppress-time-limits") << endl;
	      return 1;
	    }
	  else
	    {
	      suppress_time_limits = true;
	      server_args.push_back (string ("--suppress-time-limits"));
	      c_macros.push_back (string ("STAP_SUPPRESS_TIME_LIMITS_ENABLE"));
	      break;
	    }

	case LONG_OPT_RUNTIME:
          if (!parse_cmdline_runtime (optarg))
            return 1;
          break;

	case LONG_OPT_RUNTIME_DYNINST:
          if (!parse_cmdline_runtime ("dyninst"))
            return 1;
          break;

        case LONG_OPT_BENCHMARK_SDT:
          // XXX This option is secret, not supported, subject to change at our whim
          benchmark_sdt_threads = thread::hardware_concurrency();
          break;

        case LONG_OPT_BENCHMARK_SDT_LOOPS:
          assert(optarg != 0); // optarg can't be NULL (or getopt would choke)
          // XXX This option is secret, not supported, subject to change at our whim
          benchmark_sdt_loops = strtoul(optarg, NULL, 10);
          break;

        case LONG_OPT_BENCHMARK_SDT_THREADS:
          assert(optarg != 0); // optarg can't be NULL (or getopt would choke)
          // XXX This option is secret, not supported, subject to change at our whim
          benchmark_sdt_threads = strtoul(optarg, NULL, 10);
          break;

        case LONG_OPT_COLOR_ERRS:
          // --color without arg is equivalent to always
          if (!optarg || !strcmp(optarg, "always"))
            color_mode = color_always;
          else if (!strcmp(optarg, "auto"))
            color_mode = color_auto;
          else if (!strcmp(optarg, "never"))
            color_mode = color_never;
          else {
            cerr << _F("Invalid argument '%s' for --color.", optarg) << endl;
            return 1;
          }
          color_errors = color_mode == color_always
              || (color_mode == color_auto && isatty(STDERR_FILENO) &&
                    strcmp(getenv("TERM") ?: "notdumb", "dumb"));
          break;

        case LONG_OPT_PROLOGUE_SEARCHING:
          // --prologue-searching without arg is equivalent to always
          if (!optarg || !strcmp(optarg, "always"))
            prologue_searching_mode = prologue_searching_always;
          else if (!strcmp(optarg, "auto"))
            prologue_searching_mode = prologue_searching_auto;
          else if (!strcmp(optarg, "never"))
            prologue_searching_mode = prologue_searching_never;
          else {
            cerr << _F("Invalid argument '%s' for --prologue-searching.", optarg) << endl;
            return 1;
          }
          break;

        case LONG_OPT_SAVE_UPROBES:
          save_uprobes = true;
          break;

        case LONG_OPT_TARGET_NAMESPACES:
          assert(optarg);
          target_namespaces_pid = (int) strtoul(optarg, &num_endptr, 10);
          if (*num_endptr != '\0' || target_namespaces_pid < 1)
            {
              cerr << _("Invalid process ID number for target namespaces.") << endl;
              return 1;
            }
          break;

        case LONG_OPT_MONITOR:
          monitor = true;
          if (optarg)
            {
              monitor_interval = (int) strtoul(optarg, &num_endptr, 10);
              if (*num_endptr != '\0' || monitor_interval < 1)
                {
                  cerr << _("Invalid monitor interval.") << endl;
                  return 1;
                }
            }
          break;

	case '?':
	  // Invalid/unrecognized option given or argument required, but
	  // not given. In both cases getopt_long() will have printed the
	  // appropriate error message to stderr already.
	  usage(1);
	  break;

        default:
          // NOTREACHED unless one added a getopt option but not a corresponding case:
          cerr << _F("Unhandled argument code %d", (char)grc) << endl;
          return 1;
          break;
        }
    }

  for (std::set<std::string>::iterator it = additional_unwindsym_modules.begin();
       it != additional_unwindsym_modules.end();
       it++)
    {
      if (is_user_module(*it))
	{
	  unwindsym_modules.insert (resolve_path(sysroot + *it));
	}
      else
	{
	  unwindsym_modules.insert (*it);
	}
    }

  return 0;
}

bool
systemtap_session::parse_cmdline_runtime (const string& opt_runtime)
{
  if (opt_runtime == string("kernel"))
    runtime_mode = kernel_runtime;
  else if (opt_runtime == string("bpf"))
    {
      runtime_mode = bpf_runtime;
      use_cache = use_script_cache = false;
    }
  else if (opt_runtime == string("dyninst"))
    {
#ifndef HAVE_DYNINST
      cerr << _("ERROR: --runtime=dyninst unavailable; this build lacks DYNINST feature") << endl;
      version();
      return false;
#else
      if (privilege_set && pr_unprivileged != privilege)
        {
          cerr << _("ERROR: --runtime=dyninst implies unprivileged mode only") << endl;
          return false;
        }
      privilege = pr_unprivileged;
      privilege_set = true;
      runtime_mode = dyninst_runtime;
#endif
    }
  else
    {
      cerr << _F("ERROR: %s is an invalid argument for --runtime", opt_runtime.c_str()) << endl;
      return false;
    }

  return true;
}

void
systemtap_session::check_options (int argc, char * const argv [])
{
  for (int i = optind; i < argc; i++)
    {
      if (! have_script && ! dump_mode)
        {
          script_file = string (argv[i]);
          have_script = true;
        }
      else
        args.push_back (string (argv[i]));
    }

  // We don't need a script with --list-servers, --trust-servers, or any dump mode
  bool need_script = server_status_strings.empty () &&
                     server_trust_spec.empty () &&
                     !dump_mode && !interactive_mode;

  if (benchmark_sdt_loops > 0 || benchmark_sdt_threads > 0)
    {
      // Secret benchmarking options are for local use only, not servers or --remote
      if (client_options || !remote_uris.empty())
	{
	  cerr << _("Benchmark options are only for local use.") << endl;
	  usage(1);
	}

      // Fill defaults if either is unset.
      if (benchmark_sdt_loops == 0)
	benchmark_sdt_loops = 10000000;
      if (benchmark_sdt_threads == 0)
	benchmark_sdt_threads = 1;

      need_script = false;
    }

  // need a user file
  // NB: this is also triggered if stap is invoked with no arguments at all
  if (need_script && ! have_script)
    {
      cerr << _("A script must be specified.") << endl;
      cerr << _("Try '-i' for building a script interactively.") << endl;
      usage(1);
    }
  if (dump_mode && have_script)
    {
      cerr << _("Cannot specify a script with -l/-L/--dump-* switches.") << endl;
      usage(1);
    }
  if (dump_mode && last_pass != 5)
    {
      cerr << _("Cannot specify -p with -l/-L/--dump-* switches.") << endl;
      usage(1);
    }
  if (dump_mode && interactive_mode)
    {
      cerr << _("Cannot specify -i with -l/-L/--dump-* switches.") << endl;
      usage(1);
    }
  if (dump_mode && monitor)
    {
      cerr << _("Cannot specify --monitor with -l/-L/--dump-* switches.") << endl;
      usage(1);
    }
  // FIXME: we need to think through other options that shouldn't be
  // used with '-i'.

#if ! HAVE_NSS
  if (client_options)
    print_warning("--client-options is not supported by this version of systemtap");

  if (! server_status_strings.empty ())
    {
      print_warning("--list-servers is not supported by this version of systemtap");
      server_status_strings.clear ();
    }

  if (! server_trust_spec.empty ())
    {
      print_warning("--trust-servers is not supported by this version of systemtap");
      server_trust_spec.clear ();
    }
#endif

#if ! HAVE_MONITOR_LIBS
  if (monitor)
    {
      print_warning("Monitor mode is not supported by this version of systemtap");
      exit(1);
    }
#endif

  if (runtime_specified && ! specified_servers.empty ())
    {
      print_warning("Ignoring --use-server due to the use of -R");
      specified_servers.clear ();
    }

  if (client_options && last_pass > 4)
    {
      last_pass = 4; /* Quietly downgrade.  Server passed through -p5 naively. */
    }

  // If phase 5 has been requested, automatically adjust the --privilege setting to match the
  // user's actual privilege level and add --use-server, if necessary.
  // Do this only if we have a script and we are not the server.
  // Also do this only if we're running in the kernel (e.g. not --runtime=dyninst)
  // XXX Eventually we could check remote hosts, but disable that case for now.
  if (last_pass > 4 && have_script && ! client_options &&
      runtime_mode == kernel_runtime && remote_uris.empty())
    {
      // What is the user's privilege level?
      privilege_t credentials = get_privilege_credentials ();
      // Don't alter specifically-requested privilege levels
      if (! privilege_set && ! pr_contains (credentials, privilege))
	{
	  // We do not have the default privilege credentials (stapdev). Lower
	  // the privilege level to match our credentials.
	  if (pr_contains (credentials, pr_stapsys))
	    {
	      privilege = pr_stapsys;
	      server_args.push_back ("--privilege=stapsys");
	      auto_privilege_level_msg =
		_("--privilege=stapsys was automatically selected because you are a member "
		  "of the groups stapusr and stapsys.  [man stap]");
	    }
	  else if (pr_contains (credentials, pr_stapusr))
	    {
	      privilege = pr_stapusr;
	      server_args.push_back ("--privilege=stapusr");
	      auto_privilege_level_msg =
		_("--privilege=stapusr was automatically selected because you are a member "
		  "of the group stapusr.  [man stap]");
	    }
	  else
	    {
	      // Completely unprivileged user.
	      cerr << _("You are trying to run systemtap as a normal user.\n"
			"You should either be root, or be part of "
			"the group \"stapusr\" and possibly one of the groups "
			"\"stapsys\" or \"stapdev\".  [man stap]\n");
#if HAVE_DYNINST
	      cerr << _("Alternatively, you may specify --runtime=dyninst for userspace probing.\n");
#endif
	      usage (1); // does not return.
	    }
	}
      // Add --use-server if not already specified and the user's (lack of) credentials require
      // it for pass 5.
      if (! pr_contains (credentials, pr_stapdev))
	{
	  enable_auto_server (
            _F("For users with the privilege level %s, the module created by compiling your "
	       "script must be signed by a trusted systemtap compile-server.  [man stap-server]",
	       pr_name (credentials)));
	}
    }

  if (client_options && ! pr_contains (privilege, pr_stapdev) && ! client_options_disallowed_for_unprivileged.empty ())
    {
      cerr << _F("You can't specify %s when --privilege=%s is specified.",
                 client_options_disallowed_for_unprivileged.c_str(),
		 pr_name (privilege))
	   << endl;
      usage (1);
    }
  if ((cmd != "") && (target_pid))
    {
      cerr << _F("You can't specify %s and %s together.", "-c", "-x") << endl;
      usage (1);
    }

  // NB: In user-mode runtimes (dyninst), we can allow guru mode any time, but we
  // need to restrict guru by privilege level in the kernel runtime.
  if (! runtime_usermode_p () && ! pr_contains (privilege, pr_stapdev) && guru_mode)
    {
      cerr << _F("You can't specify %s and --privilege=%s together.", "-g", pr_name (privilege))
	   << endl;
      usage (1);
    }

  // Can't use --remote and --tmpdir together because with --remote,
  // there may be more than one tmpdir needed.
  if (!remote_uris.empty() && tmpdir_opt_set)
    {
      cerr << _F("You can't specify %s and %s together.", "--remote", "--tmpdir") << endl;
      usage(1);
    }
  // Warn in case the target kernel release doesn't match the running one.
  native_build = (release == kernel_release &&
                  machine == architecture); // NB: squashed ARCH by PR4186 logic

  // Non-native builds can't be loaded locally, but may still work on remotes
  if (last_pass > 4 && !native_build && remote_uris.empty())
    {
      print_warning("kernel release/architecture mismatch with host forces last-pass 4.");
      last_pass = 4;
    }
  if(download_dbinfo != 0 && access ("/usr/bin/abrt-action-install-debuginfo-to-abrt-cache", X_OK) < 0
                          && access ("/usr/libexec/abrt-action-install-debuginfo-to-abrt-cache", X_OK) < 0)
    {
      print_warning("abrt-action-install-debuginfo-to-abrt-cache is not installed. Continuing without downloading debuginfo.");
      download_dbinfo = 0;
    }

  // translate path of runtime to absolute path
  if (runtime_path[0] != '/')
    {
      char cwd[PATH_MAX];
      if (getcwd(cwd, sizeof(cwd)))
        {
          runtime_path = string(cwd) + "/" + runtime_path;
        }
    }

  // Abnormal characters in our temp path can break us, including parts out
  // of our control like Kbuild.  Let's enforce nice, safe characters only.
  const char *tmpdir = getenv("TMPDIR");
  if (tmpdir != NULL)
    assert_regexp_match("TMPDIR", tmpdir, "^[-/._0-9a-z]+$");

  // If the kernel is using signed modules, we need to enforce server
  // use.
  if (!client_options && modules_must_be_signed())
    {
      // Force server use to be on, if not on already.
      enable_auto_server (
	_("The kernel on your system requires modules to be signed for loading.\n"
	  "The module created by compiling your script must be signed by a systemtap "
	  "compile-server.  [man stap-server]"));

      // Cache the current system's machine owner key (MOK)
      // information, to pass over to the server.
      get_mok_info();
    }
}

int
systemtap_session::parse_kernel_config ()
{
  // PR10702: pull config options
  string kernel_config_file = kernel_build_tree + "/.config";
  struct stat st;
  int rc = stat(kernel_config_file.c_str(), &st);
  if (rc != 0)
    {
        clog << _F("Checking \"%s\" failed with error: %s",
                   kernel_config_file.c_str(), strerror(errno)) << endl;
	find_devel_rpms(*this, kernel_build_tree.c_str());
	missing_rpm_list_print(*this, "-devel");
	return rc;
    }

  ifstream kcf (kernel_config_file.c_str());
  string line;
  while (getline (kcf, line))
    {
      if (!startswith(line, "CONFIG_")) continue;
      size_t off = line.find('=');
      if (off == string::npos) continue;
      string key = line.substr(0, off);
      string value = line.substr(off+1, string::npos);
      kernel_config[key] = value;
    }
  if (verbose > 2)
    clog << _F("Parsed kernel \"%s\", ", kernel_config_file.c_str())
         << _NF("containing %zu tuple", "containing %zu tuples",
                kernel_config.size(), kernel_config.size()) << endl;


  kcf.close();
  return 0;
}


int
systemtap_session::parse_kernel_exports ()
{
  string kernel_exports_file = kernel_build_tree + "/Module.symvers";
  struct stat st;
  int rc = stat(kernel_exports_file.c_str(), &st);
  if (rc != 0)
    {
        clog << _F("Checking \"%s\" failed with error: %s\nEnsure kernel development headers & makefiles are installed",
                   kernel_exports_file.c_str(), strerror(errno)) << endl;
	return rc;
    }

  ifstream kef (kernel_exports_file.c_str());
  string line;
  while (getline (kef, line))
    {
      vector<string> tokens;
      tokenize (line, tokens, "\t");
      if (tokens.size() == 4 &&
          tokens[2] == "vmlinux" &&
          tokens[3].substr(0,13) == string("EXPORT_SYMBOL"))
        kernel_exports.insert (tokens[1]);
      // RHEL4 Module.symvers file only has 3 tokens.  No
      // 'EXPORT_SYMBOL' token at the end of the line.
      else if (tokens.size() == 3 && tokens[2] == "vmlinux")
        kernel_exports.insert (tokens[1]);
    }
  if (verbose > 2)
    clog << _NF("Parsed kernel \"%s\", containing one vmlinux export",
                "Parsed kernel \"%s\", containing %zu vmlinux exports",
                kernel_exports.size(), kernel_exports_file.c_str(),
                kernel_exports.size()) << endl;

  kef.close();
  return 0;
}


int
systemtap_session::parse_kernel_functions ()
{
  string system_map_path = kernel_build_tree + "/System.map";
  ifstream system_map;

  system_map.open(system_map_path.c_str(), ifstream::in);
  if (! system_map.is_open())
    {
      if (verbose > 1)
	clog << _F("Kernel symbol table %s unavailable, (%s)",
		   system_map_path.c_str(), strerror(errno)) << endl;

      system_map_path = "/boot/System.map-" + kernel_release;
      system_map.clear();
      system_map.open(system_map_path.c_str(), ifstream::in);
      if (! system_map.is_open())
        {
	  if (verbose > 1)
	    clog << _F("Kernel symbol table %s unavailable, (%s)",
		       system_map_path.c_str(), strerror(errno)) << endl;
        }
    }

  while (system_map.good())
    {
      assert_no_interrupts();

      string address, type, name;
      system_map >> address >> type >> name;

      if (verbose > 3)
        clog << "'" << address << "' '" << type << "' '" << name
             << "'" << endl;

      // 'T'/'t' are text code symbols
      if (type != "t" && type != "T")
          continue;

#ifdef __powerpc__ // XXX cross-compiling hazard
      // Map ".sys_foo" to "sys_foo".
      if (name[0] == '.')
          name.erase(0, 1);
#endif

      // FIXME: better things to do here - look for _stext before
      // remembering symbols. Also:
      // - stop remembering names at ???
      // - what about __kprobes_text_start/__kprobes_text_end?
      kernel_functions.insert(name);
    }
  system_map.close();

  if (kernel_functions.size() == 0)
    print_warning ("Kernel function symbol table missing [man warning::symbols]", 0);

  if (verbose > 2)
    clog << _F("Parsed kernel \"%s\", ", system_map_path.c_str())
         << _NF("containing %zu symbol", "containing %zu symbols",
                kernel_functions.size(), kernel_functions.size()) << endl;

  return 0;
}


string
systemtap_session::cmd_file ()
{
  wordexp_t words;
  string file;
  
  if (target_pid && cmd == "")
    {
      // check that the target_pid corresponds to a running process
      string err_msg;
      if(!is_valid_pid (target_pid, err_msg))
        throw SEMANTIC_ERROR(err_msg);

      file = string("/proc/") + lex_cast(target_pid) + "/exe";
    }
  else // default is to assume -c flag was given
    {  
      int rc = wordexp (cmd.c_str (), &words, WRDE_NOCMD|WRDE_UNDEF);
      if(rc == 0)
        {
          if (words.we_wordc > 0)
            file = words.we_wordv[0];
          wordfree (& words);
        }
      else
        {
          switch (rc)
            {
              case WRDE_BADCHAR:
                throw SEMANTIC_ERROR(_("command contains illegal characters"));
              case WRDE_BADVAL:
                throw SEMANTIC_ERROR(_("command contains undefined shell variables"));
              case WRDE_CMDSUB:
                throw SEMANTIC_ERROR(_("command contains command substitutions"));
              case WRDE_NOSPACE:
                throw SEMANTIC_ERROR(_("out of memory"));
              case WRDE_SYNTAX:
                throw SEMANTIC_ERROR(_("command contains shell syntax errors"));
              default:
                throw SEMANTIC_ERROR(_("unspecified wordexp failure"));
           }  
        }
    }
  return file;
}


void
systemtap_session::init_try_server ()
{
#if HAVE_NSS
  // If the option is disabled or we are a server or we are already using a
  // server, then never retry compilation using a server.
  if (! use_server_on_error || client_options || ! specified_servers.empty ())
    try_server_status = dont_try_server;
  else
    try_server_status = try_server_unset;
#else
  // No client, so don't bother.
  try_server_status = dont_try_server;
#endif
}

void
systemtap_session::set_try_server (int t)
{
  if (try_server_status != dont_try_server)
    try_server_status = t;
}


void systemtap_session::insert_loaded_modules()
{
  char line[1024];
  ifstream procmods ("/proc/modules");
  while (procmods.good()) {
    procmods.getline (line, sizeof(line));
    strtok(line, " \t");
    if (line[0] == '\0')
      break;  // maybe print a warning?
    unwindsym_modules.insert (string (line));
  }
  procmods.close();
  unwindsym_modules.insert ("kernel");
}

void
systemtap_session::setup_kernel_release (const char* kstr) 
{
  // Sometimes we may get dupes here... e.g. a server may have a full
  // -r /path/to/kernel followed by a client's -r kernel.
  if (kernel_release == kstr)
    return; // nothing new here...

  kernel_release = kernel_build_tree = kernel_source_tree = "";
  if (kstr[0] == '/') // fully specified path
    {
      kernel_build_tree = kstr;
      kernel_release = kernel_release_from_build_tree (kernel_build_tree, verbose);

      // PR10745
      // Maybe it's a full kernel source tree, for purposes of PR10745.
      // In case CONFIG_DEBUG_INFO was set, we'd find it anyway with the
      // normal search in tapsets.cxx.  Without CONFIG_DEBUG_INFO, we'd
      // need heuristics such as this one:

      string some_random_source_only_file = kernel_build_tree + "/COPYING";
      ifstream epic (some_random_source_only_file.c_str());
      if (! epic.fail())
        {
          kernel_source_tree = kernel_build_tree;
          if (verbose > 2)
            clog << _F("Located kernel source tree (COPYING) at '%s'", kernel_source_tree.c_str()) << endl;
        }
    }
  else
    {
      update_release_sysroot = true;
      kernel_release = string (kstr);
      if (!kernel_release.empty())
        kernel_build_tree = "/lib/modules/" + kernel_release + "/build";

      // PR10745
      // Let's not look for the kernel_source_tree; it's definitely
      // not THERE.  tapsets.cxx might try to find it later if tracepoints
      // need it.
    }
}


// Register all the aliases we've seen in library files, and the user
// file, as patterns.
void
systemtap_session::register_library_aliases()
{
  vector<stapfile*> files(library_files);
  files.insert(files.end(), user_files.begin(), user_files.end());

  for (unsigned f = 0; f < files.size(); ++f)
    {
      stapfile * file = files[f];
      for (unsigned a = 0; a < file->aliases.size(); ++a)
	{
	  probe_alias * alias = file->aliases[a];
          try
            {
              for (unsigned n = 0; n < alias->alias_names.size(); ++n)
                {
                  probe_point * name = alias->alias_names[n];
                  match_node * mn = pattern_root;
                  for (unsigned c = 0; c < name->components.size(); ++c)
                    {
                      probe_point::component * comp = name->components[c];
                      // XXX: alias parameters
                      if (comp->arg)
                        throw SEMANTIC_ERROR(_F("alias component %s contains illegal parameter",
                                                comp->functor.to_string().c_str()));
                      mn = mn->bind(comp->functor);
                    }
		  // PR 12916: All probe aliases are OK for all users. The actual
		  // referenced probe points will be checked when the alias is resolved.
		  mn->bind_privilege (pr_all);
                  mn->bind(new alias_expansion_builder(alias));
                }
            }
          catch (const semantic_error& e)
            {
              semantic_error er(ERR_SRC, _("while registering probe alias"),
                                alias->tok, NULL, &e);
              print_error (er);
            }
	}
    }
}


// Print this given token, but abbreviate it if the last one had the
// same file name.
void
systemtap_session::print_token (ostream& o, const token* tok)
{
  assert (tok);

  if (last_token && last_token->location.file == tok->location.file)
    {
      stringstream tmpo;
      tmpo << *tok;
      string ts = tmpo.str();
      // search & replace the file name with nothing
      size_t idx = ts.find (tok->location.file->name);
      if (idx != string::npos) {
          ts.erase(idx, tok->location.file->name.size()); // remove path
          if (color_errors) {
            string src = ts.substr(idx); // keep line & col
            ts.erase(idx);               // remove from output string
            ts += colorize(src, "source");        // re-add it colorized
          }
      }

      o << ts;
    }
  else
    o << colorize(tok);

  last_token = tok;
}


void
systemtap_session::print_error (const semantic_error& se)
{
  // skip error message printing for listing mode with low verbosity
  if (this->dump_mode && this->verbose <= 1)
    {
      seen_errors[se.errsrc_chain()]++; // increment num_errors()
      return;
    }

  // duplicate elimination
  if (verbose > 0 || seen_errors[se.errsrc_chain()] < 1)
    {
      seen_errors[se.errsrc_chain()]++;
      for (const semantic_error *e = &se; e != NULL; e = e->get_chain())
        cerr << build_error_msg(*e);
    }
  else suppressed_errors++;
}

string
systemtap_session::build_error_msg (const semantic_error& e)
{
  stringstream message;
  string align_semantic_error ("        ");

  message << colorize(_("semantic error:"), "error") << ' ' << e.what ();
  if (e.tok1 || e.tok2)
    message << ": ";
  else
    {
      print_error_details (message, align_semantic_error, e);
      message << endl;
      if (verbose > 1) // no tokens to print, so print any errsrc right there
	message << _("   thrown from: ") << e.errsrc;
    }

  if (e.tok1)
    {
      print_token (message, e.tok1);
      print_error_details (message, align_semantic_error, e);
      message << endl;
      if (verbose > 1)
        message << _("   thrown from: ") << e.errsrc << endl;
      print_error_source (message, align_semantic_error, e.tok1);
    }
  if (e.tok2)
    {
      print_token (message, e.tok2);
      message << endl;
      print_error_source (message, align_semantic_error, e.tok2);
    }
  message << endl;
  return message.str();
}

void
systemtap_session::print_error_source (std::ostream& message,
                                       std::string& align, const token* tok)
{
  unsigned i = 0;

  assert (tok);
  if (!tok->location.file)
    //No source to print, silently exit
    return;

  unsigned line = tok->location.line;
  unsigned col = tok->location.column;
  interned_string file_contents = tok->location.file->file_contents;

  size_t start_pos = 0, end_pos = 0;
  //Navigate to the appropriate line
  while (i != line && end_pos != std::string::npos)
    {
      start_pos = end_pos;
      end_pos = file_contents.find ('\n', start_pos) + 1;
      i++;
    }
  //TRANSLATORS: Here we are printing the source string of the error
  message << align << _("source: ");
  interned_string srcline = file_contents.substr(start_pos, end_pos-start_pos-1);
  if (color_errors) {
      // before token:
      message << srcline.substr(0, col-1);
      // token:
      // after token:
      // ... hold it - the token might have been synthetic,
      // or expanded from a $@ command line argument, or ...
      // so tok->content may have nothing in common with the
      // contents of srcline at the same point.
      interned_string srcline_rest = srcline.substr(col-1);
      interned_string srcline_tokenish = srcline_rest.substr(0, tok->content.size());
      if (srcline_tokenish == tok->content) { // oh good
        message << colorize(tok->content, "token");
        message << srcline_rest.substr(tok->content.size());
        message << endl;
      }
      else { // oh bad
        message << " ... ";
        col += 5; // line up with the caret
        message << colorize(tok->content, "token");
        message << " ... " << srcline_rest;
        message << endl;
      }
  } else
    message << srcline << endl;
  message << align << "        ";
  //Navigate to the appropriate column
  for (i=start_pos; i<start_pos+col-1; i++)
    {
      if(isspace(file_contents[i]))
	message << file_contents[i];
      else
	message << ' ';
    }
  message << colorize("^", "caret") << endl;

  // print chained macro invocations and synthesized code
  if (tok->chain)
    {
      if (tok->location.file->synthetic)
	message << _("\tin synthesized code from: ");
      else
	message << _("\tin expansion of macro: ");
      message << colorize(tok->chain) << endl;
      print_error_source (message, align, tok->chain);
    }
}

void
systemtap_session::print_error_details (std::ostream& message,
					std::string& align,
					const semantic_error& e)
{
  for (size_t i = 0; i < e.details.size(); ++i)
    message << endl << align << e.details[i];
}

void
systemtap_session::print_warning (const string& message_str, const token* tok)
{
  // Only output in dump mode if -vv is supplied:
  if (suppress_warnings && (!dump_mode || verbose <= 1))
    return; // NB: don't count towards suppressed_warnings count

  // Duplicate elimination
  string align_warning (" ");
  if (verbose > 0 || seen_warnings.find (message_str) == seen_warnings.end())
    {
      seen_warnings.insert (message_str);
      clog << colorize(_("WARNING:"), "warning") << ' ' << message_str;
      if (tok) { clog << ": "; print_token (clog, tok); }
      clog << endl;
      if (tok) { print_error_source (clog, align_warning, tok); }
    }
  else suppressed_warnings++;
}


void
systemtap_session::print_error (const parse_error &pe,
                                const token* tok,
                                const std::string &input_name,
                                bool is_warningerr)
{
  // duplicate elimination
  if (verbose > 0 || seen_errors[pe.errsrc_chain()] < 1)
    {
      // Sometimes, we need to print parse errors that should not be considered
      // critical. For example, when we parse tapsets and macros. In those
      // cases, is_warningerr is true, and we cancel out the increase in
      // seen_errors.size() by also increasing warningerr_count, so that the
      // net num_errors() value is unchanged.
      seen_errors[pe.errsrc_chain()]++;                         // can be simplified if we
      if (seen_errors[pe.errsrc_chain()] == 1 && is_warningerr) // change map to a set (and
        warningerr_count++;                                     // settle on threshold of 1)
      cerr << build_error_msg(pe, tok, input_name);
      for (const parse_error *e = pe.chain; e != NULL; e = e->chain)
        cerr << build_error_msg(*e, e->tok, input_name);
    }
  else suppressed_errors++;
}

string
systemtap_session::build_error_msg (const parse_error& pe,
                                    const token* tok,
                                    const std::string &input_name)
{
  stringstream message;
  string align_parse_error ("     ");

  // print either pe.what() or a deferred error from the lexer
  bool found_junk = false;
  if (tok && tok->type == tok_junk && tok->junk_type != tok_junk_unknown)
    {
      found_junk = true;
      message << colorize(_("parse error:"), "error") << ' '
              << tok->junk_message(*this) << endl;
    }
  else
    {
      message << colorize(_("parse error:"), "error") << ' ' << pe.what() << endl;
    }

  // NB: It makes sense for lexer errors to always override parser
  // errors, since the original obvious scheme was for the lexer to
  // throw an exception before the token reached the parser.

  if (pe.tok || found_junk)
    {
      message << align_parse_error << _("    at: ") << colorize(tok) << endl;
      print_error_source (message, align_parse_error, tok);
    }
  else if (tok) // "expected" type error
    {
      message << align_parse_error << _("   saw: ") << colorize(tok) << endl;
      print_error_source (message, align_parse_error, tok);
    }
  else
    {
      message << align_parse_error << _("   saw: ") << input_name << " EOF" << endl;
    }
  message << endl;

  return message.str();
}

void
systemtap_session::report_suppression()
{
  if (this->suppressed_errors > 0)
    cerr << colorize(_F("Number of similar error messages suppressed: %d.",
                         this->suppressed_errors),
                      "error") << endl;
  if (this->suppressed_warnings > 0)
    cerr << colorize(_F("Number of similar warning messages suppressed: %d.",
                         this->suppressed_warnings),
                      "warning") << endl;
  if (this->suppressed_errors > 0 || this->suppressed_warnings > 0)
    cerr << "Rerun with -v to see them." << endl;
}

void
systemtap_session::create_tmp_dir()
{
  if (!tmpdir.empty())
    return;

  if (tmpdir_opt_set)
    return;

  // Create the temp directory
  const char * tmpdir_env = getenv("TMPDIR");
  if (!tmpdir_env)
    tmpdir_env = "/tmp";

  string stapdir = "/stapXXXXXX";
  string tmpdirt = tmpdir_env + stapdir;
  const char *tmpdir_name = mkdtemp((char *)tmpdirt.c_str());
  if (! tmpdir_name)
    {
      const char* e = strerror(errno);
      //TRANSLATORS: we can't make the directory due to the error
      throw runtime_error(_F("cannot create temporary directory (\" %s \"): %s", tmpdirt.c_str(), e));
    }
  else
    tmpdir = tmpdir_name;
}

void
systemtap_session::remove_tmp_dir()
{
  if(tmpdir.empty())
    return;

  // Remove temporary directory
  if (keep_tmpdir && !tmpdir_opt_set)
      clog << _F("Keeping temporary directory \"%s\"", tmpdir.c_str()) << endl;
  else if (!tmpdir_opt_set)
    {
      // Mask signals while we're deleting the temporary directory.
      stap_sigmasker masked;

      // Remove the temporary directory.
      vector<string> cleanupcmd { "rm", "-rf", tmpdir };
      (void) stap_system(verbose, cleanupcmd);
      if (verbose>1)
        clog << _F("Removed temporary directory \"%s\"", tmpdir.c_str()) << endl;
      tmpdir.clear();

    }
}

void
systemtap_session::reset_tmp_dir()
{
  remove_tmp_dir();
  create_tmp_dir();
}

translator_output* systemtap_session::op_create_auxiliary(bool trailer_p)
{
  static int counter = 0;
  string tmpname = this->tmpdir + "/" + this->module_name + "_aux_" + lex_cast(counter++) + ".c";
  translator_output* n = new translator_output (tmpname);
  n->trailer_p = trailer_p;
  auxiliary_outputs.push_back (n);
  return n;
}

// Wrapper for checking if there are pending interrupts
void
assert_no_interrupts()
{
  if (pending_interrupts)
    throw interrupt_exception();
}

std::string
systemtap_session::colorize(const std::string& str, const std::string& type)
{
  if (str.empty() || !color_errors)
    return str;
  else {
    // Check if this type is defined in SYSTEMTAP_COLORS
    std::string color = parse_stap_color(type);
    if (!color.empty()) // no need to pollute terminal if not necessary
      return "\033[" + color + "m\033[K" + str + "\033[m\033[K";
    else
      return str;
  }
}

// Colorizes the path:row:col part of the token
std::string
systemtap_session::colorize(const token* tok)
{
  if (tok == NULL)
    return "";

  stringstream tmp;
  tmp << *tok;

  if (!color_errors)
    return tmp.str(); // Might as well stop now to save time
  else {
    string ts = tmp.str();

    // Print token location, which is also the tail of ts
    stringstream loc;
    loc << tok->location;

    // Remove token location and re-add it colorized
    ts.erase(ts.size()-loc.str().size());
    return ts + colorize(loc.str(), "source");
  }
}

/* Parse SYSTEMTAP_COLORS and returns the SGR parameter(s) for the given
type. The env var SYSTEMTAP_COLORS must be in the following format:
'key1=val1:key2=val2:' etc... where valid keys are 'error', 'warning',
'source', 'caret', 'token' and valid values constitute SGR parameter(s).
For example, the default setting would be:
'error=01;31:warning=00;33:source=00;34:caret=01:token=01'
*/
std::string
systemtap_session::parse_stap_color(const std::string& type)
{
  const char *key, *col, *eq;
  int n = type.size();
  int done = 0;

  key = getenv("SYSTEMTAP_COLORS");
  if (key == NULL)
    key = "error=01;31:warning=00;33:source=00;34:caret=01:token=01";
  else if (*key == '\0')
    return ""; // disable colors if set but empty

  while (!done) {
    if (!(col = strchr(key, ':'))) {
      col = strchr(key, '\0');
      done = 1;
    }
    if (!((eq = strchr(key, '=')) && eq < col))
      return ""; /* invalid syntax: no = in range */
    if (!(key < eq && eq < col-1))
      return ""; /* invalid syntax: key or val empty */
    if (strspn(eq+1, "0123456789;") < (size_t)(col-eq-1))
      return ""; /* invalid syntax: invalid char in val */
    if (eq-key == n && type.compare(0, n, key, n) == 0)
      return string(eq+1, col-eq-1);
    if (!done) key = col+1; /* advance to next key */
  }

  // Could not find the key
  return "";
}

/*
 * Returns true if this system requires modules to have signatures
 * (typically on a secure boot system), false otherwise.
 *
 * This routine parses /sys/module/module/parameters/sig_enforce to
 * figure out if signatures are enforced on modules. Note that if the
 * file doesn't exist, we don't really care and return false.
 *
 * On certain kernels (RHEL7), we also have to check
 * /sys/kernel/security/securelevel.
 */
bool
systemtap_session::modules_must_be_signed()
{
  ifstream statm("/sys/module/module/parameters/sig_enforce");
  ifstream securelevel("/sys/kernel/security/securelevel");
  char status = 'N';

  statm >> status;
  if (status == 'Y')
    return true;

  securelevel >> status;
  if (status == '1')
    return true;
  return false;
}

/*
 * Get information on the list of enrolled machine owner keys (MOKs) on
 * this system.
 */ 
void
systemtap_session::get_mok_info()
{
  int rc;
  stringstream out;

  // FIXME: In theory, we should be able to read /sys files and use
  // some of the guts of read_cert_info_from_file() to get the
  // fingerprints. This would rid us of our mokutil
  // dependency. However, we'd need to copy/duplicate efilib.c from
  // mokutil source to be able to decipher the efi data.

  vector<string> cmd { "mokutil", "--list-enrolled" };
  rc = stap_system_read(verbose, cmd, out);
  if (rc != 0)
    // If we're here, we know the client requires module signing, but
    // we can't get the list of MOKs. Quit.
    throw runtime_error(_F("failed to get list of machine owner keys (MOK) fingerprints: rc %d", rc));

  string line, fingerprint;
  while (! out.eof())
    {
      vector<string> matches;

      // Get a line of the output, then try to find the fingerprint in
      // the line.
      getline(out, line);
      if (! regexp_match(line, "^SHA1 Fingerprint: ([0-9a-f:]+)$", matches))
        {
	  // matches[0] is the entire line, matches[1] is the first
	  // submatch, in this case the actual fingerprint
	  if (verbose > 2)
	    clog << "MOK fingerprint found: " << matches[1] << endl;
	  if (! matches[1].empty())
	    mok_fingerprints.push_back(matches[1]);
	}
    }
}

void
systemtap_session::enable_auto_server (const string &message)
{
  // There may be more than one reason to enable auto server mode, so we may be called
  // more than once. Accumulate the messages.
  auto_server_msgs.push_back (message);

#if HAVE_NSS
  // Enable auto server mode, if not enabled already.
  if (specified_servers.empty())
    specified_servers.push_back ("");
#else
  // Compilation using a server is not supported. exit() after
  // the first explanation.
  explain_auto_options ();
  clog << _("Unable to request compilation by a compile-server\n."
	    "Without NSS, --use-server is not supported by this version systemtap.") << endl;
  exit(1);
#endif
}

void
systemtap_session::explain_auto_options()
{
  // Was there an automatic privilege setting?
  if (! auto_privilege_level_msg.empty())
    clog << auto_privilege_level_msg << endl;

  // Was a server was automatically requested? Handle this one after other auto_settings
  // which may result in an auto_server setting.
  if (! auto_server_msgs.empty())
    {
      for (vector<string>::iterator i = auto_server_msgs.begin(); i != auto_server_msgs.end(); ++i)
	{
	  clog << *i << endl;
	  clog << _("--use-server was automatically selected in order to request compilation by "
		    "a compile-server.") << endl;
	}
    }
}

bool
systemtap_session::is_user_file (const string &name)
{
  // base the check on the name of the user_file
  for (vector<stapfile*>::iterator it = user_files.begin(); it != user_files.end(); it++)
    if (name == (*it)->name)
      return true;
  return false; // no match
}

bool
systemtap_session::is_primary_probe (derived_probe *dp)
{
  // We check if this probe is from the primary user file by going back to its
  // original probe and checking if that probe was found in the primary user
  // file.

  if (user_files.empty())
    return false;

  vector<probe*> chain;
  dp->collect_derivation_chain (chain);
  const source_loc& origin = chain.back()->tok->location;
  return origin.file == user_files[0];
}

void
systemtap_session::clear_script_data()
{
  delete pattern_root;
  pattern_root = new match_node;

  // We need to be sure to reset all our error counts, so that an
  // error on one script won't be counted against the next script.
  seen_warnings.clear();
  suppressed_warnings = 0;
  seen_errors.clear();
  suppressed_errors = 0;
  warningerr_count = 0;
}

// --------------------------------------------------------------------------

/*
Perngrq sebz fzvyrlgnc.fit, rkcbegrq gb n 1484k1110 fzvyrlgnc.cat,
gurapr  catgbcnz | cazfpnyr -jvqgu 160 | 
cczqvgure -qvz 4 -erq 2 -terra 2 -oyhr 2  | cczgbnafv -2k4 | bq -i -j19 -g k1 | 
phg -s2- -q' ' | frq -r 'f,^,\\k,' -r 'f, ,\\k,t' -r 'f,^,",'  -r 'f,$,",'
*/
const char*
systemtap_session::morehelp =
"\x1b\x5b\x30\x6d\x1b\x5b\x33\x37\x6d\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20"
"\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20"
"\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20"
"\x20\x20\x20\x60\x20\x20\x2e\x60\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20"
"\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x0a\x20\x20\x20\x20\x20"
"\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20"
"\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20"
"\x20\x20\x60\x20\x60\x20\x60\x20\x60\x20\x60\x20\x60\x20\x60\x20\x60\x1b\x5b"
"\x33\x33\x6d\x20\x1b\x5b\x33\x37\x6d\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20"
"\x20\x20\x20\x20\x20\x20\x20\x20\x20\x0a\x20\x20\x20\x20\x20\x20\x20\x20\x20"
"\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20"
"\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x1b\x5b\x33\x33\x6d\x20\x60"
"\x2e\x60\x1b\x5b\x33\x37\x6d\x20\x3a\x2c\x3a\x2e\x60\x20\x60\x20\x60\x20\x60"
"\x2c\x3b\x2c\x3a\x20\x1b\x5b\x33\x33\x6d\x60\x2e\x60\x20\x1b\x5b\x33\x37\x6d"
"\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x0a\x20\x20\x20"
"\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20"
"\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x1b\x5b\x33"
"\x33\x6d\x20\x60\x20\x60\x20\x3a\x27\x60\x1b\x5b\x33\x37\x6d\x20\x60\x60\x60"
"\x20\x20\x20\x60\x20\x60\x60\x60\x20\x1b\x5b\x33\x33\x6d\x60\x3a\x60\x20\x60"
"\x20\x60\x20\x1b\x5b\x33\x37\x6d\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20"
"\x20\x20\x0a\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20"
"\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20"
"\x20\x2e\x1b\x5b\x33\x33\x6d\x60\x2e\x60\x20\x60\x20\x60\x20\x20\x1b\x5b\x33"
"\x37\x6d\x20\x3a\x20\x20\x20\x60\x20\x20\x20\x60\x20\x20\x2e\x1b\x5b\x33\x33"
"\x6d\x60\x20\x60\x2e\x60\x20\x60\x2e\x60\x20\x1b\x5b\x33\x37\x6d\x20\x20\x20"
"\x20\x20\x20\x20\x20\x20\x20\x20\x0a\x20\x20\x20\x20\x20\x20\x2e\x3a\x20\x20"
"\x20\x20\x20\x20\x20\x20\x2e\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20"
"\x20\x20\x2e\x76\x53\x1b\x5b\x33\x34\x6d\x53\x1b\x5b\x33\x37\x6d\x53\x1b\x5b"
"\x33\x31\x6d\x2b\x1b\x5b\x33\x33\x6d\x60\x20\x60\x20\x60\x20\x20\x20\x20\x1b"
"\x5b\x33\x31\x6d\x3f\x1b\x5b\x33\x30\x6d\x53\x1b\x5b\x33\x33\x6d\x2b\x1b\x5b"
"\x33\x37\x6d\x20\x20\x20\x20\x20\x20\x20\x2e\x1b\x5b\x33\x30\x6d\x24\x1b\x5b"
"\x33\x37\x6d\x3b\x1b\x5b\x33\x31\x6d\x7c\x1b\x5b\x33\x33\x6d\x20\x60\x20\x60"
"\x20\x60\x20\x60\x1b\x5b\x33\x31\x6d\x2c\x1b\x5b\x33\x32\x6d\x53\x1b\x5b\x33"
"\x37\x6d\x53\x53\x3e\x2c\x2e\x20\x20\x20\x20\x20\x0a\x20\x20\x20\x20\x20\x2e"
"\x3b\x27\x20\x20\x20\x20\x20\x20\x20\x20\x20\x60\x3c\x20\x20\x20\x20\x20\x20"
"\x20\x20\x20\x2e\x2e\x3a\x1b\x5b\x33\x30\x6d\x26\x46\x46\x46\x48\x46\x1b\x5b"
"\x33\x33\x6d\x60\x2e\x60\x20\x60\x20\x60\x20\x60\x1b\x5b\x33\x30\x6d\x4d\x4d"
"\x46\x1b\x5b\x33\x33\x6d\x20\x20\x1b\x5b\x33\x37\x6d\x20\x20\x20\x20\x1b\x5b"
"\x33\x33\x6d\x20\x3a\x1b\x5b\x33\x30\x6d\x4d\x4d\x46\x1b\x5b\x33\x33\x6d\x20"
"\x20\x20\x60\x20\x60\x2e\x60\x1b\x5b\x33\x31\x6d\x3c\x1b\x5b\x33\x30\x6d\x46"
"\x46\x46\x24\x53\x46\x1b\x5b\x33\x37\x6d\x20\x20\x20\x20\x20\x0a\x20\x20\x20"
"\x20\x2e\x3c\x3a\x60\x20\x20\x20\x20\x2e\x3a\x2e\x3a\x2e\x2e\x3b\x27\x20\x20"
"\x20\x20\x20\x20\x2e\x60\x2e\x3a\x60\x60\x3c\x27\x1b\x5b\x33\x31\x6d\x3c\x27"
"\x1b\x5b\x33\x33\x6d\x20\x60\x20\x60\x20\x60\x20\x20\x20\x60\x3c\x1b\x5b\x33"
"\x30\x6d\x26\x1b\x5b\x33\x31\x6d\x3f\x1b\x5b\x33\x33\x6d\x20\x1b\x5b\x33\x37"
"\x6d\x20\x1b\x5b\x33\x33\x6d\x20\x20\x20\x20\x20\x1b\x5b\x33\x37\x6d\x60\x1b"
"\x5b\x33\x30\x6d\x2a\x46\x1b\x5b\x33\x37\x6d\x27\x1b\x5b\x33\x33\x6d\x20\x60"
"\x20\x60\x20\x60\x20\x60\x20\x1b\x5b\x33\x31\x6d\x60\x3a\x1b\x5b\x33\x37\x6d"
"\x27\x3c\x1b\x5b\x33\x30\x6d\x23\x1b\x5b\x33\x37\x6d\x3c\x60\x3a\x20\x20\x20"
"\x0a\x20\x20\x20\x20\x3a\x60\x3a\x60\x20\x20\x20\x60\x3a\x2e\x2e\x2e\x2e\x3c"
"\x3c\x20\x20\x20\x20\x20\x20\x3a\x2e\x60\x3a\x60\x20\x20\x20\x60\x1b\x5b\x33"
"\x33\x6d\x3a\x1b\x5b\x33\x31\x6d\x60\x1b\x5b\x33\x33\x6d\x20\x60\x2e\x60\x20"
"\x60\x20\x60\x20\x60\x20\x60\x1b\x5b\x33\x37\x6d\x20\x20\x1b\x5b\x33\x33\x6d"
"\x20\x60\x20\x20\x20\x60\x1b\x5b\x33\x37\x6d\x20\x60\x20\x60\x1b\x5b\x33\x33"
"\x6d\x20\x60\x2e\x60\x20\x60\x2e\x60\x20\x60\x3a\x1b\x5b\x33\x37\x6d\x20\x20"
"\x20\x60\x3a\x2e\x60\x2e\x20\x0a\x20\x20\x20\x60\x3a\x60\x3a\x60\x20\x20\x20"
"\x20\x20\x60\x60\x60\x60\x20\x3a\x2d\x20\x20\x20\x20\x20\x60\x20\x60\x20\x20"
"\x20\x20\x20\x60\x1b\x5b\x33\x33\x6d\x3a\x60\x2e\x60\x20\x60\x20\x60\x20\x60"
"\x20\x60\x20\x20\x2e\x3b\x1b\x5b\x33\x31\x6d\x76\x1b\x5b\x33\x30\x6d\x24\x24"
"\x24\x1b\x5b\x33\x31\x6d\x2b\x53\x1b\x5b\x33\x33\x6d\x2c\x60\x20\x60\x20\x60"
"\x20\x60\x20\x60\x20\x60\x2e\x1b\x5b\x33\x31\x6d\x60\x1b\x5b\x33\x33\x6d\x3a"
"\x1b\x5b\x33\x37\x6d\x20\x20\x20\x20\x60\x2e\x60\x20\x20\x0a\x20\x20\x20\x60"
"\x3a\x3a\x3a\x3a\x20\x20\x20\x20\x3a\x60\x60\x60\x60\x3a\x53\x20\x20\x20\x20"
"\x20\x20\x3a\x2e\x60\x2e\x20\x20\x20\x20\x20\x1b\x5b\x33\x33\x6d\x3a\x1b\x5b"
"\x33\x31\x6d\x3a\x1b\x5b\x33\x33\x6d\x2e\x60\x2e\x60\x20\x60\x2e\x60\x20\x60"
"\x20\x3a\x1b\x5b\x33\x30\x6d\x24\x46\x46\x48\x46\x46\x46\x46\x46\x1b\x5b\x33"
"\x31\x6d\x53\x1b\x5b\x33\x33\x6d\x2e\x60\x20\x60\x2e\x60\x20\x60\x2e\x60\x2e"
"\x1b\x5b\x33\x31\x6d\x3a\x1b\x5b\x33\x33\x6d\x3a\x1b\x5b\x33\x37\x6d\x20\x20"
"\x20\x2e\x60\x2e\x3a\x20\x20\x0a\x20\x20\x20\x60\x3a\x3a\x3a\x60\x20\x20\x20"
"\x60\x3a\x20\x2e\x20\x3b\x27\x3a\x20\x20\x20\x20\x20\x20\x3a\x2e\x60\x3a\x20"
"\x20\x20\x20\x20\x3a\x1b\x5b\x33\x33\x6d\x3c\x3a\x1b\x5b\x33\x31\x6d\x60\x1b"
"\x5b\x33\x33\x6d\x2e\x60\x20\x60\x20\x60\x20\x60\x2e\x1b\x5b\x33\x30\x6d\x53"
"\x46\x46\x46\x53\x46\x46\x46\x53\x46\x46\x1b\x5b\x33\x33\x6d\x20\x60\x20\x60"
"\x20\x60\x2e\x60\x2e\x60\x3a\x1b\x5b\x33\x31\x6d\x3c\x1b\x5b\x33\x37\x6d\x20"
"\x20\x20\x20\x3a\x60\x3a\x60\x20\x20\x0a\x20\x20\x20\x20\x60\x3c\x3b\x3c\x20"
"\x20\x20\x20\x20\x60\x60\x60\x20\x3a\x3a\x20\x20\x20\x20\x20\x20\x20\x3a\x3a"
"\x2e\x60\x20\x20\x20\x20\x20\x3a\x1b\x5b\x33\x33\x6d\x3b\x1b\x5b\x33\x31\x6d"
"\x3c\x3a\x60\x1b\x5b\x33\x33\x6d\x2e\x60\x2e\x60\x20\x60\x3a\x1b\x5b\x33\x30"
"\x6d\x53\x46\x53\x46\x46\x46\x53\x46\x46\x46\x53\x1b\x5b\x33\x33\x6d\x2e\x60"
"\x20\x60\x2e\x60\x2e\x60\x3a\x1b\x5b\x33\x31\x6d\x3c\x1b\x5b\x33\x33\x6d\x3b"
"\x1b\x5b\x33\x37\x6d\x27\x20\x20\x20\x60\x3a\x3a\x60\x20\x20\x20\x0a\x20\x20"
"\x20\x20\x20\x60\x3b\x3c\x20\x20\x20\x20\x20\x20\x20\x3a\x3b\x60\x20\x20\x20"
"\x20\x20\x20\x20\x20\x20\x60\x3a\x60\x2e\x20\x20\x20\x20\x20\x3a\x1b\x5b\x33"
"\x33\x6d\x3c\x3b\x1b\x5b\x33\x31\x6d\x3c\x1b\x5b\x33\x33\x6d\x3a\x1b\x5b\x33"
"\x31\x6d\x3a\x1b\x5b\x33\x33\x6d\x2e\x60\x2e\x60\x20\x1b\x5b\x33\x31\x6d\x3a"
"\x1b\x5b\x33\x30\x6d\x46\x53\x46\x53\x46\x53\x46\x53\x46\x1b\x5b\x33\x31\x6d"
"\x3f\x1b\x5b\x33\x33\x6d\x20\x60\x2e\x60\x2e\x3a\x3a\x1b\x5b\x33\x31\x6d\x3c"
"\x1b\x5b\x33\x33\x6d\x3b\x1b\x5b\x33\x31\x6d\x3c\x1b\x5b\x33\x37\x6d\x60\x20"
"\x20\x20\x3a\x3a\x3a\x60\x20\x20\x20\x20\x0a\x20\x20\x20\x20\x20\x20\x53\x3c"
"\x20\x20\x20\x20\x20\x20\x3a\x53\x3a\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20"
"\x20\x60\x3a\x3a\x60\x2e\x20\x20\x20\x20\x60\x3a\x1b\x5b\x33\x31\x6d\x3c\x1b"
"\x5b\x33\x33\x6d\x3b\x1b\x5b\x33\x31\x6d\x3c\x3b\x3c\x1b\x5b\x33\x33\x6d\x3a"
"\x60\x2e\x60\x3c\x1b\x5b\x33\x30\x6d\x53\x46\x53\x24\x53\x46\x53\x24\x1b\x5b"
"\x33\x33\x6d\x60\x3a\x1b\x5b\x33\x31\x6d\x3a\x1b\x5b\x33\x33\x6d\x3a\x1b\x5b"
"\x33\x31\x6d\x3a\x3b\x3c\x1b\x5b\x33\x33\x6d\x3b\x1b\x5b\x33\x31\x6d\x3c\x1b"
"\x5b\x33\x33\x6d\x3a\x1b\x5b\x33\x37\x6d\x60\x20\x20\x2e\x60\x3a\x3a\x60\x20"
"\x20\x20\x20\x20\x0a\x20\x20\x20\x20\x20\x20\x3b\x3c\x2e\x2e\x2c\x2e\x2e\x20"
"\x3a\x3c\x3b\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x60\x3a\x3a\x3a"
"\x60\x20\x20\x20\x20\x20\x60\x3a\x1b\x5b\x33\x33\x6d\x3c\x3b\x1b\x5b\x33\x31"
"\x6d\x3c\x3b\x3c\x1b\x5b\x33\x33\x6d\x3b\x1b\x5b\x33\x31\x6d\x3c\x1b\x5b\x33"
"\x33\x6d\x3b\x1b\x5b\x33\x31\x6d\x3c\x3c\x1b\x5b\x33\x30\x6d\x53\x24\x53\x1b"
"\x5b\x33\x31\x6d\x53\x1b\x5b\x33\x37\x6d\x27\x1b\x5b\x33\x33\x6d\x2e\x3a\x3b"
"\x1b\x5b\x33\x31\x6d\x3c\x3b\x3c\x1b\x5b\x33\x33\x6d\x3a\x1b\x5b\x33\x31\x6d"
"\x3c\x1b\x5b\x33\x33\x6d\x3a\x1b\x5b\x33\x37\x6d\x60\x20\x20\x20\x60\x2e\x3a"
"\x3a\x20\x20\x20\x20\x20\x20\x20\x0a\x20\x20\x2e\x3a\x3a\x3c\x53\x3c\x3a\x60"
"\x3a\x3a\x3a\x3a\x53\x1b\x5b\x33\x32\x6d\x53\x1b\x5b\x33\x37\x6d\x3b\x27\x3a"
"\x3c\x2c\x2e\x20\x20\x20\x20\x20\x20\x20\x20\x20\x60\x3a\x3a\x3a\x3a\x2e\x60"
"\x2e\x60\x2e\x60\x3a\x1b\x5b\x33\x33\x6d\x3c\x3a\x1b\x5b\x33\x31\x6d\x3c\x1b"
"\x5b\x33\x33\x6d\x53\x1b\x5b\x33\x31\x6d\x3c\x1b\x5b\x33\x33\x6d\x3b\x1b\x5b"
"\x33\x31\x6d\x3c\x2c\x1b\x5b\x33\x33\x6d\x3c\x3b\x3a\x1b\x5b\x33\x31\x6d\x2c"
"\x1b\x5b\x33\x33\x6d\x3c\x3b\x1b\x5b\x33\x31\x6d\x3c\x1b\x5b\x33\x33\x6d\x53"
"\x1b\x5b\x33\x31\x6d\x3c\x1b\x5b\x33\x33\x6d\x3b\x3c\x1b\x5b\x33\x37\x6d\x3a"
"\x60\x2e\x60\x2e\x3b\x1b\x5b\x33\x34\x6d\x53\x1b\x5b\x33\x37\x6d\x53\x3f\x27"
"\x20\x20\x20\x20\x20\x20\x20\x20\x0a\x2e\x60\x3a\x60\x3a\x3c\x53\x53\x3b\x3c"
"\x3a\x60\x3a\x3a\x53\x53\x53\x3c\x3a\x60\x3a\x1b\x5b\x33\x30\x6d\x53\x1b\x5b"
"\x33\x37\x6d\x2b\x20\x20\x20\x20\x20\x20\x60\x20\x20\x20\x3a\x1b\x5b\x33\x34"
"\x6d\x53\x1b\x5b\x33\x30\x6d\x53\x46\x24\x1b\x5b\x33\x37\x6d\x2c\x60\x3a\x3a"
"\x3a\x3c\x3a\x3c\x1b\x5b\x33\x33\x6d\x53\x1b\x5b\x33\x37\x6d\x3c\x1b\x5b\x33"
"\x33\x6d\x53\x1b\x5b\x33\x31\x6d\x53\x1b\x5b\x33\x33\x6d\x3b\x1b\x5b\x33\x31"
"\x6d\x53\x3b\x53\x1b\x5b\x33\x33\x6d\x3b\x1b\x5b\x33\x31\x6d\x53\x1b\x5b\x33"
"\x33\x6d\x53\x1b\x5b\x33\x37\x6d\x3c\x1b\x5b\x33\x33\x6d\x53\x1b\x5b\x33\x37"
"\x6d\x3c\x53\x3c\x3a\x3a\x3a\x3a\x3f\x1b\x5b\x33\x30\x6d\x53\x24\x48\x1b\x5b"
"\x33\x37\x6d\x27\x60\x20\x60\x20\x20\x20\x20\x20\x20\x0a\x2e\x60\x3a\x60\x2e"
"\x60\x3a\x60\x2e\x60\x3a\x60\x2e\x60\x3a\x60\x2e\x60\x3a\x60\x2e\x1b\x5b\x33"
"\x30\x6d\x53\x46\x1b\x5b\x33\x37\x6d\x20\x20\x20\x20\x60\x20\x20\x20\x60\x20"
"\x60\x3a\x1b\x5b\x33\x30\x6d\x3c\x46\x46\x46\x1b\x5b\x33\x37\x6d\x3f\x2e\x60"
"\x3a\x60\x3a\x60\x3a\x60\x3a\x60\x3a\x3c\x3a\x60\x3a\x27\x3a\x60\x3a\x60\x3a"
"\x60\x3a\x60\x3b\x1b\x5b\x33\x30\x6d\x53\x46\x48\x46\x1b\x5b\x33\x37\x6d\x27"
"\x20\x60\x20\x60\x20\x60\x20\x20\x20\x20\x0a\x20\x3c\x3b\x3a\x2e\x60\x20\x60"
"\x2e\x60\x20\x60\x2e\x60\x20\x60\x2e\x60\x2c\x53\x1b\x5b\x33\x32\x6d\x53\x1b"
"\x5b\x33\x30\x6d\x53\x1b\x5b\x33\x37\x6d\x20\x20\x20\x20\x20\x20\x20\x20\x20"
"\x20\x20\x60\x20\x60\x3c\x1b\x5b\x33\x30\x6d\x46\x46\x46\x1b\x5b\x33\x34\x6d"
"\x2b\x1b\x5b\x33\x37\x6d\x3a\x20\x60\x20\x60\x20\x60\x2e\x60\x20\x60\x2e\x60"
"\x20\x60\x2e\x60\x20\x60\x20\x60\x2c\x1b\x5b\x33\x30\x6d\x24\x46\x48\x46\x1b"
"\x5b\x33\x37\x6d\x27\x20\x60\x20\x20\x20\x60\x20\x20\x20\x20\x20\x20\x0a\x20"
"\x60\x3a\x1b\x5b\x33\x30\x6d\x53\x24\x1b\x5b\x33\x37\x6d\x53\x53\x53\x3b\x3c"
"\x2c\x60\x2c\x3b\x3b\x53\x3f\x53\x1b\x5b\x33\x30\x6d\x24\x46\x3c\x1b\x5b\x33"
"\x37\x6d\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x60\x20\x60"
"\x3c\x1b\x5b\x33\x30\x6d\x48\x46\x46\x46\x1b\x5b\x33\x37\x6d\x3f\x2e\x60\x20"
"\x60\x20\x60\x20\x60\x20\x60\x20\x60\x20\x60\x20\x3b\x76\x1b\x5b\x33\x30\x6d"
"\x48\x46\x48\x46\x1b\x5b\x33\x37\x6d\x27\x20\x60\x20\x20\x20\x60\x20\x20\x20"
"\x20\x20\x20\x20\x20\x0a\x20\x20\x20\x60\x3c\x1b\x5b\x33\x30\x6d\x46\x24\x1b"
"\x5b\x33\x37\x6d\x53\x53\x53\x53\x53\x53\x1b\x5b\x33\x30\x6d\x53\x24\x53\x46"
"\x46\x46\x1b\x5b\x33\x37\x6d\x27\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20"
"\x20\x20\x20\x20\x20\x20\x20\x20\x60\x3c\x1b\x5b\x33\x30\x6d\x23\x46\x46\x46"
"\x24\x1b\x5b\x33\x37\x6d\x76\x2c\x2c\x20\x2e\x20\x2e\x20\x2c\x2c\x76\x1b\x5b"
"\x33\x30\x6d\x26\x24\x46\x46\x48\x3c\x1b\x5b\x33\x37\x6d\x27\x20\x20\x20\x20"
"\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x0a\x20\x20\x20\x20\x20\x60"
"\x3c\x1b\x5b\x33\x30\x6d\x53\x46\x46\x24\x46\x24\x46\x46\x48\x46\x53\x1b\x5b"
"\x33\x37\x6d\x20\x20\x20\x20\x20\x20\x20\x20\x2e\x60\x20\x60\x2e\x60\x2e\x60"
"\x2e\x60\x2e\x60\x3a\x3a\x3a\x3a\x3a\x1b\x5b\x33\x30\x6d\x2a\x46\x46\x46\x48"
"\x46\x48\x46\x48\x46\x46\x46\x48\x46\x48\x46\x48\x1b\x5b\x33\x37\x6d\x3c\x22"
"\x2e\x60\x2e\x60\x2e\x60\x2e\x60\x2e\x60\x20\x20\x20\x20\x20\x20\x20\x20\x0a"
"\x20\x20\x20\x20\x20\x20\x20\x60\x3a\x1b\x5b\x33\x30\x6d\x48\x46\x46\x46\x48"
"\x46\x46\x46\x1b\x5b\x33\x37\x6d\x27\x20\x20\x20\x60\x20\x60\x2e\x60\x20\x60"
"\x2e\x60\x2e\x60\x3a\x60\x3a\x60\x3a\x60\x3a\x60\x3a\x3a\x3a\x60\x3a\x3c\x3c"
"\x1b\x5b\x33\x30\x6d\x3c\x46\x48\x46\x46\x46\x48\x46\x46\x46\x1b\x5b\x33\x37"
"\x6d\x27\x3a\x60\x3a\x60\x3a\x60\x3a\x60\x2e\x60\x2e\x60\x20\x60\x2e\x60\x20"
"\x60\x20\x60\x20\x20\x0a\x20\x20\x20\x20\x20\x20\x20\x20\x20\x60\x22\x1b\x5b"
"\x33\x30\x6d\x2a\x46\x48\x46\x1b\x5b\x33\x37\x6d\x3f\x20\x20\x20\x60\x20\x60"
"\x2e\x60\x20\x60\x2e\x60\x2e\x60\x3a\x60\x2e\x60\x3a\x60\x3a\x60\x3a\x60\x3a"
"\x60\x3a\x60\x3a\x60\x3a\x60\x3a\x1b\x5b\x33\x30\x6d\x46\x46\x48\x46\x48\x46"
"\x1b\x5b\x33\x37\x6d\x27\x3a\x60\x3a\x60\x3a\x60\x3a\x60\x2e\x60\x3a\x60\x2e"
"\x60\x2e\x60\x20\x60\x2e\x60\x20\x60\x20\x60\x0a\x20\x20\x20\x20\x20\x20\x20"
"\x20\x20\x20\x20\x60\x3c\x1b\x5b\x33\x30\x6d\x48\x46\x46\x1b\x5b\x33\x37\x6d"
"\x2b\x60\x20\x20\x20\x60\x20\x60\x20\x60\x20\x60\x20\x60\x20\x60\x2e\x60\x20"
"\x60\x2e\x60\x20\x60\x2e\x60\x20\x60\x3a\x60\x2e\x60\x3b\x1b\x5b\x33\x30\x6d"
"\x48\x46\x46\x46\x1b\x5b\x33\x37\x6d\x27\x2e\x60\x2e\x60\x20\x60\x2e\x60\x20"
"\x60\x2e\x60\x20\x60\x20\x60\x20\x60\x20\x60\x20\x20\x20\x60\x20\x20\x0a\x20"
"\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x22\x1b\x5b\x33\x30\x6d\x3c"
"\x48\x46\x53\x1b\x5b\x33\x37\x6d\x2b\x3a\x20\x20\x20\x60\x20\x60\x20\x60\x20"
"\x60\x20\x60\x20\x60\x20\x60\x20\x60\x20\x60\x20\x60\x20\x60\x20\x60\x2c\x1b"
"\x5b\x33\x30\x6d\x24\x46\x48\x46\x1b\x5b\x33\x37\x6d\x3f\x20\x60\x20\x60\x20"
"\x60\x20\x60\x20\x60\x20\x60\x20\x60\x20\x60\x20\x60\x20\x60\x20\x20\x20\x60"
"\x20\x20\x20\x20\x0a\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20"
"\x60\x22\x3c\x1b\x5b\x33\x30\x6d\x48\x24\x46\x46\x1b\x5b\x33\x37\x6d\x3e\x2c"
"\x2e\x2e\x20\x20\x20\x20\x20\x20\x20\x20\x60\x20\x20\x20\x60\x20\x20\x20\x3b"
"\x2c\x2c\x1b\x5b\x33\x30\x6d\x24\x53\x46\x46\x46\x1b\x5b\x33\x37\x6d\x27\x22"
"\x20\x20\x60\x20\x20\x20\x60\x20\x20\x20\x60\x20\x20\x20\x20\x20\x20\x20\x20"
"\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x0a\x20\x20\x20\x20\x20\x20\x20\x20"
"\x20\x20\x20\x20\x20\x20\x20\x20\x20\x60\x22\x1b\x5b\x33\x30\x6d\x2a\x3c\x48"
"\x46\x46\x24\x53\x24\x1b\x5b\x33\x37\x6d\x53\x53\x53\x3e\x3e\x3e\x3e\x3e\x53"
"\x3e\x53\x1b\x5b\x33\x30\x6d\x24\x53\x24\x46\x24\x48\x46\x23\x1b\x5b\x33\x37"
"\x6d\x27\x22\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20"
"\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x0a\x20\x20"
"\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20"
"\x60\x60\x22\x3c\x1b\x5b\x33\x30\x6d\x2a\x3c\x3c\x3c\x48\x46\x46\x46\x48\x46"
"\x46\x46\x23\x3c\x1b\x5b\x33\x36\x6d\x3c\x1b\x5b\x33\x37\x6d\x3c\x27\x22\x22"
"\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20"
"\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x0a\x1b"
                                                                "\x5b\x30\x6d";

/* vim: set sw=2 ts=8 cino=>4,n-2,{2,^-2,t0,(0,u0,w1,M1 : */
