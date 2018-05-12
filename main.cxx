// systemtap translator/driver
// Copyright (C) 2005-2016 Red Hat Inc.
// Copyright (C) 2005 IBM Corp.
// Copyright (C) 2006 Intel Corporation.
//
// This file is part of systemtap, and is free software.  You can
// redistribute it and/or modify it under the terms of the GNU General
// Public License (GPL); either version 2, or (at your option) any
// later version.

#include "config.h"
#include "staptree.h"
#include "parse.h"
#include "elaborate.h"
#include "translate.h"
#include "buildrun.h"
#include "session.h"
#include "hash.h"
#include "cache.h"
#include "util.h"
#include "coveragedb.h"
#include "rpm_finder.h"
#include "task_finder.h"
#include "csclient.h"
#include "client-nss.h"
#include "remote.h"
#include "tapsets.h"
#include "setupdwfl.h"
#ifdef HAVE_LIBREADLINE
#include "interactive.h"
#endif
#include "bpf.h"

#if ENABLE_NLS
#include <libintl.h>
#include <locale.h>
#endif

#include "stap-probe.h"

#include <cstdlib>
#include <thread>
#include <algorithm>

extern "C" {
#include <glob.h>
#include <unistd.h>
#include <signal.h>
#include <sys/utsname.h>
#include <sys/times.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wordexp.h>
#include <ftw.h>
}

using namespace std;

static void
uniq_list(list<string>& l)
{
  set<string> s;
  list<string>::iterator i = l.begin();
  while (i != l.end())
    if (s.insert(*i).second)
      ++i;
    else
      i = l.erase(i);
}

static void
printscript(systemtap_session& s, ostream& o)
{
  if (s.dump_mode == systemtap_session::dump_matched_probes ||
      s.dump_mode == systemtap_session::dump_matched_probes_vars)
    {
      // We go through some heroic measures to produce clean output.
      // Record the alias and probe pointer as <name, set<derived_probe *> >
      map<string,set<derived_probe *> > probe_list;

      // Pre-process the probe alias
      for (unsigned i=0; i<s.probes.size(); i++)
        {
          assert_no_interrupts();

          derived_probe* p = s.probes[i];
          vector<probe*> chain;
          p->collect_derivation_chain (chain);

          if (s.verbose > 2) {
            p->printsig(cerr); cerr << endl;
            cerr << "chain[" << chain.size() << "]:" << endl;
            for (unsigned j=0; j<chain.size(); j++)
              {
                cerr << "  [" << j << "]: " << endl;
                cerr << "\tlocations[" << chain[j]->locations.size() << "]:" << endl;
                for (unsigned k=0; k<chain[j]->locations.size(); k++)
                  {
                    cerr << "\t  [" << k << "]: ";
                    chain[j]->locations[k]->print(cerr);
                    cerr << endl;
                  }
                const probe_alias *a = chain[j]->get_alias();
                if (a)
                  {
                    cerr << "\taliases[" << a->alias_names.size() << "]:" << endl;
                    for (unsigned k=0; k<a->alias_names.size(); k++)
                      {
                        cerr << "\t  [" << k << "]: ";
                        a->alias_names[k]->print(cerr);
                        cerr << endl;
                      }
                  }
              }
          }

          const string& pp = lex_cast(*p->script_location());

          // PR16730: We should only list probes that can be traced back to the
          // user's spec, not any auxiliary probes in the tapsets.
          // Also, do not want to the probes that are from the additional
          // scripts (-E SCRIPT) to be listed.
          if (!s.is_primary_probe(p))
            continue;

          // Now duplicate-eliminate.  An alias may have expanded to
          // several actual derived probe points, but we only want to
          // print the alias head name once.
          probe_list[pp].insert(p);
        }

      // print probe name and variables if there
      for (map<string, set<derived_probe *> >::iterator it=probe_list.begin(); it!=probe_list.end(); ++it)
        {
          // probe name or alias
          if (s.dump_mode == systemtap_session::dump_matched_probes_vars && isatty(STDOUT_FILENO))
            o << s.colorize(it->first, "source");
          else
            o << it->first;

          // Print the locals and arguments for -L mode only
          if (s.dump_mode == systemtap_session::dump_matched_probes_vars)
            {
              map<string,unsigned> var_count; // format <"name:type",count>
              map<string,unsigned> arg_count;
              list<string> var_list;
              list<string> arg_list;
              // traverse set<derived_probe *> to collect all locals and arguments
              for (set<derived_probe *>::iterator ix=it->second.begin(); ix!=it->second.end(); ++ix)
                {
                  derived_probe* p = *ix;
                  // collect available locals of the probe
                  for (unsigned j=0; j<p->locals.size(); j++)
                    {
                      stringstream tmps;
                      vardecl* v = p->locals[j];
                      v->printsig (tmps);
                      var_count[tmps.str()]++;
		      var_list.push_back(tmps.str());
                    }
                  // collect arguments of the probe if there
                  list<string> arg_set;
                  p->getargs(arg_set);
                  for (list<string>::iterator ia=arg_set.begin(); ia!=arg_set.end(); ++ia) {
                    arg_count[*ia]++;
                    arg_list.push_back(*ia);
		  }
                }

	      uniq_list(arg_list);
	      uniq_list(var_list);

              // print the set-intersection only
              for (list<string>::iterator ir=var_list.begin(); ir!=var_list.end(); ++ir)
                if (var_count.find(*ir)->second == it->second.size()) // print locals
                  o << " " << *ir;
              for (list<string>::iterator ir=arg_list.begin(); ir!=arg_list.end(); ++ir)
                if (arg_count.find(*ir)->second == it->second.size()) // print arguments
                  o << " " << *ir;
            }
          o << endl;
        }
    }
  else
    {
      if (s.embeds.size() > 0)
        o << _("# global embedded code") << endl;
      for (unsigned i=0; i<s.embeds.size(); i++)
        {
          assert_no_interrupts();
          embeddedcode* ec = s.embeds[i];
          ec->print (o);
          o << endl;
        }

      if (s.globals.size() > 0)
        o << _("# globals") << endl;
      for (unsigned i=0; i<s.globals.size(); i++)
        {
          assert_no_interrupts();
          vardecl* v = s.globals[i];
          v->printsig (o);
          if (s.verbose && v->init)
            {
              o << " = ";
              v->init->print(o);
            }
          o << endl;
        }

      if (s.functions.size() > 0)
        o << _("# functions") << endl;
      for (map<string,functiondecl*>::iterator it = s.functions.begin(); it != s.functions.end(); it++)
        {
          assert_no_interrupts();
          functiondecl* f = it->second;
          f->printsig (o);
          o << endl;
          if (f->locals.size() > 0)
            o << _("  # locals") << endl;
          for (unsigned j=0; j<f->locals.size(); j++)
            {
              vardecl* v = f->locals[j];
              o << "  ";
              v->printsig (o);
              o << endl;
            }
          if (s.verbose)
            {
              f->body->print (o);
              o << endl;
            }
        }

      if (s.probes.size() > 0)
        o << _("# probes") << endl;
      for (unsigned i=0; i<s.probes.size(); i++)
        {
          assert_no_interrupts();
          derived_probe* p = s.probes[i];
          p->printsig (o);
          o << endl;
          if (p->locals.size() > 0)
            o << _("  # locals") << endl;
          for (unsigned j=0; j<p->locals.size(); j++)
            {
              vardecl* v = p->locals[j];
              o << "  ";
              v->printsig (o);
              o << endl;
            }
          if (s.verbose)
            {
              p->body->print (o);
              o << endl;
            }
        }
    }
}


int pending_interrupts;

extern "C"
void handle_interrupt (int)
{
  // This might be nice, but we don't know our current verbosity...
  // clog << _F("Received signal %d", sig) << endl << flush;
  kill_stap_spawn(SIGTERM);

  pending_interrupts ++;
  // Absorb the first two signals.   This used to be one, but when
  // stap is run under sudo, and then interrupted, sudo relays a
  // redundant copy of the signal to stap, leading to an unclean shutdown.
  if (pending_interrupts > 2) // XXX: should be configurable? time-based?
    {
      char msg[] = "Too many interrupts received, exiting.\n";
      int rc = write (2, msg, sizeof(msg)-1);
      if (rc) {/* Do nothing; we don't care if our last gasp went out. */ ;}
      _exit (1);
    }
}


void
setup_signals (sighandler_t handler)
{
  struct sigaction sa;

  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = handler;
  sigemptyset (&sa.sa_mask);
  if (handler != SIG_IGN)
    {
      sigaddset (&sa.sa_mask, SIGHUP);
      sigaddset (&sa.sa_mask, SIGPIPE);
      sigaddset (&sa.sa_mask, SIGINT);
      sigaddset (&sa.sa_mask, SIGTERM);
      sigaddset (&sa.sa_mask, SIGXFSZ);
      sigaddset (&sa.sa_mask, SIGXCPU);
    }
  sa.sa_flags = SA_RESTART;

  sigaction (SIGHUP, &sa, NULL);
  sigaction (SIGPIPE, &sa, NULL);
  sigaction (SIGINT, &sa, NULL);
  sigaction (SIGTERM, &sa, NULL);
  sigaction (SIGXFSZ, &sa, NULL);
  sigaction (SIGXCPU, &sa, NULL);
}


static void
sdt_benchmark_thread(unsigned long i)
{
  PROBE(stap, benchmark__thread__start);
  while (i--)
    PROBE1(stap, benchmark, i);
  PROBE(stap, benchmark__thread__end);
}


static int
run_sdt_benchmark(systemtap_session& s)
{
  unsigned long loops = s.benchmark_sdt_loops ?: 10000000;
  unsigned long threads = s.benchmark_sdt_threads ?: 1;

  if (s.verbose > 0)
    clog << _F("Beginning SDT benchmark with %lu loops in %lu threads.",
               loops, threads) << endl;

  struct tms tms_before, tms_after;
  struct timeval tv_before, tv_after;
  unsigned _sc_clk_tck = sysconf (_SC_CLK_TCK);
  times (& tms_before);
  gettimeofday (&tv_before, NULL);

  PROBE(stap, benchmark__start);
    {
      vector<thread> handles;
      for (unsigned long i = 0; i < threads; ++i)
        handles.push_back(thread(sdt_benchmark_thread, loops));
      for (unsigned long i = 0; i < threads; ++i)
        handles[i].join();
    }
  PROBE(stap, benchmark__end);

  times (& tms_after);
  gettimeofday (&tv_after, NULL);
  if (s.verbose > 0)
    clog << _F("Completed SDT benchmark in %ldusr/%ldsys/%ldreal ms.",
               (long)(tms_after.tms_utime - tms_before.tms_utime) * 1000 / _sc_clk_tck,
               (long)(tms_after.tms_stime - tms_before.tms_stime) * 1000 / _sc_clk_tck,
               (long)((tv_after.tv_sec - tv_before.tv_sec) * 1000 +
                ((long)tv_after.tv_usec - (long)tv_before.tv_usec) / 1000))
         << endl;

  return EXIT_SUCCESS;
}

static set<string> files;
static string path_dir;

static int collect_stp(const char* fpath, const struct stat*,
                       int typeflag, struct FTW* ftwbuf)
{
  if (typeflag == FTW_F)
    {
      const char* ext = strrchr(fpath, '.');
      if (ext && (strcmp(".stp", ext) == 0))
        files.insert(fpath);
    }
  else if (typeflag == FTW_D && ftwbuf->level > 0)
    {
      // Only recurse for PATH root directory
      if (strncmp(path_dir.c_str(), fpath, path_dir.size()) != 0 ||
          (fpath[path_dir.size()] != '/' && fpath[path_dir.size()] != '\0'))
        return FTW_SKIP_SUBTREE;
    }
  return FTW_CONTINUE;
}

static int collect_stpm(const char* fpath, const struct stat*,
                        int typeflag, struct FTW* ftwbuf)
{
  if (typeflag == FTW_F)
    {
      const char* ext = strrchr(fpath, '.');
      if (ext && (strcmp(".stpm", ext) == 0))
        files.insert(fpath);
    }
  else if (typeflag == FTW_D && ftwbuf->level > 0)
    {
      // Only recurse for PATH root directory
      if (strncmp(path_dir.c_str(), fpath, path_dir.size()) != 0 ||
          (fpath[path_dir.size()] != '/' && fpath[path_dir.size()] != '\0'))
        return FTW_SKIP_SUBTREE;
    }
  return FTW_CONTINUE;
}

#ifndef HAVE_LINUX_BPF_H
int
translate_bpf_pass (systemtap_session &)
{
  return 1;
}
#endif

// Compilation passes 0 through 4
int
passes_0_4 (systemtap_session &s)
{
  int rc = 0;

  // If we don't know the release, there's no hope either locally or on a server.
  if (s.kernel_release.empty())
    {
      if (s.kernel_build_tree.empty())
        cerr << _("ERROR: kernel release isn't specified") << endl;
      else
        cerr << _F("ERROR: kernel release isn't found in \"%s\"",
                   s.kernel_build_tree.c_str()) << endl;
      return 1;
    }

  // Perform passes 0 through 4 using a compile server?
  if (! s.specified_servers.empty ())
    {
#if NEED_BASE_CLIENT_CODE
      compile_server_client client (s);
      return client.passes_0_4 ();
#else
      s.print_warning(_("Without NSS or HTTP client support, using a compile-server is not supported by this version of systemtap"));

      // This cannot be an attempt to use a server after a local compile failed
      // since --use-server-on-error is locked to 'no' if we don't have
      // NSS.
      assert (! s.try_server ());
      s.print_warning(_("Ignoring --use-server"));
#endif
    }

  // PASS 0: setting up
  s.verbose = s.perpass_verbose[0];
  PROBE1(stap, pass0__start, &s);

  // For PR1477, we used to override $PATH and $LC_ALL and other stuff
  // here.  We seem to use complete pathnames in
  // buildrun.cxx/tapsets.cxx now, so this is not necessary.  Further,
  // it interferes with util.cxx:find_executable(), used for $PATH
  // resolution.

  s.kernel_base_release.assign(s.kernel_release, 0, s.kernel_release.find('-'));

  // Update various paths to include the sysroot, if provided.
  if (!s.sysroot.empty())
    {
      if (s.update_release_sysroot && !s.sysroot.empty())
        s.kernel_build_tree = s.sysroot + s.kernel_build_tree;
      debuginfo_path_insert_sysroot(s.sysroot);
    }

  // Now that no further changes to s.kernel_build_tree can occur, let's use it.
  if (!s.runtime_usermode_p())
    {
      if ((rc = s.parse_kernel_config ()) != 0
	  || (rc = s.parse_kernel_exports ()) != 0
	  || (rc = s.parse_kernel_functions ()) != 0)
	{
	  // Try again with a server
	  s.set_try_server ();
	  return rc;
	}
    }

  // Create the name of the C source file within the temporary
  // directory.  Note the _src prefix, explained in
  // buildrun.cxx:compile_pass()
  s.translated_source = string(s.tmpdir) + "/" + s.module_name + "_src.c";

  PROBE1(stap, pass0__end, &s);

  struct tms tms_before;
  times (& tms_before);
  struct timeval tv_before;
  gettimeofday (&tv_before, NULL);

  // PASS 1a: PARSING LIBRARY SCRIPTS
  PROBE1(stap, pass1a__start, &s);

  // prep this array for tapset $n use too ... although we will reset once again for user scripts
  s.used_args.resize(s.args.size(), false);
  
  if (! s.pass_1a_complete)
    {
      // We need to handle the library scripts first because this pass
      // gathers information on .stpm files that might be needed to
      // parse the user script.

      // We need to first ascertain the status of the user script, though.
      struct stat user_file_stat;
      int user_file_stat_rc = -1;

      if (s.script_file == "-")
        {
	  user_file_stat_rc = fstat (STDIN_FILENO, & user_file_stat);
	}
      else if (s.script_file != "")
        {
	  user_file_stat_rc = stat (s.script_file.c_str(), & user_file_stat);
	}
      // otherwise, rc is 0 for a command line script

      vector<string> version_suffixes;
      if (!s.runtime_usermode_p())
        {
	  // Construct kernel-versioning search path
	  string kvr = s.kernel_release;

	  // add full kernel-version-release (2.6.NN-FOOBAR)
	  version_suffixes.push_back ("/" + kvr);

	  // add kernel version (2.6.NN)
	  if (kvr != s.kernel_base_release)
	    {
	      kvr = s.kernel_base_release;
	      version_suffixes.push_back ("/" + kvr);
	    }

	  // add kernel family (2.6)
	  string::size_type dot1_index = kvr.find ('.');
	  string::size_type dot2_index = kvr.rfind ('.');
	  while (dot2_index > dot1_index && dot2_index != string::npos)
	    {
	      kvr.erase(dot2_index);
	      version_suffixes.push_back ("/" + kvr);
	      dot2_index = kvr.rfind ('.');
	    }
	}

      // add empty string as last element
      version_suffixes.push_back ("");

      // Add arch variants of every path, just before each
      const string& arch = s.architecture;
      for (unsigned i=0; i<version_suffixes.size(); i+=2)
	version_suffixes.insert(version_suffixes.begin() + i,
				version_suffixes[i] + "/" + arch);

      // Add runtime variants of every path, before everything else
      string runtime_prefix;
      if (s.runtime_mode == systemtap_session::kernel_runtime)
	runtime_prefix = "/linux";
      else if (s.runtime_mode == systemtap_session::dyninst_runtime)
	runtime_prefix = "/dyninst";
      else if (s.runtime_mode == systemtap_session::bpf_runtime)
        runtime_prefix = "/bpf";
      if (!runtime_prefix.empty())
	for (unsigned i=0; i<version_suffixes.size(); i+=2)
	    version_suffixes.insert(version_suffixes.begin() + i/2,
				    runtime_prefix + version_suffixes[i]);

      // First, parse .stpm files on the include path. We need to have the
      // resulting macro definitions available for parsing library files,
      // but since .stpm files can consist only of '@define' constructs,
      // we can parse each one without reference to the others.
      set<pair<dev_t, ino_t> > seen_library_macro_files;
      set<string> seen_library_macro_files_names;

      for (unsigned i=0; i<s.include_path.size(); i++)
        {
	  // now iterate upon it
	  for (unsigned k=0; k<version_suffixes.size(); k++)
	    {
              int flags = FTW_ACTIONRETVAL;
	      string dir = s.include_path[i] + version_suffixes[k];
              files.clear();
              // we need to set this for the nftw() callback
              path_dir = s.include_path[i] + "/PATH";
              (void) nftw(dir.c_str(), collect_stpm, 1, flags);

	      unsigned prev_s_library_files = s.library_files.size();

	      for (auto it = files.begin(); it != files.end(); ++it)
	        {
		  assert_no_interrupts();

		  struct stat tapset_file_stat;
		  int stat_rc = stat (it->c_str(), & tapset_file_stat);
		  if (stat_rc == 0 && user_file_stat_rc == 0 &&
		      user_file_stat.st_dev == tapset_file_stat.st_dev &&
		      user_file_stat.st_ino == tapset_file_stat.st_ino)
		    {
		      cerr
			  << _F("usage error: macro tapset file '%s' cannot be run directly as a session script.",
				it->c_str()) << endl;
		      rc ++;
		    }

		  // PR11949: duplicate-eliminate tapset files
		  if (stat_rc == 0)
		    {
		      pair<dev_t,ino_t> here = make_pair(tapset_file_stat.st_dev,
							 tapset_file_stat.st_ino);
		      if (seen_library_macro_files.find(here) != seen_library_macro_files.end())
		        {
			  if (s.verbose>2)
			    clog << _F("Skipping tapset \"%s\", duplicate inode.", it->c_str()) << endl;
			  continue; 
			}
		      seen_library_macro_files.insert (here);
		    }

		  // PR12443: duplicate-eliminate harder
		  string full_path = *it;
		  string tapset_base = s.include_path[i]; // not dir; it has arch suffixes too
		  if (full_path.size() > tapset_base.size())
		    {
		      string tail_part = full_path.substr(tapset_base.size());
		      if (seen_library_macro_files_names.find (tail_part) != seen_library_macro_files_names.end())
		        {
			  if (s.verbose>2)
			    clog << _F("Skipping tapset \"%s\", duplicate name.", it->c_str()) << endl;
			  continue;
			}
		      seen_library_macro_files_names.insert (tail_part);
		    }

		  if (s.verbose>2)
		    clog << _F("Processing tapset \"%s\"", it->c_str()) << endl;

		  stapfile* f = parse_library_macros (s, *it);
		  if (f == 0)
		    s.print_warning(_F("macro tapset \"%s\" has errors, and will be skipped.", it->c_str()));
		  else
		    s.library_files.push_back (f);
		}

	      unsigned next_s_library_files = s.library_files.size();
	      if (s.verbose>1 && !files.empty())
		  //TRANSLATORS: Searching through directories, 'processed' means 'examined so far'
		clog << _F("Searched for library macro files: \"%s\", found: %zu, processed: %u",
			   dir.c_str(), files.size(),
			   (next_s_library_files-prev_s_library_files)) << endl;
	    }
	}

      // Next, gather and parse the library files.
      set<pair<dev_t, ino_t> > seen_library_files;
      set<string> seen_library_files_names;

      for (unsigned i=0; i<s.include_path.size(); i++)
        {
	  // now iterate upon it
	  for (unsigned k=0; k<version_suffixes.size(); k++)
	    {
              int flags = FTW_ACTIONRETVAL;
	      string dir = s.include_path[i] + version_suffixes[k];
              files.clear();
              // we need to set this for the nftw() callback
              path_dir = s.include_path[i] + "/PATH";
              (void) nftw(dir.c_str(), collect_stp, 1, flags);

	      unsigned prev_s_library_files = s.library_files.size();

              for (auto it = files.begin(); it != files.end(); ++it)
	        {
                  unsigned tapset_flags = pf_guru | pf_squash_errors;

                  // The first path is special, as it's the builtin tapset.
                  // Allow all features no matter what s.compatible says.
                  if (i == 0)
                    tapset_flags |= pf_no_compatible;

                  if (it->find("/PATH/") != string::npos)
                    tapset_flags |= pf_auto_path;

                  assert_no_interrupts();

		  struct stat tapset_file_stat;
		  int stat_rc = stat (it->c_str(), & tapset_file_stat);
		  if (stat_rc == 0 && user_file_stat_rc == 0 &&
		      user_file_stat.st_dev == tapset_file_stat.st_dev &&
		      user_file_stat.st_ino == tapset_file_stat.st_ino)
		    {
		      cerr 
			  << _F("usage error: tapset file '%s' cannot be run directly as a session script.",
				it->c_str()) << endl;
		      rc ++;
		    }

		  // PR11949: duplicate-eliminate tapset files
		  if (stat_rc == 0)
		    {
		      pair<dev_t,ino_t> here = make_pair(tapset_file_stat.st_dev,
							 tapset_file_stat.st_ino);
		      if (seen_library_files.find(here) != seen_library_files.end())
		        {
			  if (s.verbose>2)
			    clog << _F("Skipping tapset \"%s\", duplicate inode.", it->c_str()) << endl;
			  continue; 
			}
		      seen_library_files.insert (here);
		    }

		  // PR12443: duplicate-eliminate harder
		  string full_path = *it;
		  string tapset_base = s.include_path[i]; // not dir; it has arch suffixes too
		  if (full_path.size() > tapset_base.size())
		    {
		      string tail_part = full_path.substr(tapset_base.size());
		      if (seen_library_files_names.find (tail_part) != seen_library_files_names.end())
		        {
			  if (s.verbose>2)
			    clog << _F("Skipping tapset \"%s\", duplicate name.", it->c_str()) << endl;
			  continue;
			}
		      seen_library_files_names.insert (tail_part);
		    }

		  if (s.verbose>2)
		    clog << _F("Processing tapset \"%s\"", it->c_str()) << endl;

		  // NB: we don't need to restrict privilege only for
		  // /usr/share/systemtap, i.e., excluding
		  // user-specified $XDG_DATA_DIRS.  That's because
		  // stapdev gets root-equivalent privileges anyway;
		  // stapsys and stapusr use a remote compilation with
		  // a trusted environment, where client-side
		  // $XDG_DATA_DIRS are not passed.

		  stapfile* f = parse (s, *it, tapset_flags);
		  if (f == 0)
		    s.print_warning(_F("tapset \"%s\" has errors, and will be skipped", it->c_str()));
		  else
		    s.library_files.push_back (f);
		}

	      unsigned next_s_library_files = s.library_files.size();
	      if (s.verbose>1 && !files.empty())
		  //TRANSLATORS: Searching through directories, 'processed' means 'examined so far'
		clog << _F("Searched: \"%s\", found: %zu, processed: %u",
			   dir.c_str(), files.size(),
			   (next_s_library_files-prev_s_library_files)) << endl;
	    }
	}

      if (s.num_errors())
	rc ++;

      // Now that we've made it through pass 1a, remember this so we
      // don't have to do this again in interactive mode. This doesn't
      // effect non-interactive mode.
      s.pass_1a_complete = true;
  }

  // PASS 1b: PARSING USER SCRIPT
  PROBE1(stap, pass1b__start, &s);

  // reset for user scripts -- it's their use of $* we care about
  // except that tapsets like argv.stp can consume $parms
  fill(s.used_args.begin(), s.used_args.end(), false);

  // Only try to parse a user script if the user provided one, or if we have to
  // make one (as is the case for listing mode). Otherwise, s.user_script
  // remains NULL.
  if (!s.script_file.empty() ||
      !s.cmdline_script.empty() ||
      s.dump_mode == systemtap_session::dump_matched_probes ||
      s.dump_mode == systemtap_session::dump_matched_probes_vars)
    {
      unsigned user_flags = s.guru_mode ? pf_guru : 0;
      user_flags |= pf_user_file;
      if (s.script_file == "-")
        {
          s.user_files.push_back (parse (s, "<input>", cin, user_flags));
        }
      else if (s.script_file != "")
        {
          s.user_files.push_back (parse (s, s.script_file, user_flags));
        }
      else if (s.cmdline_script != "")
        {
          istringstream ii (s.cmdline_script);
          s.user_files.push_back(parse (s, "<input>", ii, user_flags));
        }
      else // listing mode
        {
          istringstream ii ("probe " + s.dump_matched_pattern + " {}");
          s.user_files.push_back (parse (s, "<input>", ii, user_flags));
        }

      // parses the additional script(s) (-E script). does so even if in listing
      // mode, incase there is something special in the additional script(s),
      // like a macro or alias. give them a unique name to differentiate the
      // scripts that were inputted.
      unsigned count = 1;
      for (vector<string>::iterator script = s.additional_scripts.begin(); script != s.additional_scripts.end(); script++)
        {
          string input_name = "<input" + lex_cast(count) + ">";
          istringstream ii (*script);
          s.user_files.push_back(parse (s, input_name, ii, user_flags));
          count ++;
        }

      for(vector<stapfile*>::iterator it = s.user_files.begin(); it != s.user_files.end(); it++)
        {
          if (!(*it))
            {
              // Syntax errors already printed.
              rc ++;
            }
        }
    }
  else if (s.cmdline_script.empty() &&
           s.dump_mode == systemtap_session::dump_none) // -e ''
    {
      cerr << _("Input file '<input>' is empty.") << endl;
      rc++;
    }

  // Dump a list of probe aliases picked up, if requested
  if (s.dump_mode == systemtap_session::dump_probe_aliases)
    {
      set<string> aliases;
      vector<stapfile*>::const_iterator file;
      for (file  = s.library_files.begin();
           file != s.library_files.end(); ++file)
        {
          vector<probe_alias*>::const_iterator alias;
          for (alias  = (*file)->aliases.begin();
               alias != (*file)->aliases.end(); ++alias)
            {
              stringstream ss;
              (*alias)->printsig(ss);
              string str = ss.str();
              if (!s.verbose && startswith(str, "_"))
                continue;
              aliases.insert(str);
            }
        }

      set<string>::iterator alias;
      for (alias  = aliases.begin();
           alias != aliases.end(); ++alias)
        {
          cout << *alias << endl;
        }
    }
  // Dump the parse tree if this is the last pass
  else if (rc == 0 && s.last_pass == 1)
    {
      cout << _("# parse tree dump") << endl;
      for (vector<stapfile*>::iterator it = s.user_files.begin(); it != s.user_files.end(); it++)
        (*it)->print (cout);
      cout << endl;
      if (s.verbose)
        for (unsigned i=0; i<s.library_files.size(); i++)
          {
            s.library_files[i]->print (cout);
            cout << endl;
          }
    }

  struct tms tms_after;
  times (& tms_after);
  unsigned _sc_clk_tck = sysconf (_SC_CLK_TCK);
  struct timeval tv_after;
  gettimeofday (&tv_after, NULL);

#define TIMESPRINT _("in ") << \
           (tms_after.tms_cutime + tms_after.tms_utime \
            - tms_before.tms_cutime - tms_before.tms_utime) * 1000 / (_sc_clk_tck) << "usr/" \
        << (tms_after.tms_cstime + tms_after.tms_stime \
            - tms_before.tms_cstime - tms_before.tms_stime) * 1000 / (_sc_clk_tck) << "sys/" \
        << ((tv_after.tv_sec - tv_before.tv_sec) * 1000 + \
            ((long)tv_after.tv_usec - (long)tv_before.tv_usec) / 1000) << "real ms."

  // syntax errors, if any, are already printed
  if (s.verbose)
    {
      // XXX also include a count of helper macro files loaded (.stpm)?
      int n = int(s.library_files.size());
      clog << _("Pass 1: parsed user script and ")
           << _NF("%d library script ", "%d library scripts ", n, n)
           << getmemusage()
           << TIMESPRINT
           << endl;
    }

  if (rc && !s.dump_mode)
    cerr << _("Pass 1: parse failed.  [man error::pass1]") << endl;

  PROBE1(stap, pass1__end, &s);

  assert_no_interrupts();
  if (rc || s.last_pass == 1 ||
      s.dump_mode == systemtap_session::dump_probe_aliases)
    return rc;

  times (& tms_before);
  gettimeofday (&tv_before, NULL);

  // PASS 2: ELABORATION
  s.verbose = s.perpass_verbose[1];
  PROBE1(stap, pass2__start, &s);
  rc = semantic_pass (s);

  // http handled probes need probe information from pass 2
  if (! s.http_servers.empty ())
    {
#if NEED_BASE_CLIENT_CODE
      compile_server_client client (s);
      return client.passes_0_4 ();
#else
      s.print_warning(_("Without NSS or HTTP client support, using a compile-server is not supported by this version of systemtap"));

      // This cannot be an attempt to use a server after a local compile failed
      // since --use-server-on-error is locked to 'no' if we don't have
      // NSS.
      assert (! s.try_server ());
      s.print_warning(_("Ignoring --use-server"));
#endif
    }

  // Dump a list of known probe point types, if requested.
  if (s.dump_mode == systemtap_session::dump_probe_types)
    s.pattern_root->dump (s);
  // Dump a list of functions we picked up, if requested.
  else if (s.dump_mode == systemtap_session::dump_functions)
    {
      map<string,functiondecl*>::const_iterator func;
      for (func  = s.functions.begin();
           func != s.functions.end(); ++func)
        {
          functiondecl& curfunc = *func->second;
          if (curfunc.synthetic)
            continue;
          if (!startswith(curfunc.name, "__global_"))
            continue;
          if (!s.verbose && startswith(curfunc.name, "__global__"))
            continue;
          curfunc.printsigtags(cout, s.verbose>0 /* all_tags */ );
          cout << endl;
        }
    }
  // Dump the whole script if requested, or if we stop at 2
  else if (s.dump_mode == systemtap_session::dump_matched_probes ||
           s.dump_mode == systemtap_session::dump_matched_probes_vars ||
           (rc == 0 && s.last_pass == 2) ||
           (rc != 0 && s.verbose > 2))
    printscript(s, cout);

  times (& tms_after);
  gettimeofday (&tv_after, NULL);

  if (s.verbose) {
    int np = s.probes.size();
    int nf = s.functions.size();
    int ne = s.embeds.size();
    int ng = s.globals.size();
    clog << _("Pass 2: analyzed script: ")
         << _NF("%d probe, ", "%d probes, ", np, np)
         << _NF("%d function, ", "%d functions, ", nf, nf)
         << _NF("%d embed, ", "%d embeds, ", ne, ne)
         << _NF("%d global ", "%d globals ", ng, ng)
         << getmemusage()
         << TIMESPRINT
         << endl;
  }

  missing_rpm_list_print(s, "-debuginfo");

  // Check for unused command line parameters.  But - if the argv
  // tapset was selected for inclusion, then the user-script need not
  // use $* directly, so we want to suppress the warning in this case.
  // This is hacky, but we don't have a formal way of tracking tokens
  // that came from command line arguments so as to do set-subtraction
  // at this point.
  //
  bool argc_found=false, argv_found=false;
  for (unsigned i = 0; i<s.globals.size(); i++) {
    if (s.globals[i]->unmangled_name == "argc") argc_found = true;
    if (s.globals[i]->unmangled_name == "argv") argv_found = true;
  }
  if (!argc_found && !argv_found)
    for (unsigned i = 0; i<s.used_args.size(); i++)
      if (! s.used_args[i])
        s.print_warning (_F("unused command line option $%u/@%u", i+1, i+1));
  
  if (rc && !s.dump_mode && !s.try_server ())
    cerr << _("Pass 2: analysis failed.  [man error::pass2]") << endl;

  PROBE1(stap, pass2__end, &s);

  assert_no_interrupts();
  // NB: none of the dump modes need to go beyond pass-2. If this changes, break
  // into individual modes here.
  if (rc || s.last_pass == 2 || s.dump_mode)
    return rc;

  rc = prepare_translate_pass (s);
  assert_no_interrupts();
  if (rc) return rc;

  // Generate hash.  There isn't any point in generating the hash
  // if last_pass is 2, since we'll quit before using it.
  if (s.use_script_cache)
    {
      ostringstream o;
      unsigned saved_verbose;

      {
        // Make sure we're in verbose mode, so that printscript()
        // will output function/probe bodies.
        saved_verbose = s.verbose;
        s.verbose = 3;
        printscript(s, o);  // Print script to 'o'
        s.verbose = saved_verbose;
      }

      // Generate hash
      find_script_hash (s, o.str());

      // See if we can use cached source/module.
      if (get_script_from_cache(s))
        {
	  // We may still need to build uprobes, if it's not also cached.
	  if (s.need_uprobes)
	    rc = uprobes_pass(s);

	  // If our last pass isn't 5, we're done (since passes 3 and
	  // 4 just generate what we just pulled out of the cache).
	  assert_no_interrupts();
	  if (rc || s.last_pass < 5) return rc;

	  // Short-circuit to pass 5.
	  return 0;
	}
    }

  // PASS 3: TRANSLATION
  // (BPF does translation and compilation at once in pass 4.)
  if (s.runtime_mode != systemtap_session::bpf_runtime)
    {
      s.verbose = s.perpass_verbose[2];
      times (& tms_before);
      gettimeofday (&tv_before, NULL);
      PROBE1(stap, pass3__start, &s);

      rc = translate_pass (s);

      if (! rc && s.last_pass == 3)
	{
	  ifstream i (s.translated_source.c_str());
	  cout << i.rdbuf();
	}

      times (& tms_after);
      gettimeofday (&tv_after, NULL);

      if (s.verbose)
	clog << _("Pass 3: translated to C into \"")
	     << s.translated_source
	     << "\" "
	     << getmemusage()
	     << TIMESPRINT
	     << endl;

      if (rc && ! s.try_server ())
	cerr << _("Pass 3: translation failed.  [man error::pass3]") << endl;

      PROBE1(stap, pass3__end, &s);

      assert_no_interrupts();
      if (rc || s.last_pass == 3)
	return rc;
    }

  // PASS 4: COMPILATION
  s.verbose = s.perpass_verbose[3];
  times (& tms_before);
  gettimeofday (&tv_before, NULL);
  PROBE1(stap, pass4__start, &s);

  if (s.runtime_mode == systemtap_session::bpf_runtime)
    rc = translate_bpf_pass (s);
  else
    {
      if (s.use_cache)
	{
	  find_stapconf_hash(s);
	  get_stapconf_from_cache(s);
	}
      rc = compile_pass (s);
    }

  if (! rc && s.last_pass <= 4)
    {
      cout << ((s.hash_path == "") ? s.module_filename() : s.hash_path);
      cout << endl;
    }

  times (& tms_after);
  gettimeofday (&tv_after, NULL);

  if (s.verbose)
    {
      if (s.runtime_mode == systemtap_session::bpf_runtime)
	clog << _("Pass 4: compiled BPF into \"");
      else
	clog << _("Pass 4: compiled C into \"");
      clog << s.module_filename() << "\" " << TIMESPRINT << endl;
    }

  if (rc && ! s.try_server ())
    cerr << _("Pass 4: compilation failed.  [man error::pass4]") << endl;
  else
    {
      // Update cache. Cache cleaning is kicked off at the
      // beginning of this function.
      if (s.use_script_cache)
        add_script_to_cache(s);
      if (s.use_cache && !s.runtime_usermode_p())
        add_stapconf_to_cache(s);

      // We may need to save the module in $CWD if the cache was
      // inaccessible for some reason.
      if (! s.use_script_cache && s.last_pass <= 4)
        s.save_module = true;

      // Copy module to the current directory.
      if (s.save_module && !pending_interrupts)
        {
	  string module_src_path = s.tmpdir + "/" + s.module_filename();
	  string module_dest_path = s.module_filename();
	  copy_file(module_src_path, module_dest_path, s.verbose > 1);
	}

      // Copy uprobes module to the current directory.
      if (s.save_uprobes && !s.uprobes_path.empty() && !pending_interrupts)
        {
          rc = create_dir("uprobes");
          if (! rc)
            copy_file(s.uprobes_path, "uprobes/uprobes.ko", s.verbose > 1);
        }
    }

  PROBE1(stap, pass4__end, &s);

  return rc;
}

int
pass_5 (systemtap_session &s, vector<remote*> targets)
{
  // PASS 5: RUN
  s.verbose = s.perpass_verbose[4];
  struct tms tms_before;
  times (& tms_before);
  struct timeval tv_before;
  gettimeofday (&tv_before, NULL);
  // NB: this message is a judgement call.  The other passes don't emit
  // a "hello, I'm starting" message, but then the others aren't interactive
  // and don't take an indefinite amount of time.
  PROBE1(stap, pass5__start, &s);
  if (s.verbose) clog << _("Pass 5: starting run.") << endl;
  int rc = remote::run(targets);
  struct tms tms_after;
  times (& tms_after);
  unsigned _sc_clk_tck = sysconf (_SC_CLK_TCK);
  struct timeval tv_after;
  gettimeofday (&tv_after, NULL);
  if (s.verbose) clog << _("Pass 5: run completed ")
                      << TIMESPRINT
                      << endl;

  if (rc)
    cerr << _("Pass 5: run failed.  [man error::pass5]") << endl;
  else
    // Interrupting pass-5 to quit is normal, so we want an EXIT_SUCCESS below.
    pending_interrupts = 0;

  PROBE1(stap, pass5__end, &s);

  return rc;
}

static void
cleanup (systemtap_session &s, int rc)
{
  // PASS 6: cleaning up
  PROBE1(stap, pass6__start, &s);

  for (systemtap_session::session_map_t::iterator it = s.subsessions.begin();
       it != s.subsessions.end(); ++it)
    cleanup (*it->second, rc);

  // update the database information
  if (!rc && s.tapset_compile_coverage && !pending_interrupts) {
#ifdef HAVE_LIBSQLITE3
    update_coverage_db(s);
#else
    cerr << _("Coverage database not available without libsqlite3") << endl;
#endif
  }

  s.report_suppression();

  PROBE1(stap, pass6__end, &s);
}

static int
passes_0_4_again_with_server (systemtap_session &s)
{
  // Not a server and not already using a server.
  assert (! s.client_options);
  assert (s.specified_servers.empty ());

  // Specify default server(s).
  s.specified_servers.push_back ("");

  // Reset the previous temporary directory and start fresh
  s.reset_tmp_dir();

  // Try to compile again, using the server
  clog << _("Attempting compilation using a compile server")
       << endl;

  int rc = passes_0_4 (s);
  return rc;
}

int
main (int argc, char * const argv [])
{
  // Initialize defaults.
  try {
    systemtap_session s;
#if ENABLE_NLS
    setlocale (LC_ALL, "");
    bindtextdomain (PACKAGE, LOCALEDIR);
    textdomain (PACKAGE);
#endif

    // Set up our handler to catch routine signals, to allow clean
    // and reasonably timely exit.
    setup_signals(&handle_interrupt);

    // PR13520: Parse $SYSTEMTAP_DIR/rc for extra options
    string rc_file = s.data_path + "/rc";
    ifstream rcf (rc_file.c_str());
    string rcline;
    wordexp_t words;
    memset (& words, 0, sizeof(words));
    int rc = 0;
    int linecount = 0;
    while (getline (rcf, rcline))
      {
        rc = wordexp (rcline.c_str(), & words, WRDE_NOCMD|WRDE_UNDEF|
                      (linecount > 0 ? WRDE_APPEND : 0)); 
        // NB: WRDE_APPEND automagically reallocates words.* as more options are added.
        linecount ++;
        if (rc) break;
      }
    rcf.close();

    int extended_argc = words.we_wordc + argc;
    char **extended_argv = (char**) calloc (extended_argc + 1, sizeof(char*));
    if (rc || !extended_argv)
      {
        clog << _F("Error processing extra options in %s", rc_file.c_str());
        return EXIT_FAILURE;
      }
    // Copy over the arguments *by reference*, first the ones from the rc file.
    char **p = & extended_argv[0];
    *p++ = argv[0];
    for (unsigned i=0; i<words.we_wordc; i++) *p++ = words.we_wordv[i];
    for (int j=1; j<argc; j++) *p++ = argv[j];
    *p++ = NULL;

    // Process the command line.
    rc = s.parse_cmdline (extended_argc, extended_argv);
    if (rc != 0)
      return rc;

    // Create the temp dir.
    s.create_tmp_dir();

    if (words.we_wordc > 0 && s.verbose > 1)
      clog << _F("Extra options in %s: %d\n", rc_file.c_str(), (int)words.we_wordc);

    // Check for options conflicts. Exits if errors are detected.
    s.check_options (extended_argc, extended_argv);

    // We don't need these strings any more.
    wordfree (& words);
    free (extended_argv);

    // arguments parsed; get down to business
    if (s.verbose > 1)
      s.version ();

    // Need to send the verbose message here, rather than in the session ctor, since
    // we didn't know if verbose was set.
    if (rc == 0 && s.verbose>1)
      clog << _F("Created temporary directory \"%s\"", s.tmpdir.c_str()) << endl;

    // Run the benchmark and quit right away.
    if (s.benchmark_sdt_loops || s.benchmark_sdt_threads)
      return run_sdt_benchmark(s);

    // Prepare connections for each specified remote target.
    vector<remote*> targets;
    bool fake_remote=false;
    if (s.remote_uris.empty())
      {
        fake_remote=true;
        s.remote_uris.push_back("direct:");
      }
    for (unsigned i = 0; rc == 0 && i < s.remote_uris.size(); ++i)
      {
        // PR13354: pass remote id#/url only in non --remote=HOST cases
        remote *target = remote::create(s, s.remote_uris[i],
                                        fake_remote ? -1 : (int)i);
        if (target)
          targets.push_back(target);
        else
          rc = 1;
      }

    // Discover and loop over each unique session created by the remote targets.
    set<systemtap_session*> sessions;
    for (unsigned i = 0; i < targets.size(); ++i)
      sessions.insert(targets[i]->get_session());

    // FIXME: For now, only attempt local interactive use.
    if (s.interactive_mode && fake_remote)
      {
#ifdef HAVE_LIBREADLINE
	rc = interactive_mode (s, targets);
#endif
      }
    else
      {
	for (set<systemtap_session*>::iterator it = sessions.begin();
	     rc == 0 && !pending_interrupts && it != sessions.end(); ++it)
	  {
	    systemtap_session& ss = **it;
            if (ss.verbose > 1)
	      clog << _F("Session arch: %s release: %s",
			 ss.architecture.c_str(), ss.kernel_release.c_str())
		   << endl;

#if HAVE_NSS
	    // If requested, query server status. This is independent
	    // of other tasks.
	    nss_client_query_server_status (ss);

	    // If requested, manage trust of servers. This is
	    // independent of other tasks.
	    nss_client_manage_server_trust (ss);
#endif

	    // Run the passes only if a script has been specified or
	    // if we're dumping something. The requirement for a
	    // script has already been checked in
	    // systemtap_session::check_options.
	    if (ss.have_script || ss.dump_mode)
	      {
		// Run passes 0-4 for each unique session, either
		// locally or using a compile-server.
		ss.init_try_server ();
		if ((rc = passes_0_4 (ss)))
		  {
		    // Compilation failed.
		    // Try again using a server if appropriate.
		    if (ss.try_server ())
		      rc = passes_0_4_again_with_server (ss);
		  }
		if (rc || s.perpass_verbose[0] >= 1)
		  s.explain_auto_options ();
	      }
	  }

	// Run pass 5, if requested
	if (rc == 0 && s.have_script && s.last_pass >= 5 && ! pending_interrupts)
	  rc = pass_5 (s, targets);
      }

    // Pass 6. Cleanup
    for (unsigned i = 0; i < targets.size(); ++i)
      delete targets[i];
    cleanup (s, rc);

    assert_no_interrupts();
    return (rc) ? EXIT_FAILURE : EXIT_SUCCESS;
  }
  catch (const interrupt_exception& e) {
      // User entered ctrl-c, exit quietly.
      return EXIT_FAILURE;
  }
  catch (const exit_exception& e) {
      // Exiting for any quiet reason.
      return e.rc;
  }
  catch (const exception &e) {
      // Some other uncaught exception.
      cerr << e.what() << endl;
      return EXIT_FAILURE;
  }
  catch (...) {
      // Catch all other unknown exceptions.
      cerr << _("ERROR: caught unknown exception!") << endl;
      return EXIT_FAILURE;
  }
}

/* vim: set sw=2 ts=8 cino=>4,n-2,{2,^-2,t0,(0,u0,w1,M1 : */
