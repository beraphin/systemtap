// -*- C++ -*-
// Copyright (C) 2014 Red Hat Inc.
//
// This file is part of systemtap, and is free software.  You can
// redistribute it and/or modify it under the terms of the GNU General
// Public License (GPL); either version 2, or (at your option) any
// later version.

#ifndef CMDLINE_H
#define CMDLINE_H 1
extern "C" {
#include <getopt.h>
}

// NB: when adding new options, consider very carefully whether they
// should be restricted from stap clients (after --client-options)!
// NB: The values of these enumerators must not conflict with the values of ordinary
// characters, since those are returned by getopt_long for short options.
enum {
  LONG_OPT_VERBOSE_PASS = 256,
  LONG_OPT_SKIP_BADVARS,
  LONG_OPT_UNPRIVILEGED,
  LONG_OPT_CLIENT_OPTIONS,
  LONG_OPT_HELP,
  LONG_OPT_DISABLE_CACHE,
  LONG_OPT_POISON_CACHE,
  LONG_OPT_CLEAN_CACHE,
  LONG_OPT_COMPATIBLE,
  LONG_OPT_LDD,
  LONG_OPT_USE_SERVER,
  LONG_OPT_LIST_SERVERS,
  LONG_OPT_TRUST_SERVERS,
  LONG_OPT_USE_HTTP_SERVER,
  LONG_OPT_ALL_MODULES,
  LONG_OPT_REMOTE,
  LONG_OPT_CHECK_VERSION,
  LONG_OPT_USE_SERVER_ON_ERROR,
  LONG_OPT_VERSION,
  LONG_OPT_REMOTE_PREFIX,
  LONG_OPT_TMPDIR,
  LONG_OPT_DOWNLOAD_DEBUGINFO,
  LONG_OPT_DUMP_PROBE_TYPES,
  LONG_OPT_DUMP_PROBE_ALIASES,
  LONG_OPT_DUMP_FUNCTIONS,
  LONG_OPT_PRIVILEGE,
  LONG_OPT_SUPPRESS_HANDLER_ERRORS,
  LONG_OPT_MODINFO,
  LONG_OPT_RLIMIT_AS,
  LONG_OPT_RLIMIT_CPU,
  LONG_OPT_RLIMIT_NPROC,
  LONG_OPT_RLIMIT_STACK,
  LONG_OPT_RLIMIT_FSIZE,
  LONG_OPT_SYSROOT,
  LONG_OPT_SYSENV,
  LONG_OPT_SUPPRESS_TIME_LIMITS,
  LONG_OPT_RUNTIME,
  LONG_OPT_RUNTIME_DYNINST,
  LONG_OPT_BENCHMARK_SDT,
  LONG_OPT_BENCHMARK_SDT_LOOPS,
  LONG_OPT_BENCHMARK_SDT_THREADS,
  LONG_OPT_COLOR_ERRS,
  LONG_OPT_PROLOGUE_SEARCHING,
  LONG_OPT_SAVE_UPROBES,
  LONG_OPT_TARGET_NAMESPACES,
  LONG_OPT_MONITOR,
  LONG_OPT_INTERACTIVE,
};

// NB: when adding new options, consider very carefully whether they
// should be restricted from stap clients (after --client-options)!
#define STAP_SHORT_OPTIONS "hVvtp:I:e:E:o:R:r:a:m:kgPc:x:D:bs:uqiwl:d:L:FS:B:J:jWG:T:"

extern struct option stap_long_options[];

#endif // CMDLINE_H
