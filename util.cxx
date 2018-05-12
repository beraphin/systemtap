// Copyright (C) Andrew Tridgell 2002 (original file)
// Copyright (C) 2006-2014 Red Hat Inc. (systemtap changes)
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License as
// published by the Free Software Foundation; either version 2 of the
// License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include "util.h"
#include "stap-probe.h"
#include <stdexcept>
#include <cerrno>
#include <map>
#include <set>
#include <string>
#include <fstream>
#include <cassert>
#include <ext/stdio_filebuf.h>
#include <algorithm>
#include <mutex>

extern "C" {
#include <elf.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <regex.h>
#include <stdarg.h>
}

using namespace std;
using namespace __gnu_cxx;


// Return current users home directory or die.
const char *
get_home_directory(void)
{
  const char *p = getenv("HOME");
  if (p)
    return p;

  struct passwd *pwd = getpwuid(getuid());
  if (pwd)
    return pwd->pw_dir;

  cerr << _("Unable to determine home directory") << endl;
  return "/";
}


// Get the size of a file in bytes
size_t
get_file_size(const string &path)
{
  struct stat file_info;

  if (stat(path.c_str(), &file_info) == 0)
    return file_info.st_size;
  else
    return 0;
}

// Get the size of a file in bytes
size_t
get_file_size(int fd)
{
  struct stat file_info;

  if (fstat(fd, &file_info) == 0)
    return file_info.st_size;
  else
    return 0;
}

// Check that a file is present
bool
file_exists (const string &path)
{
  struct stat file_info;

  if (stat(path.c_str(), &file_info) == 0)
    return true;

  return false;
}

// Copy a file.  The copy is done via a temporary file and atomic
// rename.
bool
copy_file(const string& src, const string& dest, bool verbose)
{
  int fd1, fd2;
  char buf[10240];
  int n;
  string tmp;
  char *tmp_name;
  mode_t mask;

  if (verbose)
    clog << _F("Copying %s to %s", src.c_str(), dest.c_str()) << endl;

  // Open the src file.
  fd1 = open(src.c_str(), O_RDONLY);
  if (fd1 == -1)
    goto error;

  // Open the temporary output file.
  tmp = dest + string(".XXXXXX");
  tmp_name = (char *)tmp.c_str();
  fd2 = mkstemp(tmp_name);
  if (fd2 == -1)
    {
      close(fd1);
      goto error;
    }

  // Copy the src file to the temporary output file.
  while ((n = read(fd1, buf, sizeof(buf))) > 0)
    {
      if (write(fd2, buf, n) != n)
        {
	  close(fd2);
	  close(fd1);
	  unlink(tmp_name);
          goto error;
	}
    }
  close(fd1);

  // Set the permissions on the temporary output file.
  mask = umask(0);
  fchmod(fd2, 0666 & ~mask);
  umask(mask);

  // Close the temporary output file.  The close can fail on NFS if
  // out of space.
  if (close(fd2) == -1)
    {
      unlink(tmp_name);
      goto error;
    }

  // Rename the temporary output file to the destination file.
  unlink(dest.c_str());
  if (rename(tmp_name, dest.c_str()) == -1)
    {
      unlink(tmp_name);
      goto error;
    }

  return true;

error:
  cerr << _F("Copy failed (\"%s\" to \"%s\"): %s", src.c_str(),
             dest.c_str(), strerror(errno)) << endl;
  return false;
}


// Make sure a directory exists.
int
create_dir(const char *dir, int mode)
{
  struct stat st;
  if (stat(dir, &st) == 0)
    {
      if (S_ISDIR(st.st_mode))
	return 0;
      errno = ENOTDIR;
      return 1;
    }

  // Create the directory. We must create each component
  // of the path ourselves.
  vector<string> components;
  tokenize (dir, components, "/");
  string path;
  if (*dir == '/')
    {
      // Absolute path
      path = "/";
    }
  unsigned limit = components.size ();
  assert (limit != 0);
  for (unsigned ix = 0; ix < limit; ++ix)
    {
      path += components[ix] + '/';
      if (mkdir(path.c_str (), mode) != 0 && errno != EEXIST)
	return 1;
    }

  return 0;
}

// Remove a file or directory
int
remove_file_or_dir (const char *name)
{
  int rc;
  struct stat st;

  if ((rc = stat(name, &st)) != 0)
    {
      if (errno == ENOENT)
	return 0;
      return 1;
    }

  if (remove (name) != 0)
    return 1;

  return 0;
}


int
appendenv (const char *env_name, const string source)
{
  string dirname = source.substr(0, source.rfind("/"));
  char *env = getenv(env_name);
  string new_env;
  
  if (env)
    new_env = string (env) + ":" + dirname;
  else
    new_env = dirname;
    
  return setenv(env_name, new_env.c_str(), 1);
}


/* Obtain the gid of the given group. */
gid_t get_gid (const char *group_name)
{
  struct group *stgr;
  /* If we couldn't find the group, return an invalid number. */
  stgr = getgrnam(group_name);
  if (stgr == NULL)
    return (gid_t)-1;
  return stgr->gr_gid;
}

// Determine whether the current user is in the given group
// by gid.
bool
in_group_id (gid_t target_gid)
{
  // According to the getgroups() man page, getgroups() may not
  // return the effective gid, so try to match it first. */
  if (target_gid == getegid())
    return true;

  // Get the list of the user's groups.
  int ngids = getgroups(0, 0); // Returns the number to allocate.
  if (ngids > 0) {
    gid_t gidlist[ngids];
    ngids = getgroups(ngids, gidlist);
    for (int i = 0; i < ngids; i++) {
      // If the user is a member of the target group, then we're done.
      if (gidlist[i] == target_gid)
	return true;
    }
  }
  if (ngids < 0) {
    cerr << _("Unable to retrieve group list") << endl;
    return false;
  }

  // The user is not a member of the target group
  return false;
}

/*
 * Returns a string describing memory resource usage.
 * Since it seems getrusage() doesn't maintain the mem related fields,
 * this routine parses /proc/self/statm to get the statistics.
 */
string
getmemusage ()
{
  static long sz = sysconf(_SC_PAGESIZE);

  long pages;
  ostringstream oss;
  ifstream statm("/proc/self/statm");
  statm >> pages;
  long kb1 = pages * sz / 1024; // total program size; vmsize
  statm >> pages;
  long kb2 = pages * sz / 1024; // resident set size; vmrss
  statm >> pages;
  long kb3 = pages * sz / 1024; // shared pages
  statm >> pages;
  long kb4 = pages * sz / 1024; // text
  statm >> pages;
  (void) kb4;
  long kb5 = pages * sz / 1024; // library
  statm >> pages;
  (void) kb5;
  long kb6 = pages * sz / 1024; // data+stack
  statm >> pages;
  long kb7 = pages * sz / 1024; // dirty
  (void) kb7;

  oss << _F("using %ldvirt/%ldres/%ldshr/%lddata kb, ", kb1, kb2, kb3, kb6);
  return oss.str();
}

void
tokenize(const string& str, vector<string>& tokens,
	 const string& delimiters)
{
  // Skip delimiters at beginning.
  string::size_type lastPos = str.find_first_not_of(delimiters, 0);
  // Find first "non-delimiter".
  string::size_type pos     = str.find_first_of(delimiters, lastPos);

  while (pos != string::npos || lastPos != string::npos)
    {
      // Found a token, add it to the vector.
      tokens.push_back(str.substr(lastPos, pos - lastPos));
      // Skip delimiters.  Note the "not_of"
      lastPos = str.find_first_not_of(delimiters, pos);
      // Find next "non-delimiter"
      pos = str.find_first_of(delimiters, lastPos);
    }
}

// Akin to tokenize(...,...), but allow tokens before the first delimeter, after the
// last delimiter and allow internal empty tokens
void
tokenize_full(const string& str, vector<string>& tokens,
	      const string& delimiters)
{
  // Check for an empty string or a string of length 1. Neither can have the requested
  // components.
  if (str.size() <= 1)
    return;

  // Find the first delimeter.
  string::size_type lastPos = 0;
  string::size_type pos = str.find_first_of(delimiters, lastPos);
  if (pos == string::npos)
    return; // no delimeters

  /* No leading empty component allowed. */
  if (pos == lastPos)
    ++lastPos;

  assert (lastPos < str.size());
  do
    {
      pos = str.find_first_of(delimiters, lastPos);
      if (pos == string::npos)
	break; // Final trailing component
      // Found a token, add it to the vector.
      tokens.push_back(str.substr (lastPos, pos - lastPos));
      // Skip the delimiter.
      lastPos = pos + 1;
    }
  while (lastPos < str.size());

  // A final non-delimited token, if it is not empty.
  if (lastPos < str.size())
    {
      assert (pos == string::npos);
      tokens.push_back(str.substr (lastPos));
    }
}

// Akin to tokenize(...,"::"), but it also has to deal with C++ template
// madness.  We do this naively by balancing '<' and '>' characters.  This
// doesn't eliminate blanks either, so a leading ::scope still works.
void
tokenize_cxx(const string& str, vector<string>& tokens)
{
  int angle_count = 0;
  string::size_type pos = 0;
  string::size_type colon_pos = str.find("::");
  string::size_type angle_pos = str.find_first_of("<>");
  while (colon_pos != string::npos &&
         (angle_count == 0 || angle_pos != string::npos))
    {
      if (angle_count > 0 || angle_pos < colon_pos)
        {
          angle_count += str.at(angle_pos) == '<' ? 1 : -1;
          colon_pos = str.find("::", angle_pos + 1);
          angle_pos = str.find_first_of("<>", angle_pos + 1);
        }
      else
        {
          tokens.push_back(str.substr(pos, colon_pos - pos));
          pos = colon_pos + 2;
          colon_pos = str.find("::", pos);
          angle_pos = str.find_first_of("<>", pos);
        }
    }
  tokens.push_back(str.substr(pos));
}

// Searches for lines in buf delimited by either \n, \0. Returns a vector
// containing tuples of the type (start of line, length of line). If data
// remains before the end of the buffer, a last line is added. All delimiters
// are kept.
vector<pair<const char*,int> >
split_lines(const char *buf, size_t n)
{
  vector<pair<const char*,int> > lines;
  const char *eol, *line;
  line = eol = buf;
  while ((size_t)(eol-buf) < n)
    {
      if (*eol == '\n' || *eol == '\0')
        {
          lines.push_back(make_pair(line, eol-line+1));
          line = ++eol;
        }
      else
        eol++;
    }

  // add any last line
  if (eol > line)
    lines.push_back(make_pair(line, eol-line));

  return lines;
}

// Resolve an executable name to a canonical full path name, with the
// same policy as execvp().  A program name not containing a slash
// will be searched along the $PATH.

string find_executable(const string& name)
{
  const map<string, string> sysenv;
  return find_executable(name, "", sysenv);
}

string find_executable(const string& name, const string& sysroot,
		       const map<string, string>& sysenv,
		       const string& env_path)
{
  string retpath;

  if (name.size() == 0)
    return name;

  struct stat st;

  if (name.find('/') != string::npos) // slash in the path already?
    {
      retpath = sysroot + name;
    }
  else // Nope, search $PATH.
    {
      const char *path;
      if (sysenv.count(env_path) != 0)
        path = sysenv.find(env_path)->second.c_str();
      else
        path = getenv(env_path.c_str());
      if (path)
        {
          // Split PATH up.
          vector<string> dirs;
          tokenize(string(path), dirs, string(":"));

          // Search the path looking for the first executable of the right name.
          for (vector<string>::iterator i = dirs.begin(); i != dirs.end(); i++)
            {
              string fname = sysroot + *i + "/" + name;
              const char *f = fname.c_str();

              // Look for a normal executable file.
              if (access(f, X_OK) == 0
                  && stat(f, &st) == 0
                  && S_ISREG(st.st_mode))
                {
                  retpath = fname;
                  break;
                }
            }
        }
    }


  // Could not find the program on the $PATH.  We'll just fall back to
  // the unqualified name, which our caller will probably fail with.
  if (retpath == "")
    retpath = sysroot + name;

  // Canonicalize the path name.
  string scf = resolve_path(retpath);
  if (!startswith(scf, sysroot))
    throw runtime_error(_F("find_executable(): file %s not in sysroot %s",
                           scf.c_str(), sysroot.c_str()));
  return scf;
}


bool is_fully_resolved(const string& path, const string& sysroot,
		       const map<string, string>& sysenv,
		       const string& env_path)
{
  return !path.empty()
      && !contains_glob_chars(path)
      && path.find('/') != string::npos
      && path == find_executable(path, sysroot, sysenv, env_path);
}

const string cmdstr_quoted(const string& cmd)
{
	// original cmd : substr1
	//           or : substr1'substr2
	//           or : substr1'substr2'substr3......
	// after quoted :
	// every substr(even it's empty) is quoted by ''
	// every single-quote(') is quoted by ""
	// examples: substr1 --> 'substr1'
	//           substr1'substr2 --> 'substr1'"'"'substr2'

	string quoted_cmd;
	string quote("'");
	string replace("'\"'\"'");
	string::size_type pos = 0;

	quoted_cmd += quote;
	for (string::size_type quote_pos = cmd.find(quote, pos);
			quote_pos != string::npos;
			quote_pos = cmd.find(quote, pos)) {
		quoted_cmd += cmd.substr(pos, quote_pos - pos);
		quoted_cmd += replace;
		pos = quote_pos + 1;
	}
	quoted_cmd += cmd.substr(pos, cmd.length() - pos);
	quoted_cmd += quote;

	return quoted_cmd;
}

const string
detox_path(const string& str)
{
  ostringstream hash;
  for (int i=0; i<int(str.length()); i++)
    if (isalnum(str[i]))
      hash << str[i];
    else
      hash << "_";
  hash << "_";
  return hash.str();
}

const string
cmdstr_join(const vector<string>& cmds)
{
  if (cmds.empty())
    throw runtime_error(_("cmdstr_join called with an empty command!"));

  stringstream cmd;
  cmd << cmdstr_quoted(cmds[0]);
  for (size_t i = 1; i < cmds.size(); ++i)
    cmd << " " << cmdstr_quoted(cmds[i]);

  return cmd.str();
}


const string
join(const vector<string>& vec, const string &delim)
{
  if (vec.empty())
    throw runtime_error(_("join called with an empty vector!"));

  stringstream join_str;
  join_str << vec[0];
  for (size_t i = 1; i < vec.size(); ++i)
    join_str << delim << vec[i];

  return join_str.str();
}


// signal-safe set of pids
class spawned_pids_t {
  private:
    set<pid_t> pids;

#ifndef SINGLE_THREADED
    mutex mux_pids;
#endif

    unique_lock<mutex> lock()
      {
#ifndef SINGLE_THREADED
        return unique_lock<mutex>(mux_pids);
#else
        return {};
#endif
      }

  public:

    bool contains (pid_t p)
      {
        stap_sigmasker masked;
        auto guard = lock();

        bool ret = (pids.count(p)==0) ? true : false;

        return ret;
      }

    bool insert (pid_t p)
      {
        stap_sigmasker masked;
        auto guard = lock();

        bool ret = (p > 0) ? pids.insert(p).second : false;

        return ret;
      }

    void erase (pid_t p)
      {
        stap_sigmasker masked;
        auto guard = lock();

        pids.erase(p);
      }

    int killall (int sig)
      {
        int ret = 0;
        stap_sigmasker masked;
        auto guard = lock();

        for (set<pid_t>::const_iterator it = pids.begin();
             it != pids.end(); ++it)
          ret = kill(*it, sig) ?: ret;
        return ret;
      }

};
static spawned_pids_t spawned_pids;

/* Returns exit code of pid if terminated nicely, or 128+signal if terminated
 * not nicely, or -1 if waitpid() failed. So if ret >= 0, it's the rc/signal */
int
stap_waitpid(int verbose, pid_t pid)
{
  int ret, status;
  if (verbose > 1 && spawned_pids.contains(pid))
    clog << _F("Spawn waitpid call on unmanaged pid %d", pid) << endl;
  ret = waitpid(pid, &status, 0);
  if (ret == pid)
    {
      spawned_pids.erase(pid);
      ret = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
      if (verbose > 1)
        clog << _F("Spawn waitpid result (0x%x): %d", (unsigned)status, ret) << endl;
    }
  else
    {
      if (verbose > 1)
        clog << _F("Spawn waitpid error (%d): %s", ret, strerror(errno)) << endl;
      ret = -1;
    }
  PROBE2(stap, stap_system__complete, ret, pid);
  return ret;
}

static int
pipe_child_fd(posix_spawn_file_actions_t* fa, int pipefd[2], int childfd)
{
  if (pipe(pipefd))
    return -1;

  int dir = childfd ? 1 : 0;
  if (!fcntl(pipefd[0], F_SETFD, FD_CLOEXEC) &&
      !fcntl(pipefd[1], F_SETFD, FD_CLOEXEC) &&
      !posix_spawn_file_actions_adddup2(fa, pipefd[dir], childfd))
    return 0;

  close(pipefd[0]);
  close(pipefd[1]);
  return -1;
}

static int
null_child_fd(posix_spawn_file_actions_t* fa, int childfd)
{
  int flags = childfd ? O_WRONLY : O_RDONLY;
  return posix_spawn_file_actions_addopen(fa, childfd, "/dev/null", flags, 0);
}

// Runs a command with a saved PID, so we can kill it from the signal handler
pid_t
stap_spawn(int verbose, const vector<string>& args,
           posix_spawn_file_actions_t* fa, const vector<string>& envVec)
{
  string::const_iterator it;
  it = args[0].begin();
  string command;
  if(*it == '/' && (access(args[0].c_str(), X_OK)==-1)) //checking to see if staprun is executable
    // XXX PR13274 needs-session to use print_warning()
    clog << _F("WARNING: %s is not executable (%s)", args[0].c_str(), strerror(errno)) << endl;
  for (size_t i = 0; i < args.size(); ++i)
    command += " " + args[i];
  PROBE1(stap, stap_system__start, command.c_str());
  if (verbose > 1)
    clog << _("Running") << command << endl;

  char const * argv[args.size() + 1];
  for (size_t i = 0; i < args.size(); ++i)
    argv[i] = args[i].c_str();
  argv[args.size()] = NULL;

  char** env;
  bool allocated;
  // environ can be NULL. This has been observed when running under gdb.
  if(envVec.empty() && environ != NULL)
  {
	  env = environ;
  	  allocated = false;
  }
  else
  {
	allocated = true;
	env = new char*[envVec.size() + 1];

  	for (size_t i = 0; i < envVec.size(); ++i)
  	    env[i] = (char*)envVec[i].c_str();
  	  env[envVec.size()] = NULL;
  }

  pid_t pid = 0;
  int ret = posix_spawnp(&pid, argv[0], fa, NULL,
                         const_cast<char * const *>(argv), env);
 if (allocated)
	  delete[] env;

  PROBE2(stap, stap_system__spawn, ret, pid);
  if (ret != 0)
    {
      if (verbose > 1)
        clog << _F("Spawn error (%d): %s", ret, strerror(ret)) << endl;
      pid = -1;
    }
  else
    spawned_pids.insert(pid);
  return pid;
}

// The API version of stap_spawn doesn't expose file_actions, for now.
pid_t
stap_spawn(int verbose, const vector<string>& args)
{
  return stap_spawn(verbose, args, NULL);
}

pid_t
stap_spawn_piped(int verbose, const vector<string>& args,
                 int *child_in, int *child_out, int* child_err)
{
  pid_t pid = -1;
  int infd[2], outfd[2], errfd[2];
  posix_spawn_file_actions_t fa;
  if (posix_spawn_file_actions_init(&fa) != 0)
    return -1;

  if (child_in && pipe_child_fd(&fa, infd, 0) != 0)
    goto cleanup_fa;
  if (child_out && pipe_child_fd(&fa, outfd, 1) != 0)
    goto cleanup_in;
  if (child_err && pipe_child_fd(&fa, errfd, 2) != 0)
    goto cleanup_out;

  pid = stap_spawn(verbose, args, &fa);

  if (child_err)
    {
      if (pid > 0)
        *child_err = errfd[0];
      else
        close(errfd[0]);
      close(errfd[1]);
    }

cleanup_out:
  if (child_out)
    {
      if (pid > 0)
        *child_out = outfd[0];
      else
        close(outfd[0]);
      close(outfd[1]);
    }

cleanup_in:
  if (child_in)
    {
      if (pid > 0)
        *child_in = infd[1];
      else
        close(infd[1]);
      close(infd[0]);
    }

cleanup_fa:
  posix_spawn_file_actions_destroy(&fa);

  return pid;
}

// Global set of supported localization variables. Make changes here to
// add or remove variables. List of variables from:
// http://publib.boulder.ibm.com/infocenter/tivihelp/v8r1/index.jsp?topic=/
// com.ibm.netcool_OMNIbus.doc_7.3.0/omnibus/wip/install/concept/omn_con_settingyourlocale.html
const set<string>&
localization_variables()
{
  static set<string> localeVars;
  if (localeVars.empty())
    {
      localeVars.insert("LANG");
      localeVars.insert("LC_ALL");
      localeVars.insert("LC_CTYPE");
      localeVars.insert("LC_COLLATE");
      localeVars.insert("LC_MESSAGES");
      localeVars.insert("LC_TIME");
      localeVars.insert("LC_MONETARY");
      localeVars.insert("LC_NUMERIC");
    }
  return localeVars;
}

// Runs a command with a saved PID, so we can kill it from the signal handler,
// and wait for it to finish.
int
stap_system(int verbose, const string& description,
            const vector<string>& args,
            bool null_out, bool null_err)
{
  int ret = 0;
  posix_spawn_file_actions_t fa;
  if (posix_spawn_file_actions_init(&fa) != 0)
    return -1;

  if ((null_out && null_child_fd(&fa, 1) != 0) ||
      (null_err && null_child_fd(&fa, 2) != 0))
    ret = -1;
  else
    {
      pid_t pid = stap_spawn(verbose, args, &fa);
      ret = pid;
      if (pid > 0){
        ret = stap_waitpid(verbose, pid);

        // XXX PR13274 needs-session to use print_warning()
        if (ret > 128)
          clog << _F("WARNING: %s exited with signal: %d (%s)",
                     description.c_str(), ret - 128, strsignal(ret - 128)) << endl;
        else if (ret > 0)
          clog << _F("WARNING: %s exited with status: %d",
                     description.c_str(), ret) << endl;
      }
    }

  posix_spawn_file_actions_destroy(&fa);
  return ret;
}

// Like stap_system, but capture stdout
int
stap_system_read(int verbose, const vector<string>& args, ostream& out)
{
  int child_fd = -1;
  pid_t child = stap_spawn_piped(verbose, args, NULL, &child_fd);
  if (child > 0)
    {
      // read everything from the child
      stdio_filebuf<char> in(child_fd, ios_base::in);
      out << &in;
      return stap_waitpid(verbose, child);
    }
  return -1;
}


std::pair<bool,int>
stap_fork_read(int verbose, ostream& out)
{
  int pipefd[2];
  if (pipe(pipefd) != 0)
    return make_pair(false, -1);

  fflush(stdout); cout.flush();

  if (verbose > 1)
    clog << _("Forking subprocess...") << endl;

  pid_t child = fork();
  PROBE1(stap, stap_system__fork, child);
  // child < 0: fork failure
  if (child < 0)
    {
      if (verbose > 1)
        clog << _F("Fork error (%d): %s", child, strerror(errno)) << endl;
      close(pipefd[0]);
      close(pipefd[1]);
      return make_pair(false, -1);
    }
  // child == 0: we're the child
  else if (child == 0)
    {
      close(pipefd[0]);
      fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);
      return make_pair(true, pipefd[1]);
    }

  // child > 0: we're the parent
  spawned_pids.insert(child);

  // read everything from the child
  close(pipefd[1]);
  stdio_filebuf<char> in(pipefd[0], ios_base::in);
  out << &in;
  return make_pair(false, stap_waitpid(verbose, child));
}


// Send a signal to our spawned commands
int
kill_stap_spawn(int sig)
{
  return spawned_pids.killall(sig);
}



void assert_regexp_match (const string& name, const string& value, const string& re)
{
  typedef map<string,regex_t*> cache;
  static cache compiled;
  cache::iterator it = compiled.find (re);
  regex_t* r = 0;
  if (it == compiled.end())
    {
      r = new regex_t;
      int rc = regcomp (r, re.c_str(), REG_ICASE|REG_NOSUB|REG_EXTENDED);
      assert (rc == 0);
      compiled[re] = r;
    }
  else
    r = it->second;

  // run regexec
  int rc = regexec (r, value.c_str(), 0, 0, 0);
  if (rc)
    throw runtime_error
      (_F("ERROR: Safety pattern mismatch for %s ('%s' vs. '%s') rc=%d",
          name.c_str(), value.c_str(), re.c_str(), rc));
}


int regexp_match (const string& value, const string& re, vector<string>& matches)
{
  typedef map<string,regex_t*> cache;  // separate cache because we use different regcomp options
  static cache compiled;
  cache::iterator it = compiled.find (re);
  regex_t* r = 0;
  if (it == compiled.end())
    {
      r = new regex_t;
      int rc = regcomp (r, re.c_str(), REG_EXTENDED); /* REG_ICASE? */
      assert (rc == 0);
      compiled[re] = r;
    }
  else
    r = it->second;


  // run regexec
#define maxmatches 10
  regmatch_t rm[maxmatches];

  int rc = regexec (r, value.c_str(), maxmatches, rm, 0);
  if (rc) return rc;

  matches.erase(matches.begin(), matches.end());
  for (unsigned i=0; i<maxmatches; i++) // XXX: ideally, the number of actual subexpressions in re
    {
      if (rm[i].rm_so >= 0)
        matches.push_back(value.substr (rm[i].rm_so, rm[i].rm_eo-rm[i].rm_so));
      else
        matches.push_back("");
    }

  return 0;
}


bool contains_glob_chars (const string& str)
{
  for (unsigned i=0; i<str.size(); i++)
    {
      char this_char = str[i];
      if (this_char == '\\' && (str.size() > i+1))
        {
          // PR13338: skip the escape backslash and the escaped character
          i++;
          continue;
        }
      if (this_char == '*' || this_char == '?' || this_char == '[')
        return true;
    }

  return false;
}


// PR13338: we need these functions to be able to pass through glob metacharacters
// through the recursive process("...*...") expansion process.
string escape_glob_chars (const string& str)
{
  string op;
  for (unsigned i=0; i<str.size(); i++)
    {
      char this_char = str[i];
      if (this_char == '*' || this_char == '?' || this_char == '[')
        op += '\\';
      op += this_char;
    }
  return op;
}

string unescape_glob_chars (const string& str)
{
  string op;
  for (unsigned i=0; i<str.size(); i++)
    {
      char this_char = str[i];
      if (this_char == '\\' && (str.size() > i+1) )
        {
          op += str[i+1];
          i++;
          continue;
        }
      op += this_char;
    }

  return op;
}

bool identifier_string_needs_escape (const string& str)
{
  for (unsigned i = 0; i < str.size (); i++)
    {
      char this_char = str[i];
      if (! isalnum (this_char) && this_char != '_')
	return true;
    }

  return false;
}

string escaped_indentifier_string (const string &str)
{
  if (! identifier_string_needs_escape (str))
    return str;

  string op;
  for (unsigned i = 0; i < str.size (); i++)
    {
      char this_char = str[i];
      if (! isalnum (this_char) && this_char != '_')
	{
	  char b[32];
	  sprintf (b, "_%x_", (unsigned int) this_char);
	  op += b;
        }
      else
	op += this_char;
    }

  return op;
}

string
normalize_machine(const string& machine)
{
  // PR4186: Copy logic from coreutils uname (uname -i) to squash
  // i?86->i386.  Actually, copy logic from linux top-level Makefile
  // to squash uname -m -> $(SUBARCH).
  //
  // This logic needs to match the logic in the stap_get_arch shell
  // function in stap-env.
  //
  // But: RHBZ669082 reminds us that this renaming post-dates some
  // of the kernel versions we know and love.  So in buildrun.cxx
  // we undo this renaming for ancient powerpc.
  //
  // NB repeated: see also stap-env (stap_get_arch)
  if (machine == "i486") return "i386";
  else if (machine == "i586") return "i386";
  else if (machine == "i686") return "i386";
  else if (machine == "sun4u") return "sparc64";
  else if (machine.substr(0,3) == "arm") return "arm";
  else if (machine == "sa110") return "arm";
  else if (machine == "s390x") return "s390";
  else if (machine == "aarch64") return "arm64";
  else if (machine.substr(0,3) == "ppc") return "powerpc";
  else if (machine.substr(0,4) == "mips") return "mips";
  else if (machine.substr(0,3) == "sh2") return "sh";
  else if (machine.substr(0,3) == "sh3") return "sh";
  else if (machine.substr(0,3) == "sh4") return "sh";
  // NB repeated: see also stap-env (stap_get_arch)
  return machine;
}

int
elf_class_from_normalized_machine (const string &machine)
{
  // Must match kernel machine architectures as used un tapset directory.
  // And must match normalization done in normalize_machine ().
  if (machine == "i386"
      || machine == "arm")         // arm assumes 32-bit
    return ELFCLASS32;
  else if (machine == "s390"       // powerpc and s390 always assume 64-bit,
           || machine == "powerpc" // see normalize_machine ().
           || machine == "x86_64"
           || machine == "ia64"
           || machine == "arm64")
    return ELFCLASS64;

  cerr << _F("Unknown kernel machine architecture '%s', don't know elf class",
             machine.c_str()) << endl;
  return -1;
}

string
kernel_release_from_build_tree (const string &kernel_build_tree, int verbose)
{
  string version_file_name = kernel_build_tree + "/include/config/kernel.release";
  // The file include/config/kernel.release within the
  // build tree is used to pull out the version information
  ifstream version_file (version_file_name.c_str());
  if (version_file.fail ())
    {
      if (verbose > 1)
	//TRANSLATORS: Missing a file
	cerr << _F("Missing %s", version_file_name.c_str()) << endl;
      return "";
    }

  string kernel_release;
  char c;
  while (version_file.get(c) && c != '\n')
    kernel_release.push_back(c);

  return kernel_release;
}

string autosprintf(const char* format, ...)
{
  va_list args;
  char *str;
  va_start (args, format);
  int rc = vasprintf (&str, format, args);
  if (rc < 0)
    {
      va_end(args);
      return _F("autosprintf/vasprintf error %d", rc);
    }
  string s = str;
  va_end (args);
  free (str);
  return s; /* by copy */
}

string
get_self_path()
{
  char buf[1024]; // This really should be enough for anybody...
  const char *file = "/proc/self/exe";
  ssize_t len = readlink(file, buf, sizeof(buf) - 1);
  if (len > 0)
    {
      buf[len] = '\0';
      file = buf;
    }
  // otherwise the path is ridiculously large, fall back to /proc/self/exe.
  //
  return string(file);
}

bool
is_valid_pid (pid_t pid, string& err_msg)
{
  err_msg = "";
  if (pid <= 0)
    {
      err_msg = _F("cannot probe pid %d: Invalid pid", pid);
      return false;
    }
  else if (kill(pid, 0) == -1)
    {
      err_msg = _F("cannot probe pid %d: %s", pid, strerror(errno));
      return false;
    }
  return true;
}

// String sorter using the Levenshtein algorithm
// TODO: Performance may be improved by adding a maximum distance
// parameter which would abort the operation if we know the final
// distance will be larger than the maximum. This may entail maintaining
// another data structure, and thus the cost might outweigh the benefit
unsigned
levenshtein(const string& a, const string& b)
{
  Array2D<unsigned> d(a.size()+1, b.size()+1);

  // border values
  for (unsigned i = 0; i < d.width; i++)
    d(i, 0) = i;
  for (unsigned j = 0; j < d.height; j++)
    d(0, j) = j;

  // the meat
  for (unsigned i = 1; i < d.width; i++) {
    for (unsigned j = 1; j < d.height; j++) {
      if (a[i-1] == b[j-1]) // match
        d(i,j) = d(i-1, j-1);
      else // penalties open for adjustments
        {
          unsigned subpen = 2; // substitution penalty
          // check if they are upper/lowercase related
          if (tolower(a[i-1]) == tolower(b[j-1]))
            subpen = 1; // half penalty
          d(i,j) = min(min(
              d(i-1,j-1) + subpen,  // substitution
              d(i-1,j)   + 2), // deletion
              d(i,j-1)   + 2); // insertion
        }
    }
  }

  return d(d.width-1, d.height-1);
}

// Returns comma-separated list of set elements closest to the target string.
// Print a maximum amount of 'max' elements, with a maximum levenshtein score
// of 'threshold'.
string
levenshtein_suggest(const string& target,        // string to match against
                    const set<string>& elems,    // elements to suggest from
                    unsigned max,                // max elements to print
                    unsigned threshold)          // max leven score to print
{
  // calculate leven score for each elem and put in map
  multimap<unsigned, string> scores;
  for (set<string>::const_iterator it = elems.begin();
                                      it != elems.end(); ++it)
    {
      if (it->empty()) // skip empty strings
        continue;

      // Approximate levenshtein by size-difference only; real score
      // is at least this high
      unsigned min_score = labs(target.size() - it->size());

      if (min_score > threshold) // min-score too high for threshold
        continue;

      /* Check if we can skip calculating the score for this element. This works
       * on knowing two facts:
       * (1) We will only print the 'max' number of the top elements. The
       *     current top 'max' candidates reside in the scores map already.
       * (2) The score will be AT LEAST the difference between the lengths of
       *     the two strings.
       * So what we do is retrieve the WORST score of the current best
       * candidates by iterating through the map (which is ordered) and
       * retrieving the 'max-th' item and check if that's still better than the
       * BEST score we could possibly get from the two strings (by comparing
       * their lengths). If the 'max-th' item is indeed better, then we know
       * this element will NEVER make it to the terminal. So we just skip it and
       * move on. Quite tragic if you ask me...
       */
      unsigned maxth_score = std::numeric_limits<unsigned>::max();
      if (scores.size() >= max) // do we have at least 'max' items?
        {
          // retrieve 'max-th' item
          multimap<unsigned, string>::iterator itt = scores.begin();
          for (unsigned i = 0; i < max-1; i++) itt++; // will not go to .end()
          maxth_score = itt->first;
        }

      if (min_score > maxth_score) // min-score too high for known candidates
        continue;

      unsigned score = levenshtein(target, *it);

      if (score > maxth_score) // actual score too high for known candidates
        continue;

      if (score > threshold) // actual score too high for threshold
        continue;

      // a candidate!
      scores.insert(make_pair(score, *it));
    }

  string suggestions;

  // Print out the top 'max' elements
  multimap<unsigned, string>::iterator it = scores.begin();
  for (unsigned i = 0; it != scores.end() && i < max; ++it, i++)
    suggestions += it->second + ", ";
  if (!suggestions.empty())
    suggestions.erase(suggestions.size()-2);

  return suggestions;
}

string
levenshtein_suggest(const string& target,        // string to match against
                    const set<interned_string>& elems,// elements to suggest from
                    unsigned max,                // max elements to print
                    unsigned threshold)          // max leven score to print
{
  set<string> elems2;
  for (set<interned_string>::const_iterator it = elems.begin();
       it != elems.end();
       it++)
    elems2.insert(it->to_string());
  
  return levenshtein_suggest (target, elems2, max, threshold);
  
}


#ifndef HAVE_PPOLL
// This is a poor-man's ppoll, only used carefully by readers that need to be
// interruptible, like remote::run and mutator::run.  It does not provide the
// same guarantee of atomicity as on systems with a true ppoll.
//
// In our use, this would cause trouble if a signal came in any time from the
// moment we mask signals to prepare pollfds, to the moment we call poll in
// emulation here.  If there's no data on any of the pollfds, we will be stuck
// waiting indefinitely.
//
// Since this is mainly about responsiveness of CTRL-C cleanup, we'll just
// throw in a one-second forced timeout to ensure we have a chance to notice
// there was an interrupt without too much delay.
int
ppoll(struct pollfd *fds, nfds_t nfds,
      const struct timespec *timeout_ts,
      const sigset_t *sigmask)
{
  sigset_t origmask;
  int timeout = (timeout_ts == NULL) ? 1000 // don't block forever...
    : (timeout_ts->tv_sec * 1000 + timeout_ts->tv_nsec / 1000000);
  sigprocmask(SIG_SETMASK, sigmask, &origmask);
  int rc = poll(fds, nfds, timeout);
  sigprocmask(SIG_SETMASK, &origmask, NULL);
  return rc;
}
#endif


int
read_from_file (const string &fname, int &data)
{
  // C++ streams may not set errno in the even of a failure. However if we
  // set it to 0 before each operation and it gets set during the operation,
  // then we can use its value in order to determine what happened.
  errno = 0;
  ifstream f (fname.c_str ());
  if (! f.good ())
    {
      clog << _F("Unable to open file '%s' for reading: ", fname.c_str());
      goto error;
    }

  // Read the data;
  errno = 0;
  f >> data;
  if (f.fail ())
    {
      clog << _F("Unable to read from file '%s': ", fname.c_str());
      goto error;
    }

  // NB: not necessary to f.close ();
  return 0; // Success

 error:
  if (errno)
    clog << strerror (errno) << endl;
  else
    clog << _("unknown error") << endl;
  return 1; // Failure
}

template <class T>
int
write_to_file (const string &fname, const T &data)
{
  // C++ streams may not set errno in the even of a failure. However if we
  // set it to 0 before each operation and it gets set during the operation,
  // then we can use its value in order to determine what happened.
  errno = 0;
  ofstream f (fname.c_str ());
  if (! f.good ())
    {
      clog << _F("Unable to open file '%s' for writing: ", fname.c_str());
      goto error;
    }

  // Write the data;
  f << data;
  errno = 0;
  if (f.fail ())
    {
      clog << _F("Unable to write to file '%s': ", fname.c_str());
      goto error;
    }

  // NB: not necessary to f.close ();
  return 0; // Success

 error:
  if (errno)
    clog << strerror (errno) << endl;
  else
    clog << _("unknown error") << endl;
  return 1; // Failure
}

// Let's go ahead an instantiate a few variants of the write_to_file()
// templated function.
template int write_to_file (const string &fname, const string &data);
template int write_to_file (const string &fname, const int &data);

int
flush_to_stream (const string &fname, ostream &o)
{
  // C++ streams may not set errno in the even of a failure. However if we
  // set it to 0 before each operation and it gets set during the operation,
  // then we can use its value in order to determine what happened.
  errno = 0;
  ifstream f (fname.c_str ());
  if (! f.good ())
    {
      clog << _F("Unable to open file '%s' for reading: ", fname.c_str());
      goto error;
    }

  // Stream the data

  // NB: o << f.rdbuf() misbehaves for some reason, appearing to close o,
  // which is unfortunate if o == clog or cout.
  while (1)
    {
      errno = 0;
      int c = f.get();
      if (f.eof ()) return 0; // normal exit
      if (! f.good()) break;
      o.put(c);
      if (! o.good()) break;
    }

  // NB: not necessary to f.close ();

 error:
  if (errno)
    clog << strerror (errno) << endl;
  else
    clog << _("unknown error") << endl;
  return 1; // Failure
}

// Tries to determine the name and version of the running Linux OS
// distribution. Fills in the 'info' argument with (name, version)
// strings. Returns true if it was able to retrieve the information.
bool
get_distro_info(vector<string> &info)
{
    string name, version;

    // Getting the distro name and version is harder than it should
    // be. We've got a multi-pronged strategy.
    //
    // (1) First, try the "lsb_release" executable, which may or may
    // not exist on the system.
    vector<string> cmd { "lsb_release", "--short", "--id" };
    stringstream out;
    int rc = stap_system_read(0, cmd, out);
    if (rc == 0) {
	name = out.str();

	vector<string> cmd2 { "lsb_release", "--short", "--release" };
	rc = stap_system_read(0, cmd2, out);
	if (rc == 0) {
	    version = out.str();
	}
    }

    // (2) Look for the /etc/os-release file.
    if (name.empty()) {
	ifstream infile;
	infile.open("/etc/os-release");
	if (infile.is_open()) {
	    string line;
	    while (getline(infile, line)) {
		vector<string> components;
		tokenize(line, components, "=");
		transform(components[0].begin(), components[0].end(),
			  components[0].begin(), ::tolower);
		if (components[0] == "name") {
		    name = components[1];
		}
		else if (components[0] == "version_id") {
		    version = components[1];
		}
	    }
	    infile.close();
	}
    }

    // (3) Here we could look for /etc/*-release ('redhat', 'arch', 'gentoo',
    // etc.) or /etc/*_version ('debian', etc.), if needed.

    info.clear();
    if (! name.empty()) {
	info.push_back(name);
	info.push_back(version);
	return true;
    }
    return false;
}

/* vim: set sw=2 ts=8 cino=>4,n-2,{2,^-2,t0,(0,u0,w1,M1 : */
