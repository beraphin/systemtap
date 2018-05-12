// build/run probes
// Copyright (C) 2005-2016 Red Hat Inc.
//
// This file is part of systemtap, and is free software.  You can
// redistribute it and/or modify it under the terms of the GNU General
// Public License (GPL); either version 2, or (at your option) any
// later version.

#include "config.h"
#include "buildrun.h"
#include "session.h"
#include "util.h"
#include "hash.h"
#include "translate.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>

extern "C" {
#include <signal.h>
#include <sys/wait.h>
#include <pwd.h>
#include <grp.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/resource.h>
}

// A bit of obfuscation for Gentoo's sake.
// We *need* -Werror for stapconf to work correctly.
// https://bugs.gentoo.org/show_bug.cgi?id=522908
#define WERROR ("-W" "error")

using namespace std;

/* Adjust and run make_cmd to build a kernel module. */
static int
run_make_cmd(systemtap_session& s, vector<string>& make_cmd,
             bool null_out=false, bool null_err=false)
{
  assert_no_interrupts();

  // PR14168: we used to unsetenv values here; instead do it via
  // env(1) in make_any_make_cmd().

  // Disable ccache to avoid saving files that will never be reused.
  // (ccache is useless to us, because our compiler commands always
  // include the randomized tmpdir path.)
  // It's not critical if this fails, so the return is ignored.
  (void) setenv("CCACHE_DISABLE", "1", 0);

  if (s.verbose > 2)
    make_cmd.push_back("V=1");
  else if (s.verbose > 1)
    make_cmd.push_back("--no-print-directory");
  else
    {
      make_cmd.push_back("-s");
      make_cmd.push_back("--no-print-directory");
    }

  // Exploit SMP parallelism, if available.
  long smp = thread::hardware_concurrency();
  if (smp <= 0) smp = 1;
  // PR16276: but only if we're not running severely nproc-rlimited
  struct rlimit rlim;
  int rlimit_rc = getrlimit(RLIMIT_NPROC, &rlim);
  const unsigned int severely_limited = smp*30; // WAG at number of gcc+make etc. nested processes
  bool nproc_limited = (rlimit_rc == 0 && (rlim.rlim_max <= severely_limited || 
                                           rlim.rlim_cur <= severely_limited));
  if (smp >= 1 && !nproc_limited)
    make_cmd.push_back("-j" + lex_cast(smp+1));

  if (strverscmp (s.kernel_base_release.c_str(), "2.6.29") < 0)
    {
      // Older kernels, before linux commit #fd54f502841c1, include
      // gratuitous "echo"s in their Makefile.  We need to suppress
      // that with this bluntness.
      null_out = true;
    }

  int rc = stap_system (s.verbose, "kbuild", make_cmd, null_out, null_err);
  if (rc != 0)
    s.set_try_server ();
  return rc;
}

static vector<string>
make_any_make_cmd(systemtap_session& s, const string& dir, const string& target)
{
  string newpath("PATH=/usr/bin:/bin");
  const char *oldpath = getenv("PATH");
  if (oldpath != NULL)
    {
      newpath += ':';
      newpath += oldpath;
    }

  vector<string> make_cmd
    {
      // PR14168: sanitize environment variables for kbuild invocation
      "env", "-uARCH", "-uKBUILD_EXTMOD", "-uCROSS_COMPILE", "-uKBUILD_IMAGE",
      "-uKCONFIG_CONFIG", "-uINSTALL_PATH", "-uLD_LIBRARY_PATH", newpath,

      "make", "-C", s.kernel_build_tree,
      "M=" + dir, // need make-quoting?
      target,

      // PR13847: suppress debuginfo creation by default
      "CONFIG_DEBUG_INFO=",

      // RHBZ1321628: suppress stack validation; expected to be temporary
      "CONFIG_STACK_VALIDATION=",
    };

  // Add architecture, except for old powerpc (RHBZ669082)
  if (s.architecture != "powerpc" ||
      (strverscmp (s.kernel_base_release.c_str(), "2.6.15") >= 0))
    make_cmd.push_back("ARCH=" + s.architecture); // need make-quoting?

  // Add any custom kbuild flags
  make_cmd.insert(make_cmd.end(), s.kbuildflags.begin(), s.kbuildflags.end());

  return make_cmd;
}

static vector<string>
make_make_cmd(systemtap_session& s, const string& dir)
{
  vector<string> mc = make_any_make_cmd(s, dir, "modules");
  if (s.keep_tmpdir)
    {
      string E_source = s.translated_source.substr(s.translated_source.find_last_of("/")+1);
      E_source.at(E_source.length() - 1) = 'i'; // overwrite the last character
      mc.push_back(E_source);
    }
  return mc;
}

static vector<string>
make_make_objs_cmd(systemtap_session& s, const string& dir)
{
  // Kbuild uses these rules to build external modules:
  //
  //   module-dirs := $(addprefix _module_,$(KBUILD_EXTMOD))
  //   modules: $(module-dirs)
  //       @$(kecho) '  Building modules, stage 2.';
  //       $(Q)$(MAKE) -f $(srctree)/scripts/Makefile.modpost
  //
  // So if we're only interested in the stage 1 objects, we can
  // cheat and make only the $(module-dirs) part.
  return make_any_make_cmd(s, dir, "_module_" + dir);
}

static void
output_autoconf(systemtap_session& s, ofstream& o, const char *autoconf_c,
                const char *deftrue, const char *deffalse)
{
  o << "\t";
  if (s.verbose < 4)
    o << "@";
  o << "if $(CHECK_BUILD) $(SYSTEMTAP_RUNTIME)/linux/" << autoconf_c;
  if (s.verbose < 5)
    o << " > /dev/null 2>&1";
  o << "; then ";
  if (deftrue)
    o << "echo \"#define " << deftrue << " 1\"";
  if (deffalse)
    o << "; else echo \"#define " << deffalse << " 1\"";
  o << "; fi >> $@" << endl;
}


void output_exportconf(systemtap_session& s, ofstream& o, const char *symbol,
                     const char *deftrue)
{
  o << "\t";
  if (s.verbose < 4)
    o << "@";
  if (s.kernel_exports.find(symbol) != s.kernel_exports.end())
    o << "echo \"#define " << deftrue << " 1\"";
  o << ">> $@" << endl;
}


void output_dual_exportconf(systemtap_session& s, ofstream& o,
			    const char *symbol1, const char *symbol2,
			    const char *deftrue)
{
  o << "\t";
  if (s.verbose < 4)
    o << "@";
  if (s.kernel_exports.find(symbol1) != s.kernel_exports.end()
      && s.kernel_exports.find(symbol2) != s.kernel_exports.end())
    o << "echo \"#define " << deftrue << " 1\"";
  o << ">> $@" << endl;
}


void output_either_exportconf(systemtap_session& s, ofstream& o,
			      const char *symbol1, const char *symbol2,
			      const char *deftrue)
{
  o << "\t";
  if (s.verbose < 4)
    o << "@";
  if (s.kernel_exports.find(symbol1) != s.kernel_exports.end()
      || s.kernel_exports.find(symbol2) != s.kernel_exports.end())
    o << "echo \"#define " << deftrue << " 1\"";
  o << ">> $@" << endl;
}


static int
compile_dyninst (systemtap_session& s)
{
  const string module = s.tmpdir + "/" + s.module_filename();

  vector<string> cmd
    {
      "gcc", "--std=gnu99", s.translated_source, "-o", module,
      "-fvisibility=hidden", "-O2", "-I" + s.runtime_path, "-D__DYNINST__",
      "-Wall", WERROR, "-Wno-unused", "-Wno-strict-aliasing",
      "-pthread", "-lrt", "-fPIC", "-shared",
    };

  // BZ855981/948279.  Since dyninst/runtime.h includes __sync_* calls,
  // the compiler may generate different code for it depending on -march.
  // For example, if the default is i386, we may get references to auxiliary
  // functions like __sync_add_and_fetch_4, which appear to be defined
  // nowhere.  We hack around this problem thusly:
  if (s.architecture == "i386")
    cmd.push_back("-march=i586");

  for (size_t i = 0; i < s.c_macros.size(); ++i)
    cmd.push_back("-D" + s.c_macros[i]);

  if (s.verbose > 3)
    cmd.insert(cmd.end(), { "-ftime-report", "-Q" });

  // Add any custom kbuild flags
  cmd.insert(cmd.end(), s.kbuildflags.begin(), s.kbuildflags.end());

  int rc = stap_system (s.verbose, cmd);
  if (rc)
    s.set_try_server ();
  return rc;
}


int
compile_pass (systemtap_session& s)
{
  if (s.runtime_usermode_p())
    return compile_dyninst (s);

  int rc = uprobes_pass (s);
  if (rc)
    {
      s.set_try_server ();
      return rc;
    }

  // fill in a quick Makefile
  string makefile_nm = s.tmpdir + "/Makefile";
  ofstream o (makefile_nm.c_str());

  // Create makefile

  // Clever hacks copied from vmware modules
  string superverbose;
  if (s.verbose > 3)
    superverbose = "set -x;";

  string redirecterrors = "> /dev/null 2>&1";
  if (s.verbose > 6)
    redirecterrors = "";

  // Support O= (or KBUILD_OUTPUT) option
  o << "_KBUILD_CFLAGS := $(call flags,KBUILD_CFLAGS)" << endl;

  o << "stap_check_gcc = $(shell " << superverbose
    << " if $(CC) $(1) -S -o /dev/null -xc /dev/null > /dev/null 2>&1; then "
    << "echo \"$(1)\"; else echo \"$(2)\"; fi)" << endl;
  o << "CHECK_BUILD := $(CC) $(NOSTDINC_FLAGS) $(KBUILD_CPPFLAGS) $(CPPFLAGS) "
    << "$(LINUXINCLUDE) $(_KBUILD_CFLAGS) $(CFLAGS_KERNEL) $(EXTRA_CFLAGS) "
    << "$(CFLAGS) -DKBUILD_BASENAME=\\\"" << s.module_name << "\\\" "
    << WERROR << " -S -o /dev/null -xc " << endl;
  o << "stap_check_build = $(shell " << superverbose << " if $(CHECK_BUILD) $(1) "
    << redirecterrors << " ; then echo \"$(2)\"; else echo \"$(3)\"; fi)" << endl;

  o << "SYSTEMTAP_RUNTIME = \"" << s.runtime_path << "\"" << endl;

  // "autoconf" options go here

  // RHBZ 543529: early rhel6 kernels' module-signing kbuild logic breaks out-of-tree modules
  o << "CONFIG_MODULE_SIG := n" << endl;

  string module_cflags = "EXTRA_CFLAGS";
  o << module_cflags << " :=" << endl;

  // XXX: This gruesome hack is needed on some kernels built with separate O=directory,
  // where files like 2.6.27 x86's asm/mach-*/mach_mpspec.h are not found on the cpp path.
  // This could be a bug in arch/x86/Makefile that names
  //      mflags-y += -Iinclude/asm-x86/mach-default
  // but that path does not exist in an O= build tree.
  o << module_cflags << " += -Iinclude2/asm/mach-default" << endl;
  o << module_cflags << " += -I" + s.kernel_build_tree << endl;
  if (s.kernel_source_tree != "")
    o << module_cflags << " += -I" + s.kernel_source_tree << endl;
  for (unsigned i = 0; i < s.kernel_extra_cflags.size(); i++)
    o << module_cflags << " += " + s.kernel_extra_cflags[i] << endl;

  // NB: don't try
  // o << module_cflags << " += -Iusr/include" << endl;
  // since such headers are cleansed of _KERNEL_ pieces that we need

  o << "STAPCONF_HEADER := " << s.tmpdir << "/" << s.stapconf_name << endl;
  o << "$(STAPCONF_HEADER):" << endl;
  o << "\t@> $@" << endl;
  output_autoconf(s, o, "autoconf-hrtimer-rel.c", "STAPCONF_HRTIMER_REL", NULL);
  output_exportconf(s, o, "hrtimer_get_res", "STAPCONF_HRTIMER_GET_RES");
  output_autoconf(s, o, "autoconf-generated-compile.c", "STAPCONF_GENERATED_COMPILE", NULL);
  output_autoconf(s, o, "autoconf-hrtimer-getset-expires.c", "STAPCONF_HRTIMER_GETSET_EXPIRES", NULL);
  output_autoconf(s, o, "autoconf-inode-private.c", "STAPCONF_INODE_PRIVATE", NULL);
  output_autoconf(s, o, "autoconf-inode-rwsem.c", "STAPCONF_INODE_RWSEM", NULL);
  output_autoconf(s, o, "autoconf-constant-tsc.c", "STAPCONF_CONSTANT_TSC", NULL);
  output_autoconf(s, o, "autoconf-ktime-get-real.c", "STAPCONF_KTIME_GET_REAL", NULL);
  output_autoconf(s, o, "autoconf-x86-uniregs.c", "STAPCONF_X86_UNIREGS", NULL);
  output_autoconf(s, o, "autoconf-nameidata.c", "STAPCONF_NAMEIDATA_CLEANUP", NULL);
  output_dual_exportconf(s, o, "unregister_kprobes", "unregister_kretprobes", "STAPCONF_UNREGISTER_KPROBES");
  output_autoconf(s, o, "autoconf-kprobe-symbol-name.c", "STAPCONF_KPROBE_SYMBOL_NAME", NULL);
  output_autoconf(s, o, "autoconf-real-parent.c", "STAPCONF_REAL_PARENT", NULL);
  output_autoconf(s, o, "autoconf-uaccess.c", "STAPCONF_LINUX_UACCESS_H", NULL);
  output_autoconf(s, o, "autoconf-oneachcpu-retry.c", "STAPCONF_ONEACHCPU_RETRY", NULL);
  output_autoconf(s, o, "autoconf-dpath-path.c", "STAPCONF_DPATH_PATH", NULL);
  output_exportconf(s, o, "synchronize_kernel", "STAPCONF_SYNCHRONIZE_KERNEL");
  output_exportconf(s, o, "synchronize_rcu", "STAPCONF_SYNCHRONIZE_RCU");
  output_exportconf(s, o, "synchronize_sched", "STAPCONF_SYNCHRONIZE_SCHED");
  output_autoconf(s, o, "autoconf-task-uid.c", "STAPCONF_TASK_UID", NULL);
  output_autoconf(s, o, "autoconf-from_kuid_munged.c", "STAPCONF_FROM_KUID_MUNGED", NULL);
  output_exportconf(s, o, "get_mm_exe_file", "STAPCONF_GET_MM_EXE_FILE");
  output_dual_exportconf(s, o, "alloc_vm_area", "free_vm_area", "STAPCONF_VM_AREA");
  output_autoconf(s, o, "autoconf-procfs-owner.c", "STAPCONF_PROCFS_OWNER", NULL);
  output_autoconf(s, o, "autoconf-alloc-percpu-align.c", "STAPCONF_ALLOC_PERCPU_ALIGN", NULL);
  output_autoconf(s, o, "autoconf-x86-fs.c", "STAPCONF_X86_FS", NULL);
  output_autoconf(s, o, "autoconf-x86-xfs.c", "STAPCONF_X86_XFS", NULL);
  output_autoconf(s, o, "autoconf-x86-gs.c", "STAPCONF_X86_GS", NULL);
  output_autoconf(s, o, "autoconf-grsecurity.c", "STAPCONF_GRSECURITY", NULL);
  output_autoconf(s, o, "autoconf-trace-printk.c", "STAPCONF_TRACE_PRINTK", NULL);
  output_autoconf(s, o, "autoconf-regset.c", "STAPCONF_REGSET", NULL);
  output_autoconf(s, o, "autoconf-utrace-regset.c", "STAPCONF_UTRACE_REGSET", NULL);
  output_autoconf(s, o, "autoconf-uprobe-get-pc.c", "STAPCONF_UPROBE_GET_PC", NULL);
  output_autoconf(s, o, "autoconf-hlist-4args.c", "STAPCONF_HLIST_4ARGS", NULL);
  output_exportconf(s, o, "tsc_khz", "STAPCONF_TSC_KHZ");
  output_exportconf(s, o, "cpu_khz", "STAPCONF_CPU_KHZ");
  output_exportconf(s, o, "__module_text_address", "STAPCONF_MODULE_TEXT_ADDRESS");
  output_exportconf(s, o, "add_timer_on", "STAPCONF_ADD_TIMER_ON");

  output_dual_exportconf(s, o, "probe_kernel_read", "probe_kernel_write", "STAPCONF_PROBE_KERNEL");
  output_autoconf(s, o, "autoconf-hw_breakpoint_context.c",
		  "STAPCONF_HW_BREAKPOINT_CONTEXT", NULL);
  output_autoconf(s, o, "autoconf-save-stack-trace.c",
                  "STAPCONF_KERNEL_STACKTRACE", NULL);
  output_autoconf(s, o, "autoconf-save-stack-trace-no-bp.c",
                  "STAPCONF_KERNEL_STACKTRACE_NO_BP", NULL);
  output_autoconf(s, o, "autoconf-asm-syscall.c",
		  "STAPCONF_ASM_SYSCALL_H", NULL);
  output_autoconf(s, o, "autoconf-ring_buffer-flags.c", "STAPCONF_RING_BUFFER_FLAGS", NULL);
  output_autoconf(s, o, "autoconf-ring_buffer_lost_events.c", "STAPCONF_RING_BUFFER_LOST_EVENTS", NULL);
  output_autoconf(s, o, "autoconf-ring_buffer_read_prepare.c", "STAPCONF_RING_BUFFER_READ_PREPARE", NULL);
  output_autoconf(s, o, "autoconf-kallsyms-on-each-symbol.c", "STAPCONF_KALLSYMS_ON_EACH_SYMBOL", NULL);
  output_autoconf(s, o, "autoconf-walk-stack.c", "STAPCONF_WALK_STACK", NULL);
  output_autoconf(s, o, "autoconf-stacktrace_ops-warning.c",
                  "STAPCONF_STACKTRACE_OPS_WARNING", NULL);
  output_autoconf(s, o, "autoconf-stacktrace_ops-int-address.c",
                  "STAPCONF_STACKTRACE_OPS_INT_ADDRESS", NULL);
  output_autoconf(s, o, "autoconf-mm-context-vdso.c", "STAPCONF_MM_CONTEXT_VDSO", NULL);
  output_autoconf(s, o, "autoconf-mm-context-vdso-base.c", "STAPCONF_MM_CONTEXT_VDSO_BASE", NULL);
  output_autoconf(s, o, "autoconf-blk-types.c", "STAPCONF_BLK_TYPES", NULL);
  output_autoconf(s, o, "autoconf-perf-structpid.c", "STAPCONF_PERF_STRUCTPID", NULL);
  output_autoconf(s, o, "perf_event_counter_context.c",
		  "STAPCONF_PERF_COUNTER_CONTEXT", NULL);
  output_autoconf(s, o, "perf_probe_handler_nmi.c",
		  "STAPCONF_PERF_HANDLER_NMI", NULL);
  output_exportconf(s, o, "path_lookup", "STAPCONF_PATH_LOOKUP");
  output_exportconf(s, o, "kern_path_parent", "STAPCONF_KERN_PATH_PARENT");
  output_exportconf(s, o, "vfs_path_lookup", "STAPCONF_VFS_PATH_LOOKUP");
  output_exportconf(s, o, "kern_path", "STAPCONF_KERN_PATH");
  output_exportconf(s, o, "proc_create_data", "STAPCONF_PROC_CREATE_DATA");
  output_exportconf(s, o, "PDE_DATA", "STAPCONF_PDE_DATA");
  output_autoconf(s, o, "autoconf-module-sect-attrs.c", "STAPCONF_MODULE_SECT_ATTRS", NULL);

  output_autoconf(s, o, "autoconf-utrace-via-tracepoints.c", "STAPCONF_UTRACE_VIA_TRACEPOINTS", NULL);
  output_autoconf(s, o, "autoconf-task_work-struct.c", "STAPCONF_TASK_WORK_STRUCT", NULL);
  output_autoconf(s, o, "autoconf-vm-area-pte.c", "STAPCONF_VM_AREA_PTE", NULL);
  output_autoconf(s, o, "autoconf-relay-umode_t.c", "STAPCONF_RELAY_UMODE_T", NULL);
  output_autoconf(s, o, "autoconf-relay_buf-per_cpu_ptr.c", "STAPCONF_RELAY_BUF_PER_CPU_PTR", NULL);
  output_autoconf(s, o, "autoconf-fs_supers-hlist.c", "STAPCONF_FS_SUPERS_HLIST", NULL);
  output_autoconf(s, o, "autoconf-compat_sigaction.c", "STAPCONF_COMPAT_SIGACTION", NULL);
  output_autoconf(s, o, "autoconf-netfilter.c", "STAPCONF_NETFILTER_V313", NULL);
  output_autoconf(s, o, "autoconf-netfilter-313b.c", "STAPCONF_NETFILTER_V313B", NULL);
  output_autoconf(s, o, "autoconf-netfilter-4_1.c", "STAPCONF_NETFILTER_V41", NULL);
  output_autoconf(s, o, "autoconf-netfilter-4_4.c", "STAPCONF_NETFILTER_V44", NULL);
  output_autoconf(s, o, "autoconf-smpcall-5args.c", "STAPCONF_SMPCALL_5ARGS", NULL);
  output_autoconf(s, o, "autoconf-smpcall-4args.c", "STAPCONF_SMPCALL_4ARGS", NULL);
  output_autoconf(s, o, "autoconf-sched-mm.c", "STAPCONF_SCHED_MM_H", NULL);
  output_autoconf(s, o, "autoconf-sched-task_stack.c", "STAPCONF_SCHED_TASK_STACK_H", NULL);

  // used by tapset/timestamp_monotonic.stp
  output_autoconf(s, o, "autoconf-cpu-clock.c", "STAPCONF_CPU_CLOCK", NULL);
  output_autoconf(s, o, "autoconf-local-clock.c", "STAPCONF_LOCAL_CLOCK", NULL);

  // used by runtime/uprobe-inode.c
  output_either_exportconf(s, o, "uprobe_register", "register_uprobe",
			   "STAPCONF_UPROBE_REGISTER_EXPORTED");
  output_either_exportconf(s, o, "uprobe_unregister", "unregister_uprobe",
			   "STAPCONF_UPROBE_UNREGISTER_EXPORTED");
  output_autoconf(s, o, "autoconf-old-inode-uprobes.c", "STAPCONF_OLD_INODE_UPROBES", NULL);
  output_autoconf(s, o, "autoconf-inode-uretprobes.c", "STAPCONF_INODE_URETPROBES", NULL);

  // used by tapsets.cxx inode uprobe generated code
  output_exportconf(s, o, "uprobe_get_swbp_addr", "STAPCONF_UPROBE_GET_SWBP_ADDR_EXPORTED");

  // used by runtime/loc2c-runtime.h
  output_exportconf(s, o, "task_user_regset_view", "STAPCONF_TASK_USER_REGSET_VIEW_EXPORTED");

  // used by runtime/stp_utrace.c
  output_exportconf(s, o, "task_work_add", "STAPCONF_TASK_WORK_ADD_EXPORTED");
  output_exportconf(s, o, "wake_up_state", "STAPCONF_WAKE_UP_STATE_EXPORTED");
  output_exportconf(s, o, "try_to_wake_up", "STAPCONF_TRY_TO_WAKE_UP_EXPORTED");
  output_exportconf(s, o, "signal_wake_up_state", "STAPCONF_SIGNAL_WAKE_UP_STATE_EXPORTED");
  output_exportconf(s, o, "signal_wake_up", "STAPCONF_SIGNAL_WAKE_UP_EXPORTED");
  output_exportconf(s, o, "__lock_task_sighand", "STAPCONF___LOCK_TASK_SIGHAND_EXPORTED");

  output_autoconf(s, o, "autoconf-pagefault_disable.c", "STAPCONF_PAGEFAULT_DISABLE", NULL);
  output_exportconf(s, o, "kallsyms_lookup_name", "STAPCONF_KALLSYMS");
  output_autoconf(s, o, "autoconf-uidgid.c", "STAPCONF_LINUX_UIDGID_H", NULL);
  output_exportconf(s, o, "sigset_from_compat", "STAPCONF_SIGSET_FROM_COMPAT_EXPORTED");
  output_exportconf(s, o, "vzalloc", "STAPCONF_VZALLOC");
  output_exportconf(s, o, "vzalloc_node", "STAPCONF_VZALLOC_NODE");
  output_exportconf(s, o, "vmalloc_node", "STAPCONF_VMALLOC_NODE");

  // RHBZ1233912 - s390 temporary workaround for non-atomic udelay()
  output_exportconf(s, o, "udelay_simple", "STAPCONF_UDELAY_SIMPLE_EXPORTED");
  output_autoconf(s, o, "autoconf-udelay_simple.c", "STAPCONF_UDELAY_SIMPLE",
		  NULL);

  output_autoconf(s, o, "autoconf-tracepoint-strings.c", "STAPCONF_TRACEPOINT_STRINGS", NULL);
  output_autoconf(s, o, "autoconf-timerfd.c", "STAPCONF_TIMERFD_H", NULL);

  output_autoconf(s, o, "autoconf-module_layout.c",
		  "STAPCONF_MODULE_LAYOUT", NULL);
  output_autoconf(s, o, "autoconf-mod_kallsyms.c",
		  "STAPCONF_MOD_KALLSYMS", NULL);
  output_exportconf(s, o, "get_user_pages_remote", "STAPCONF_GET_USER_PAGES_REMOTE");
  output_autoconf(s, o, "autoconf-get_user_pages_remote-flags.c",
		  "STAPCONF_GET_USER_PAGES_REMOTE_FLAGS", NULL);
  output_autoconf(s, o, "autoconf-get_user_pages_remote-flags_locked.c",
		  "STAPCONF_GET_USER_PAGES_REMOTE_FLAGS_LOCKED", NULL);
  output_autoconf(s, o, "autoconf-uapi-linux-sched-types.c",
		  "STAPCONF_UAPI_LINUX_SCHED_TYPES", NULL);
  output_autoconf(s, o, "autoconf-bio-bi_opf.c", "STAPCONF_BIO_BI_OPF", NULL);

  // used by runtime/linux/netfilter.c
  output_exportconf(s, o, "nf_register_hook", "STAPCONF_NF_REGISTER_HOOK");

  // used by tapset/linux/ioblock.stp
  output_exportconf(s, o, "disk_get_part", "STAPCONF_DISK_GET_PART");

  o << module_cflags << " += -include $(STAPCONF_HEADER)" << endl;

  for (unsigned i=0; i<s.c_macros.size(); i++)
    o << "EXTRA_CFLAGS += -D " << lex_cast_qstring(s.c_macros[i]) << endl; // XXX right quoting?

  if (s.verbose > 3)
    o << "EXTRA_CFLAGS += -ftime-report -Q" << endl;

  // XXX: unfortunately, -save-temps can't work since linux kbuild cwd
  // is not writable.
  //
  // if (s.keep_tmpdir)
  // o << "CFLAGS += -fverbose-asm -save-temps" << endl;

  // Kernels can be compiled with CONFIG_CC_OPTIMIZE_FOR_SIZE to select
  // -Os, otherwise -O2 is the default.
  o << "EXTRA_CFLAGS += -freorder-blocks" << endl; // improve on -Os

  // Generate eh_frame for self-backtracing
  o << "EXTRA_CFLAGS += -fasynchronous-unwind-tables" << endl;

  // We used to allow the user to override default optimization when so
  // requested by adding a -O[0123s] so they could determine the
  // time/space/speed tradeoffs themselves, but we cannot guantantee that
  // the (un)optimized code actually compiles and/or generates functional
  // code, so we had to remove it.
  // o << "EXTRA_CFLAGS += " << s.gcc_flags << endl; // Add -O[0123s]

  // o << "CFLAGS += -fno-unit-at-a-time" << endl;

  // 256^W512 bytes should be enough for anybody
  // XXX this doesn't validate varargs, per gcc bug #41633
  o << "EXTRA_CFLAGS += $(call cc-option,-Wframe-larger-than=512)" << endl;

  // gcc 5.0.0-0.13.fc23 ipa-icf seems to consume gigacpu on stap-generated code
  o << "EXTRA_CFLAGS += $(call cc-option,-fno-ipa-icf)" << endl;

  // Assumes linux 2.6 kbuild
  o << "EXTRA_CFLAGS += -Wno-unused " << WERROR << endl;
  #if CHECK_POINTER_ARITH_PR5947
  o << "EXTRA_CFLAGS += -Wpointer-arith" << endl;
  #endif
  o << "EXTRA_CFLAGS += -I\"" << s.runtime_path << "\"" << endl;
  // XXX: this may help ppc toc overflow
  // o << "CFLAGS := $(subst -Os,-O2,$(CFLAGS)) -fminimal-toc" << endl;
  o << "obj-m := " << s.module_name << ".o" << endl;

  // print out all the auxiliary source (->object) file names
  o << s.module_name << "-y := ";
  for (unsigned i=0; i<s.auxiliary_outputs.size(); i++)
    {
      if (s.auxiliary_outputs[i]->trailer_p) continue;
      string srcname = s.auxiliary_outputs[i]->filename;
      assert (srcname != "" && srcname.rfind('/') != string::npos);
      string objname = srcname.substr(srcname.rfind('/')+1); // basename
      assert (objname != "" && objname[objname.size()-1] == 'c');
      objname[objname.size()-1] = 'o'; // now objname
      o << " " + objname;
    }
  // and once again, for the translated_source file.  It can't simply
  // be named MODULENAME.c, since kbuild doesn't allow a foo.ko file
  // consisting of multiple .o's to have foo.o/foo.c as a source.
  // (It uses ld -r -o foo.o EACH.o EACH.o).
  {
    string srcname = s.translated_source;
    assert (srcname != "" && srcname.rfind('/') != string::npos);
    string objname = srcname.substr(srcname.rfind('/')+1); // basename
    assert (objname != "" && objname[objname.size()-1] == 'c');
    objname[objname.size()-1] = 'o'; // now objname
    o << " " + objname;
  }
  // and once again, for the trailer type auxiliary outputs.
  for (unsigned i=0; i<s.auxiliary_outputs.size(); i++)
    {
      if (! s.auxiliary_outputs[i]->trailer_p) continue;
      string srcname = s.auxiliary_outputs[i]->filename;
      assert (srcname != "" && srcname.rfind('/') != string::npos);
      string objname = srcname.substr(srcname.rfind('/')+1); // basename
      assert (objname != "" && objname[objname.size()-1] == 'c');
      objname[objname.size()-1] = 'o'; // now objname
      o << " " + objname;
    }
  o << endl;

  // add all stapconf dependencies
  o << s.translated_source << ": $(STAPCONF_HEADER)" << endl;
  for (unsigned i=0; i<s.auxiliary_outputs.size(); i++)
    o << s.auxiliary_outputs[i]->filename << ": $(STAPCONF_HEADER)" << endl;  

  o.close ();

  // Generate module directory pathname and make sure it exists.
  string module_dir = s.kernel_build_tree;
  string module_dir_makefile = module_dir + "/Makefile";
  struct stat st;
  rc = stat(module_dir_makefile.c_str(), &st);
  if (rc != 0)
    {
        clog << _F("Checking \" %s \" failed with error: %s\nEnsure kernel development headers & makefiles are installed.",
                   module_dir_makefile.c_str(), strerror(errno)) << endl;
	s.set_try_server ();
	return rc;
    }

  // Run make
  vector<string> make_cmd = make_make_cmd(s, s.tmpdir);
  rc = run_make_cmd(s, make_cmd);
  if (rc)
    s.set_try_server ();
  return rc;
}

/*
 * If uprobes was built as part of the kernel build (either built-in
 * or as a module), the uprobes exports should show up.  This is to be
 * as distinct from the stap-built uprobes.ko from the runtime.
 */
static bool
kernel_built_uprobes (systemtap_session& s)
{
  if (s.runtime_usermode_p())
    return true; // sort of, via dyninst

  // see also tapsets.cxx:kernel_supports_inode_uprobes()
  return ((s.kernel_config["CONFIG_ARCH_SUPPORTS_UPROBES"] == "y" && s.kernel_config["CONFIG_UPROBES"] == "y") ||
          (s.kernel_exports.find("unregister_uprobe") != s.kernel_exports.end()));
}

static int
make_uprobes (systemtap_session& s)
{
  if (s.verbose > 1)
    clog << _("Pass 4, preamble: (re)building SystemTap's version of uprobes.")
	 << endl;

  // create a subdirectory for the uprobes module
  string dir(s.tmpdir + "/uprobes");
  if (create_dir(dir.c_str()) != 0)
    {
      s.print_warning("failed to create directory for build uprobes.");
      s.set_try_server ();
      return 1;
    }

  // create a simple Makefile
  string makefile(dir + "/Makefile");
  ofstream omf(makefile.c_str());
  omf << "obj-m := uprobes.o" << endl;
  // RHBZ 655231: later rhel6 kernels' module-signing kbuild logic breaks out-of-tree modules
  omf << "CONFIG_MODULE_SIG := n" << endl;
  omf.close();

  // create a simple #include-chained source file
  string runtimesourcefile(s.runtime_path + "/linux/uprobes/uprobes.c");
  string sourcefile(dir + "/uprobes.c");
  ofstream osrc(sourcefile.c_str());
  osrc << "#include \"" << runtimesourcefile << "\"" << endl;

  // pass --modinfo k=v to uprobes build too
  for (unsigned i = 0; i < s.modinfos.size(); i++)
    {
      const string& mi = s.modinfos[i];
      size_t loc = mi.find('=');
      string tag = mi.substr (0, loc);
      string value = mi.substr (loc+1);
      osrc << "MODULE_INFO(" << tag << "," << lex_cast_qstring(value) << ");" << endl;
    }

  osrc.close();

  // make the module
  vector<string> make_cmd = make_make_cmd(s, dir);
  int rc = run_make_cmd(s, make_cmd);
  if (!rc && !copy_file(dir + "/Module.symvers",
                        s.tmpdir + "/Module.symvers"))
    rc = -1;

  if (s.verbose > 1)
    clog << _("uprobes rebuild exit code: ") << rc << endl;
  if (rc)
    s.set_try_server ();
  else
    s.uprobes_path = dir + "/uprobes.ko";
  return rc;
}

static bool
get_cached_uprobes(systemtap_session& s)
{
  s.uprobes_hash = s.use_cache ? find_uprobes_hash(s) : "";
  if (!s.uprobes_hash.empty())
    {
      // NB: We always put uprobes.ko in its own directory, especially so
      // stap-serverd can more easily locate it.
      string dir(s.tmpdir + "/uprobes");
      if (create_dir(dir.c_str()) != 0)
        return false;

      string cacheko = s.uprobes_hash + ".ko";
      string tmpko = dir + "/uprobes.ko";

      // The symvers file still needs to go in the script module's directory.
      string cachesyms = s.uprobes_hash + ".symvers";
      string tmpsyms = s.tmpdir + "/Module.symvers";

      if (get_file_size(cacheko) > 0 && copy_file(cacheko, tmpko) &&
          get_file_size(cachesyms) > 0 && copy_file(cachesyms, tmpsyms))
        {
          s.uprobes_path = tmpko;
          return true;
        }
    }
  return false;
}

static void
set_cached_uprobes(systemtap_session& s)
{
  if (s.use_cache && !s.uprobes_hash.empty())
    {
      string cacheko = s.uprobes_hash + ".ko";
      string tmpko = s.tmpdir + "/uprobes/uprobes.ko";
      copy_file(tmpko, cacheko);

      string cachesyms = s.uprobes_hash + ".symvers";
      string tmpsyms = s.tmpdir + "/uprobes/Module.symvers";
      copy_file(tmpsyms, cachesyms);
    }
}

int
uprobes_pass (systemtap_session& s)
{
  if (!s.need_uprobes || kernel_built_uprobes(s))
    return 0;

  if (s.kernel_config["CONFIG_UTRACE"] != string("y"))
    {
      clog << _("user-space process-tracking facilities not available [man error::process-tracking]") << endl;
      s.set_try_server ();
      return 1;
    }

  /*
   * We need to use the version of uprobes that comes with SystemTap.  Try to
   * get it from the cache first.  If not found, build it and try to save it to
   * the cache for future reuse.
   */
  int rc = 0;
  if (!get_cached_uprobes(s))
    {
      rc = make_uprobes(s);
      if (!rc)
        set_cached_uprobes(s);
    }
  if (rc)
    s.set_try_server ();
  return rc;
}

static vector<string>
make_dyninst_run_command (systemtap_session& s, const string& remotedir,
			  const string&)
{
  vector<string> cmd { getenv("SYSTEMTAP_STAPDYN") ?: BINDIR "/stapdyn" };

  // use slightly less verbosity
  if (s.verbose > 0)
    cmd.insert(cmd.end(), s.verbose - 1, "-v");

  if (s.suppress_warnings)
    cmd.push_back("-w");

  if (!s.cmd.empty())
    cmd.insert(cmd.end(), { "-c", s.cmd });

  if (s.target_pid)
    cmd.insert(cmd.end(), { "-x", lex_cast(s.target_pid) });

  if (!s.output_file.empty())
    cmd.insert(cmd.end(), { "-o", s.output_file });

  if (s.color_mode != s.color_auto)
    {
      auto mode = s.color_mode == s.color_always ? "always" : "never";
      cmd.insert(cmd.end(), { "-C", mode });
    }

  cmd.push_back((remotedir.empty() ? s.tmpdir : remotedir)
		+ "/" + s.module_filename());

  // add module arguments
  cmd.insert(cmd.end(), s.globalopts.begin(), s.globalopts.end());

  return cmd;
}

static vector<string>
make_kernel_run_command (systemtap_session& s, const string& remotedir,
			 const string& version)
{
  // for now, just spawn staprun
  vector<string> cmd { getenv("SYSTEMTAP_STAPRUN") ?: BINDIR "/staprun" };

  // use slightly less verbosity
  if (s.verbose > 0)
    cmd.insert(cmd.end(), s.verbose - 1, "-v");

  if (s.suppress_warnings)
    cmd.push_back("-w");

  if (!s.output_file.empty())
    cmd.insert(cmd.end(), { "-o", s.output_file });

  if (!s.cmd.empty())
    cmd.insert(cmd.end(), { "-c", s.cmd });

  if (s.target_pid)
    cmd.insert(cmd.end(), { "-t", lex_cast(s.target_pid) });

  if (s.target_namespaces_pid)
    cmd.insert(cmd.end(), { "-N", lex_cast(s.target_namespaces_pid) });

  if (s.buffer_size)
    cmd.insert(cmd.end(), { "-b", lex_cast(s.buffer_size) });

  if (s.read_stdin)
    cmd.insert(cmd.end(), "-i");

  if (s.need_uprobes && !kernel_built_uprobes(s))
    {
      string opt_u = "-u";
      if (!s.uprobes_path.empty() &&
          strverscmp("1.4", version.c_str()) <= 0)
        {
          if (remotedir.empty())
            opt_u.append(s.uprobes_path);
          else
            opt_u.append(remotedir + "/" + basename(s.uprobes_path.c_str()));
        }
      cmd.push_back(opt_u);
    }

  if (s.load_only)
    cmd.push_back(s.output_file.empty() ? "-L" : "-D");

  // Note that if this system requires signed modules, we can't rename
  // it after it has been signed.
  if (!s.modname_given && (strverscmp("1.6", version.c_str()) <= 0)
      && s.mok_fingerprints.empty())
    cmd.push_back("-R");

  if (!s.size_option.empty())
    cmd.insert(cmd.end(), { "-S", s.size_option });

  if (s.color_mode != s.color_auto)
    {
      auto mode = s.color_mode == s.color_always ? "always" : "never";
      cmd.insert(cmd.end(), { "-C", mode });
    }

  if (s.monitor)
    cmd.insert(cmd.end(), { "-M", lex_cast(s.monitor_interval) });

  cmd.push_back((remotedir.empty() ? s.tmpdir : remotedir)
                        + "/" + s.module_filename());

  // add module arguments
  cmd.insert(cmd.end(), s.globalopts.begin(), s.globalopts.end());

  return cmd;
}

static vector<string>
make_bpf_run_command (systemtap_session& s, const string& remotedir,
		      const string&)
{
  vector<string> cmd;
  cmd.push_back(getenv("SYSTEMTAP_STAPBPF") ?: BINDIR "/stapbpf");

  for (unsigned i=1; i<s.verbose; i++)
    cmd.push_back("-v");
  if (s.suppress_warnings)
    cmd.push_back("-w");

  if (!s.output_file.empty())
    {
      cmd.push_back("-o");
      cmd.push_back(s.output_file);
    }

  cmd.push_back((remotedir.empty() ? s.tmpdir : remotedir)
		+ "/" + s.module_filename());

  // FIXME add module arguments
  // ??? Maybe insert these via BEGIN probe.
  // cmd.insert(cmd.end(), s.globalopts.begin(), s.globalopts.end());

  return cmd;
}

vector<string>
make_run_command (systemtap_session& s, const string& remotedir,
                  const string& version)
{
  switch (s.runtime_mode)
    {
    case systemtap_session::kernel_runtime:
      return make_kernel_run_command (s, remotedir, version);
    case systemtap_session::dyninst_runtime:
      return make_dyninst_run_command(s, remotedir, version);
    case systemtap_session::bpf_runtime:
      return make_bpf_run_command(s, remotedir, version);
    default:
      abort();
    }
}

// Build tiny kernel modules to query tracepoints.
// Given a (header-file -> test-contents) map, compile them ASAP, and return
// a (header-file -> obj-filename) map.

map<string,string>
make_tracequeries(systemtap_session& s, const map<string,string>& contents)
{
  static unsigned tick = 0;
  string basename("tracequery_kmod_" + lex_cast(++tick));
  map<string,string> objs;

  // create a subdirectory for the module
  string dir(s.tmpdir + "/" + basename);
  if (create_dir(dir.c_str()) != 0)
    {
      s.print_warning("failed to create directory for querying tracepoints.");
      s.set_try_server ();
      return objs;
    }

  // create a simple Makefile
  string makefile(dir + "/Makefile");
  ofstream omf(makefile.c_str());
  // force debuginfo generation, and relax implicit functions
  omf << "EXTRA_CFLAGS := -g -Wno-implicit-function-declaration " << WERROR << endl;
  // RHBZ 655231: later rhel6 kernels' module-signing kbuild logic breaks out-of-tree modules
  omf << "CONFIG_MODULE_SIG := n" << endl;

  // PR18389: disable GCC's Identical Code Folding, since the stubs may look identical
  omf << "EXTRA_CFLAGS += $(call cc-option,-fno-ipa-icf)" << endl;

  omf << "EXTRA_CFLAGS += -I" + s.kernel_build_tree << endl;
  if (s.kernel_source_tree != "")
    omf << "EXTRA_CFLAGS += -I" + s.kernel_source_tree << endl;
  for (unsigned i = 0; i < s.kernel_extra_cflags.size(); i++)
    omf << "EXTRA_CFLAGS += " + s.kernel_extra_cflags[i] << endl;

  omf << "obj-m := " << endl;
  // write out each header-specific source file into a separate file
  for (map<string,string>::const_iterator it = contents.begin(); it != contents.end(); it++)
    {
      string sbasename = basename + "_" + lex_cast(++tick); // suffixed

      // write out source code
      string srcname = dir + "/" + sbasename + ".c";
      string src = it->second;
      ofstream osrc(srcname.c_str());
      osrc << src;
      osrc.close();

      if (s.verbose > 2)
        clog << _F("Processing tracepoint header %s with query %s", 
                   it->first.c_str(), srcname.c_str())
             << endl;

      // arrange to build it
      omf << "obj-m += " + sbasename + ".o" << endl; // NB: without <dir> prefix
      objs[it->first] = dir + "/" + sbasename + ".o";
    }
  omf.close();

  // make the module
  vector<string> make_cmd = make_make_objs_cmd(s, dir);
  make_cmd.push_back ("-i"); // ignore errors, give rc 0 even in case of tracepoint header nits
  bool quiet = (s.verbose < 4);
  int rc = run_make_cmd(s, make_cmd, quiet, quiet);
  if (rc)
    s.set_try_server ();

  // Sometimes we fail a tracequery due to PR9993 / PR11649 type
  // kernel trace header problems.  In this case, due to PR12729, we
  // used to get a lovely "Warning: make exited with status: 2" but no
  // other useful diagnostic.  -vvvv would let a user see what's up,
  // but the user can't fix the problem even with that.

  return objs;
}


// Build a tiny kernel module to query type information
static int
make_typequery_kmod(systemtap_session& s, const vector<string>& headers, string& name)
{
  static unsigned tick = 0;
  string basename("typequery_kmod_" + lex_cast(++tick));

  // create a subdirectory for the module
  string dir(s.tmpdir + "/" + basename);
  if (create_dir(dir.c_str()) != 0)
    {
      s.print_warning("failed to create directory for querying types.");
      s.set_try_server ();
      return 1;
    }

  name = dir + "/" + basename + ".ko";

  // create a simple Makefile
  string makefile(dir + "/Makefile");
  ofstream omf(makefile.c_str());
  omf << "EXTRA_CFLAGS := -g -fno-eliminate-unused-debug-types" << endl;

  // RHBZ 655231: later rhel6 kernels' module-signing kbuild logic breaks out-of-tree modules
  omf << "CONFIG_MODULE_SIG := n" << endl;

  // NB: We use -include instead of #include because that gives us more power.
  // Using #include searches relative to the source's path, which in this case
  // is /tmp/..., so that's not helpful.  Using -include will search relative
  // to the cwd, which will be the kernel build root.  This means if you have a
  // full kernel build tree, it's possible to get at types that aren't in the
  // normal include path, e.g.:
  //    @cast(foo, "bsd_acct_struct", "kernel<kernel/acct.c>")->...
  omf << "CFLAGS_" << basename << ".o :=";
  for (size_t i = 0; i < headers.size(); ++i)
    omf << " -include " << lex_cast_qstring(headers[i]); // XXX right quoting?
  omf << endl;

  omf << "obj-m := " + basename + ".o" << endl;
  omf.close();

  // create our empty source file
  string source(dir + "/" + basename + ".c");
  ofstream osrc(source.c_str());
  osrc.close();

  // make the module
  vector<string> make_cmd = make_make_cmd(s, dir);
  bool quiet = (s.verbose < 4);
  int rc = run_make_cmd(s, make_cmd, quiet, quiet);
  if (rc)
    s.set_try_server ();
  return rc;
}


// Build a tiny user module to query type information
static int
make_typequery_umod(systemtap_session& s, const vector<string>& headers, string& name)
{
  static unsigned tick = 0;

  name = s.tmpdir + "/typequery_umod_" + lex_cast(++tick) + ".so";

  // make the module
  //
  // NB: As with kmod, using -include makes relative paths more useful.  The
  // cwd in this case will be the cwd of stap itself though, which may be
  // trickier to deal with.  It might be better to "cd `dirname $script`"
  // first...
  vector<string> cmd
    {
      "gcc", "-shared", "-g", "-fno-eliminate-unused-debug-types",
      "-xc", "/dev/null", "-o", name,
    };
  for (size_t i = 0; i < headers.size(); ++i)
    cmd.insert(cmd.end(), { "-include", headers[i] });

  bool quiet = (s.verbose < 4);
  int rc = stap_system (s.verbose, cmd, quiet, quiet);
  if (rc)
    s.set_try_server ();
  return rc;
}


int
make_typequery(systemtap_session& s, string& module)
{
  int rc;
  string new_module;
  vector<string> headers;
  bool kernel = startswith(module, "kernel");

  for (size_t end, i = kernel ? 6 : 0; i < module.size(); i = end + 1)
    {
      if (module[i] != '<')
        return -1;
      end = module.find('>', ++i);
      if (end == string::npos)
        return -1;
      string header = module.substr(i, end - i);
      vector<string> matches;
      if (regexp_match(header, "^[a-zA-Z0-9/_.+-]+$", matches))
        s.print_warning("skipping malformed @cast header \""+ header + "\"");
      else
        headers.push_back(header);
    }
  if (headers.empty())
    return -1;

  if (kernel)
      rc = make_typequery_kmod(s, headers, new_module);
  else
      rc = make_typequery_umod(s, headers, new_module);

  if (!rc)
    module = new_module;

  return rc;
}

/* vim: set sw=2 ts=8 cino=>4,n-2,{2,^-2,t0,(0,u0,w1,M1 : */
