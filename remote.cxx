// systemtap remote execution
// Copyright (C) 2010-2015 Red Hat Inc.
//
// This file is part of systemtap, and is free software.  You can
// redistribute it and/or modify it under the terms of the GNU General
// Public License (GPL); either version 2, or (at your option) any
// later version.

#include "config.h"

extern "C" {
#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
}

#include <cstdio>
#include <iomanip>
#include <memory>
#include <stdexcept>
#include <sstream>
#include <string>
#include <vector>

#include "buildrun.h"
#include "remote.h"
#include "util.h"

using namespace std;

// Decode URIs as per RFC 3986, though not bothering to be strict
class uri_decoder {
  public:
    const string uri;
    string scheme, authority, path, query, fragment;
    bool has_authority, has_query, has_fragment;

    uri_decoder(const string& uri):
      uri(uri), has_authority(false), has_query(false), has_fragment(false)
    {
      const string re =
        "^([^:]+):(//[^/?#]*)?([^?#]*)(\\?[^#]*)?(#.*)?$";

      vector<string> matches;
      if (regexp_match(uri, re, matches) != 0)
        throw runtime_error(_F("string doesn't appear to be a URI: %s", uri.c_str()));

      scheme = matches[1];

      if (!matches[2].empty())
        {
          has_authority = true;
          authority = matches[2].substr(2);
        }

      path = matches[3];

      if (!matches[4].empty())
        {
          has_query = true;
          query = matches[4].substr(1);
        }

      if (!matches[5].empty())
        {
          has_fragment = true;
          fragment = matches[5].substr(1);
        }
    }
};


// loopback target for running locally
class direct : public remote {
  private:
    pid_t child;
    vector<string> args;
    direct(systemtap_session& s): remote(s), child(0) {}

    int start()
      {
        args = make_run_command(*s);
        if (! staprun_r_arg.empty()) // PR13354
          {
            if (s->runtime_mode == systemtap_session::dyninst_runtime)
              args.insert(args.end(), {"_stp_dyninst_remote=" + staprun_r_arg});
            else
              args.insert(args.end(), { "-r", staprun_r_arg });
          }
        pid_t pid = stap_spawn (s->verbose, args);
        if (pid <= 0)
          return 1;
        child = pid;
        return 0;
      }

    int finish()
      {
        if (child <= 0)
          return 1;

        int ret = stap_waitpid(s->verbose, child);
        if (ret > 128)
          s->print_warning(_F("%s exited with signal: %d (%s)",
                              args.front().c_str(), ret - 128,
                              strsignal(ret - 128)));
        else if (ret > 0)
          s->print_warning(_F("%s exited with status: %d",
                              args.front().c_str(), ret));
        child = 0;
        return ret;
      }

  public:
    friend class remote;

    virtual ~direct() { finish(); }
};


class stapsh : public remote {
  private:
    int interrupts_sent;
    int fdin, fdout;
    FILE *IN, *OUT;
    string remote_version;
    size_t data_size;
    string target_stream;

    enum {
      STAPSH_READY, // ready to receive next command
      STAPSH_DATA   // currently printing data from a 'data' command
    } stream_state;

    virtual void prepare_poll(vector<pollfd>& fds)
      {
        if (fdout >= 0 && OUT)
          {
            pollfd p = { fdout, POLLIN, 0 };
            fds.push_back(p);
          }

        // need to send a signal?
        if (fdin >= 0 && IN && interrupts_sent < pending_interrupts)
          {
            pollfd p = { fdin, POLLOUT, 0 };
            fds.push_back(p);
          }
      }

    virtual void handle_poll(vector<pollfd>& fds)
      {
        for (unsigned i=0; i < fds.size(); ++i)
          if (fds[i].fd == fdin || fds[i].fd == fdout)
            {
              bool err = false;

              // need to send a signal?
              if (fds[i].revents & POLLOUT && IN &&
                  interrupts_sent < pending_interrupts)
                {
                  if (send_command("quit\n") == 0)
                    ++interrupts_sent;
                  else
                    err = true;
                }

              // have data to read?
              if (fds[i].revents & POLLIN && OUT)
                {
                  // if stapsh doesn't support commands, then just dump everything
                  if (!vector_has(options, string("data")))
                    {
                      // NB: we could splice here, but we don't know how much
                      // data is available for reading. One way could be to
                      // splice in small chunks and poll() fdout to check if
                      // there's more.
                      char buf[4096];
                      size_t bytes_read;
                      while ((bytes_read = fread(buf, 1, sizeof(buf), OUT)) > 0)
                        printout(buf, bytes_read);
                      if (errno != EAGAIN)
                        err = true;
                    }
                  else // we expect commands (just data for now)
                    {
                      // When the 'data' option is turned on, all outputs from
                      // staprun are first prepended with a line formatted as
                      // "data <stream> <size>\n", where <stream> is either
                      // "stdout" or "stderr", and <size> is the size of the
                      // data. Also, the line "quit\n" is sent from stapsh right
                      // before it exits.

                      if (stream_state == STAPSH_READY)
                        {
                          char cmdbuf[1024];
                          char *rc;
                          while ((rc = fgets(cmdbuf, sizeof(cmdbuf), OUT)) != NULL)
                            {
                              // check if it's a debug output from stapsh itself
                              string line = string(cmdbuf);
                              if (startswith(line, "stapsh:"))
                                clog << line;
                              else if (startswith(line, "quit\n"))
                                  // && vector_has(options, string("data")))
                                  // uncomment above if another command becomes
                                  // possible
                                {
                                  err = true; // close connection
                                  break;
                                }
                              else if (startswith(line, "data"))
                                  // && vector_has(options, string("data")))
                                  // uncomment above if another command becomes
                                  // possible
                                {
                                  vector<string> cmd;
                                  tokenize(line, cmd, " \t\r\n");
                                  if (!is_valid_data_cmd(cmd))
                                    {
                                      clog << _("invalid data command from stapsh") << endl;
                                      clog << _("received: ") << line;
                                      err = true;
                                    }
                                  else
                                    {
                                      target_stream = cmd[1];
                                      data_size = lex_cast<size_t>(cmd[2]);
                                      stream_state = STAPSH_DATA;
                                    }
                                  break;
                                }
                            }
                          if (rc == NULL && errno != EAGAIN)
                            err = true;
                        }

                      if (stream_state == STAPSH_DATA)
                        {
                          if (data_size != 0)
                            {
                              // keep reading from OUT until either dry or data_size bytes done
                              char buf[4096];
                              size_t max_read = min<size_t>(sizeof(buf), data_size);
                              size_t bytes_read = 0;
                              while (max_read > 0
                                  && (bytes_read = fread(buf, 1, max_read, OUT)) > 0)
                                {
                                  printout(buf, bytes_read);
                                  data_size -= bytes_read;
                                  max_read = min<size_t>(sizeof(buf), data_size);
                                }
                              if (bytes_read == 0 && errno != EAGAIN)
                                err = true;
                            }
                          if (data_size == 0)
                            stream_state = STAPSH_READY;
                        }
                    }
                }

              // any errors?
              if (err || fds[i].revents & ~(POLLIN|POLLOUT))
                close();
            }
      }

    string get_reply()
      {
        // Some schemes like unix may have stdout and stderr mushed together.
        // There shouldn't be anything except dbug messages on stderr before we
        // actually start running, and there's no get_reply after that.  So
        // we'll just loop and skip those that start with "stapsh:".
        char reply[4096];
        while (fgets(reply, sizeof(reply), OUT))
          {
            if (!startswith(reply, "stapsh:"))
              return reply;

            // if data is not prefixed, we will have no way to distinguish
            // between stdout and stderr during staprun runtime, so we might as
            // well print to stdout now as well to be more consistent
            if (vector_has(options, string("data")))
              clog << reply; // must be stderr since only replies go to stdout
            else
              cout << reply; // output to stdout to be more consistent with later
          }

        // Reached EOF, nothing to reply...
        return "";
      }

    int send_command(const string& cmd)
      {
        if (!IN)
          return 2;
        if (fputs(cmd.c_str(), IN) < 0 ||
            fflush(IN) != 0)
          return 1;
        return 0;
      }

    int send_file(const string& filename, const string& dest)
      {
        int rc = 0;
        FILE* f = fopen(filename.c_str(), "r");
        if (!f)
          return 1;

        struct stat fs;
        rc = fstat(fileno(f), &fs);
        if (!rc)
          {
            ostringstream cmd;
            cmd << "file " << fs.st_size << " " << dest << "\n";
            rc = send_command(cmd.str());
          }

        off_t i = 0;
        while (!rc && i < fs.st_size)
          {
            char buf[4096];
            size_t r = sizeof(buf);
            if (fs.st_size - i < (off_t)r)
              r = fs.st_size - i;
            r = fread(buf, 1, r, f);
            if (r == 0)
              rc = 1;
            else
              {
                size_t w = fwrite(buf, 1, r, IN);
                if (w != r)
                  rc = 1;
                else
                  i += w;
              }
          }
        if (!rc)
          rc = fflush(IN);

        fclose(f);

        if (!rc)
          {
            string reply = get_reply();
            if (reply != "OK\n")
              {
                rc = 1;
                if (s->verbose > 0)
                  {
                    if (reply.empty())
                      clog << _("stapsh file ERROR: no reply") << endl;
                    else
                      clog << _F("stapsh file replied %s", reply.c_str());
                  }
              }
          }

        return rc;
      }

    static string qpencode(const string& str)
      {
        ostringstream o;
        o << setfill('0') << hex;
        for (const char* s = str.c_str(); *s; ++s)
          if (*s >= 33 && *s <= 126 && *s != 61)
            o << *s;
          else
            o << '=' << setw(2) << (unsigned)(unsigned char) *s;
        return o.str();
      }

    static bool is_valid_data_cmd(const vector<string>& cmd)
      {
        bool err = false;

        err = err || (cmd[0] != "data");
        err = err || (cmd[1] != "stdout" && cmd[1] != "stderr");

        if (!err) // try to cast as size_t
          try { lex_cast<size_t>(cmd[2]); }
          catch (...) { err = true; }

        return !err;
      }

    virtual void printout(const char *buf, size_t size)
      {
        static string last_prefix;
        static bool on_same_line;

        if (size == 0)
          return;

        // don't prefix for stderr to be more consistent with ssh
        if (!prefix.empty() && target_stream != "stderr")
          {
            vector<pair<const char*,int> > lines = split_lines(buf, size);
            for (vector<pair<const char*,int> >::iterator it = lines.begin();
                it != lines.end(); ++it)
              {
                if (last_prefix != prefix)
                  {
                    if (on_same_line)
                      cout << endl;
                    cout << prefix;
                  }
                else if (!on_same_line)
                  cout << prefix;
                cout.write(it->first, it->second);
              }
            cout.flush();
            const char *last_line = lines.back().first;
            const char last_char = last_line[lines.back().second-1];
            on_same_line = !lines.empty() && last_char != '\n';
            last_prefix = prefix;
          }
        else
          {
            // Otherwise dump the whole block
            // NB: The buf could contain binary data,
            // including \0, so write as a block instead of
            // the usual << string.
            if (target_stream == "stdout")
              {
                cout.write(buf, size);
                cout.flush();
              }
            else // stderr
              clog.write(buf, size);
          }
      }

  protected:
    stapsh(systemtap_session& s)
      : remote(s), interrupts_sent(0),
        fdin(-1), fdout(-1), IN(0), OUT(0),
        data_size(0), target_stream("stdout"), // default to stdout for schemes
        stream_state(STAPSH_READY)         // that don't pipe stderr (e.g. ssh)
      {}

    vector<string> options;

    virtual int prepare()
      {
        int rc = 0;

        string extension = s->runtime_mode == systemtap_session::dyninst_runtime ? ".so" : ".ko";
        string localmodule = s->tmpdir + "/" + s->module_name + extension;
        string remotemodule = s->module_name + extension;
        if ((rc = send_file(localmodule, remotemodule)))
          return rc;

        if (file_exists(localmodule + ".sgn") &&
            (rc = send_file(localmodule + ".sgn", remotemodule + ".sgn")))
          return rc;

        if (!s->uprobes_path.empty())
          {
            string remoteuprobes = basename(s->uprobes_path.c_str());
            if ((rc = send_file(s->uprobes_path, remoteuprobes)))
              return rc;

            if (file_exists(s->uprobes_path + ".sgn") &&
                (rc = send_file(s->uprobes_path + ".sgn", remoteuprobes + ".sgn")))
              return rc;
          }

        return rc;
      }

    virtual int start()
      {
        // Send the staprun args
        // NB: The remote is left to decide its own staprun path
        ostringstream run("run", ios::out | ios::ate);
        vector<string> cmd = make_run_command(*s, ".", remote_version);

        // PR13354: identify our remote index/url
        if (strverscmp("1.7", remote_version.c_str()) <= 0 && // -r supported?
            ! staprun_r_arg.empty())
          {
             if (s->runtime_mode == systemtap_session::dyninst_runtime)
	       cmd.insert(cmd.end(), { "_stp_dyninst_remote=" +  staprun_r_arg});
             else
               cmd.insert(cmd.end(), { "-r", staprun_r_arg });
          }

        for (unsigned i = 1; i < cmd.size(); ++i)
          run << ' ' << qpencode(cmd[i]);
        run << '\n';

        int rc = send_command(run.str());

        if (!rc)
          {
            string reply = get_reply();
            if (reply != "OK\n")
              {
                rc = 1;
                if (s->verbose > 0)
                  {
                    if (reply.empty())
                      clog << _("stapsh run ERROR: no reply") << endl;
                    else
                      clog << _F("stapsh run replied %s", reply.c_str());
                  }
              }
          }

        if (!rc)
          {
            long flags;
            if ((flags = fcntl(fdout, F_GETFL)) == -1
              || fcntl(fdout, F_SETFL, flags | O_NONBLOCK) == -1)
              {
                clog << _("failed to change to non-blocking mode") << endl;
                rc = 1;
              }
          }

        if (rc)
          // If run failed for any reason, then this
          // connection is effectively dead to us.
          close();

        return rc;
      }

    void close()
      {
        if (IN) fclose(IN);
        if (OUT) fclose(OUT);
        IN = OUT = NULL;
        fdin = fdout = -1;
      }

    virtual int finish()
      {
        close();
        return 0;
      }

    void set_child_fds(int in, int out)
      {
        if (fdin >= 0 || fdout >= 0 || IN || OUT)
          throw runtime_error(_("stapsh file descriptors already set"));

        fdin = in;
        fdout = out;
        IN = fdopen(fdin, "w");
        OUT = fdopen(fdout, "r");
        if (!IN || !OUT)
          throw runtime_error(_("invalid file descriptors for stapsh"));

        if (send_command("stap " VERSION "\n"))
          throw runtime_error(_("error sending hello to stapsh"));

        string reply = get_reply();
        if (reply.empty())
          throw runtime_error(_("error receiving hello from stapsh"));

        // stapsh VERSION MACHINE RELEASE
        vector<string> uname;
        tokenize(reply, uname, " \t\r\n");
        if (uname.size() != 4 || uname[0] != "stapsh")
          throw runtime_error(_F("invalid hello from stapsh: %s", reply.c_str()));

        // We assume that later versions will know how to talk to us.
        // Looking backward, we use this for make_run_command().
        this->remote_version = uname[1];

        this->s = s->clone(uname[2], uname[3]);

        // set any option requested
        if (!this->options.empty())
          {
            // check first if the option command is supported
            if (strverscmp("2.4", this->remote_version.c_str()) > 0)
              throw runtime_error(_F("stapsh %s does not support options",
                                            this->remote_version.c_str()));

            for (vector<string>::iterator it = this->options.begin();
                it != this->options.end(); ++it)
              {
                int rc = send_command("option " + *it + "\n");
                if (rc != 0)
                  throw runtime_error(_F("could not set option %s: "
                                         "send_command returned %d",
                                         it->c_str(), rc));
                string reply = get_reply();
                if (reply != "OK\n")
                  throw runtime_error(_F("could not set option %s: %s",
                                          it->c_str(), reply.c_str()));
              }
          }
      }

  public:
    virtual ~stapsh() { close(); }
};


// direct_stapsh is meant only for testing, as a way to exercise the stapsh
// mechanisms without requiring test machines to have actual remote access.
class direct_stapsh : public stapsh {
  private:
    pid_t child;

    direct_stapsh(systemtap_session& s)
      : stapsh(s), child(0)
      {
        int in, out;
        vector<string> cmd { BINDIR "/stapsh" };
        if (s.perpass_verbose[4] > 1)
          cmd.push_back("-v");
        if (s.perpass_verbose[4] > 2)
          cmd.push_back("-v");

        // mask signals while we spawn, so we can simulate manual signals to
        // the "remote" target, as we must for the real ssh_remote case.
        {
          stap_sigmasker masked;
          child = stap_spawn_piped(s.verbose, cmd, &in, &out);
        }

        if (child <= 0)
          throw runtime_error(_("error launching stapsh"));

        try
          {
            set_child_fds(in, out);
          }
        catch (runtime_error&)
          {
            finish();
            throw;
          }
      }

    virtual int finish()
      {
        int rc = stapsh::finish();
        if (child <= 0)
          return rc;

        int rc2 = stap_waitpid(s->verbose, child);
        child = 0;
        return rc ?: rc2;
      }

  public:
    friend class remote;

    virtual ~direct_stapsh() { finish(); }
};


// Connect to an existing stapsh on a unix socket.
class unix_stapsh : public stapsh {
  private:

    unix_stapsh(systemtap_session& s, const uri_decoder& ud)
      : stapsh(s)
      {
        // Request that data be encapsulated since both stdout and stderr have
        // to go over the same line. Also makes stapsh tell us when it quits.
        this->options.push_back("data");

        // set verbosity to the requested level
        for (unsigned i = 1; i < s.perpass_verbose[4]; i++)
          this->options.push_back("verbose");

        sockaddr_un server;
        server.sun_family = AF_UNIX;
        if (ud.path.empty())
          throw runtime_error(_("unix target requires a /path"));
        if (ud.path.size() > sizeof(server.sun_path) - 1)
          throw runtime_error(_("unix target /path is too long"));
        strcpy(server.sun_path, ud.path.c_str());

        if (ud.has_authority)
          throw runtime_error(_("unix target doesn't support a hostname"));
        if (ud.has_query)
          throw runtime_error(_("unix target URI doesn't support a ?query"));
        if (ud.has_fragment)
          throw runtime_error(_("unix target URI doesn't support a #fragment"));

        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd <= 0)
          throw runtime_error(_("error opening a socket"));

        if (connect(fd, (struct sockaddr *)&server, SUN_LEN(&server)) < 0)
          {
            const char *msg = strerror(errno);
            ::close(fd);
            throw runtime_error(_F("error connecting to socket %s: %s",
                                    server.sun_path, msg));
          }

        // Try to dup it, so class stapsh can have truly separate fds for its
        // fdopen handles.  If it fails for some reason, it can still work with
        // just one though.
        int fd2 = dup(fd);
        if (fd2 < 0)
          fd2 = fd;

        try
          {
            set_child_fds(fd, fd2);
          }
        catch (runtime_error&)
          {
            finish();
            ::close(fd);
            ::close(fd2);
            throw;
          }
      }

  public:
    friend class remote;

    virtual ~unix_stapsh() { finish(); }
};

class libvirt_stapsh : public stapsh {
  private:

    pid_t child;

    libvirt_stapsh(systemtap_session& s, const uri_decoder& ud)
      : stapsh(s)
      {
        string domain;
        string libvirt_uri;

        // Request that data be encapsulated since both stdout and stderr have
        // to go over the same line. Also makes stapsh tell us when it quits.
        this->options.push_back("data");

        // set verbosity to the requested level
        for (unsigned i = 1; i < s.perpass_verbose[4]; i++)
          this->options.push_back("verbose");

        // A valid libvirt URI has one of two forms:
        //  - libvirt://DOMAIN/LIBVIRT_URI?LIBVIRT_QUERY
        //  - libvirt:DOMAIN/LIBVIRT_URI?LIBVIRT_QUERY
        // We only advertise the first form, but to be nice, we also accept the
        // second form. In the first form, DOMAIN is authority, LIBVIRT_URI is
        // path, and LIBVIRT_QUERY is the query. In the second form, the DOMAIN
        // is part of the path.

        if (ud.has_fragment)
          throw runtime_error(_("libvirt target URI doesn't support a #fragment"));

        if (ud.has_authority) // first form
          {
            domain = ud.authority;
            if (!ud.path.empty())
              {
                libvirt_uri = ud.path.substr(1);
                if (ud.has_query)
                  libvirt_uri += "?" + ud.query;
              }
          }
        else // second form
          {
            if (ud.path.empty())
              throw runtime_error(_("libvirt target URI requires a domain name"));

            size_t slash = ud.path.find_first_of('/');
            if (slash == string::npos)
              domain = ud.path;
            else
              {
                domain = ud.path.substr(0, slash);
                libvirt_uri = ud.path.substr(slash+1);
              }
          }

        int in, out;

        string stapvirt = BINDIR "/stapvirt";
        if (!file_exists(stapvirt))
          throw runtime_error(_("stapvirt missing"));

        vector<string> cmd { stapvirt };

        // carry verbosity into stapvirt
        if (s.perpass_verbose[4] > 0)
          cmd.insert(cmd.end(), s.perpass_verbose[4] - 1, "-v");

        if (!libvirt_uri.empty())
          cmd.insert(cmd.end(), { "-c", libvirt_uri });

        cmd.insert(cmd.end(), { "connect", domain });

        // mask signals for stapvirt since it relies instead on stap or stapsh
        // closing its connection to know when to exit (otherwise, stapvirt
        // would receive SIGINT on Ctrl-C and exit nonzero)

        // NB: There is an interesting issue to note here. Some libvirt URIs
        // will create child processes of stapvirt (e.g. qemu+ssh:// will
        // create an ssh process under stapvirt, also qemu+ext://, see
        // <libvirt.org/remote.html> for more info). These processes do not
        // keep the mask of stapvirt we are creating here, but are still part
        // of the same process group.
        //     This means that e.g. SIGINT will not be blocked. Thus, stapvirt
        // explicitly adds a handler for SIGCHLD and forcefully disconnects
        // upon receiving it (otherwise it would await indefinitely).
        {
          stap_sigmasker masked;
          child = stap_spawn_piped(s.verbose, cmd, &in, &out);
        }

        if (child <= 0)
          throw runtime_error(_("error launching stapvirt"));

        try
          {
            set_child_fds(in, out);
          }
        catch (runtime_error&)
          {
            finish();
            throw;
          }
      }

    int finish()
      {
        int rc = stapsh::finish();
        if (child <= 0)
          return rc;

        int rc2 = stap_waitpid(s->verbose, child);
        child = 0;
        return rc ?: rc2;
      }

    public:
      friend class remote;

      virtual ~libvirt_stapsh() { finish(); }
};

// stapsh-based ssh_remote
class ssh_remote : public stapsh {
  private:
    pid_t child;

    ssh_remote(systemtap_session& s): stapsh(s), child(0) {}

    int connect(const string& host, const string& port)
      {
        int rc = 0;
        int in, out;
        vector<string> cmd { "ssh", "-q", host };
        if (!port.empty())
          cmd.insert(cmd.end(), { "-p", port });

        // This is crafted so that we get a silent failure with status 127 if
        // the command is not found.  The combination of -P and $cmd ensures
        // that we pull the command out of the PATH, not aliases or such.
        string stapsh_cmd = "cmd=`type -P stapsh || exit 127` && exec \"$cmd\"";
        if (s->perpass_verbose[4] > 1)
          stapsh_cmd.append(" -v");
        if (s->perpass_verbose[4] > 2)
          stapsh_cmd.append(" -v");
        // NB: We need to explicitly choose bash, as $SHELL may be weird...
        cmd.push_back("/bin/bash -c '" + stapsh_cmd + "'");

        // mask signals while we spawn, so we can manually send even tty
        // signals *through* ssh rather than to ssh itself
        {
          stap_sigmasker masked;
          child = stap_spawn_piped(s->verbose, cmd, &in, &out);
        }

        if (child <= 0)
          throw runtime_error(_("error launching stapsh"));

        try
          {
            set_child_fds(in, out);
          }
        catch (runtime_error&)
          {
            rc = finish();

            // ssh itself signals errors with 255
            if (rc == 255)
              throw runtime_error(_("error establishing ssh connection"));

            // If rc == 127, that's command-not-found, so we let ::create()
            // try again in legacy mode.  But only do this if there's a single
            // remote, as the old code didn't handle ttys well with multiple
            // remotes.  Otherwise, throw up again. *barf*
            if (rc != 127 || s->remote_uris.size() > 1)
              throw;
          }

        return rc;
      }

    int finish()
      {
        int rc = stapsh::finish();
        if (child <= 0)
          return rc;

        int rc2 = stap_waitpid(s->verbose, child);
        child = 0;
        return rc ?: rc2;
      }

  public:

    static remote* create(systemtap_session& s, const string& host);
    static remote* create(systemtap_session& s, const uri_decoder& ud);

    virtual ~ssh_remote() { finish(); }
};


// ssh connection without stapsh, for legacy stap installations
// NB: ssh commands use a tty (-t) so signals are passed along to the remote.
// It does this by putting the local tty in raw mode, so it only works for tty
// signals, and only for a single remote at a time.
class ssh_legacy_remote : public remote {
  private:
    vector<string> ssh_args, scp_args;
    string ssh_control;
    string host, port, tmpdir;
    pid_t child;

    ssh_legacy_remote(systemtap_session& s, const string& host, const string& port)
      : remote(s), host(host), port(port), child(0)
      {
        open_control_master();
        try
          {
            get_uname();
          }
        catch (runtime_error&)
          {
            close_control_master();
            throw;
          }
      }

    vector<string> ssh_command(initializer_list<string> ilist)
      {
        vector<string> cmd = ssh_args;
        cmd.insert(cmd.end(), ilist);
        return cmd;
      }

    int copy_to_remote(const string& local, const string& remote)
      {
        vector<string> cmd = scp_args;
        cmd.insert(cmd.end(), { local, host + ":" + remote });
        return stap_system(s->verbose, cmd);
      }

    void open_control_master()
      {
        static unsigned index = 0;

        if (s->tmpdir.empty()) // sanity check, shouldn't happen
          throw runtime_error(_("No tmpdir available for ssh control master"));

        ssh_control = s->tmpdir + "/ssh_remote_control_" + lex_cast(++index);

        scp_args = { "scp", "-q", "-o", "ControlPath=" + ssh_control };
        ssh_args = { "ssh", "-q", "-o", "ControlPath=" + ssh_control, host };

        if (!port.empty())
          {
            scp_args.insert(scp_args.end(), { "-P", port });
            ssh_args.insert(ssh_args.end(), { "-p", port });
          }

        // NB: ssh -f will stay in the foreground until authentication is
        // complete and the control socket is created, so we know it's ready to
        // go when stap_system returns.
        auto cmd = ssh_command({ "-f", "-N", "-M" });
        int rc = stap_system(s->verbose, cmd);
        if (rc != 0)
          throw runtime_error(_F("failed to create an ssh control master for %s : rc= %d",
                                 host.c_str(), rc));

        if (s->verbose>1)
          clog << _F("Created ssh control master at %s",
                     lex_cast_qstring(ssh_control).c_str()) << endl;
      }

    void close_control_master()
      {
        if (ssh_control.empty())
          return;

        auto cmd = ssh_command({ "-O", "exit" });
        int rc = stap_system(s->verbose, cmd, true, true);
        if (rc != 0)
          cerr << _F("failed to stop the ssh control master for %s : rc=%d",
                     host.c_str(), rc) << endl;

        ssh_control.clear();
        scp_args.clear();
        ssh_args.clear();
      }

    void get_uname()
      {
        ostringstream out;
        vector<string> uname;
        auto cmd = ssh_command({ "-t", "uname -rm" });
        int rc = stap_system_read(s->verbose, cmd, out);
        if (rc == 0)
          tokenize(out.str(), uname, " \t\r\n");
        if (uname.size() != 2)
          throw runtime_error(_F("failed to get uname from %s : rc= %d", host.c_str(), rc));
        const string& release = uname[0];
        const string& arch = uname[1];
        // XXX need to deal with command-line vs. implied arch/release
        this->s = s->clone(arch, release);
      }

    int start()
      {
        int rc;
        string localmodule = s->tmpdir + "/" + s->module_name + ".ko";
        string tmpmodule;

        // Make a remote tempdir.
        {
          ostringstream out;
          vector<string> vout;
          auto cmd = ssh_command({ "-t", "mktemp -d -t stapXXXXXX" });
          rc = stap_system_read(s->verbose, cmd, out);
          if (rc == 0)
            tokenize(out.str(), vout, "\r\n");
          if (vout.size() != 1)
            {
              cerr << _F("failed to make a tempdir on %s : rc=%d",
                         host.c_str(), rc) << endl;
              return -1;
            }
          tmpdir = vout[0];
          tmpmodule = tmpdir + "/" + s->module_name + ".ko";
        }

        // Transfer the module.
        if (rc == 0)
          {
            rc = copy_to_remote(localmodule, tmpmodule);
            if (rc != 0)
              cerr << _F("failed to copy the module to %s : rc=%d",
                         host.c_str(), rc) << endl;
          }

        // Transfer the module signature.
        if (rc == 0 && file_exists(localmodule + ".sgn"))
          {
            rc = copy_to_remote(localmodule + ".sgn", tmpmodule + ".sgn");
            if (rc != 0)
              cerr << _F("failed to copy the module signature to %s : rc=%d",
                         host.c_str(), rc) << endl;
          }

        // What about transfering uprobes.ko?  In this ssh "legacy" mode, we
        // don't the remote systemtap version, but -uPATH wasn't added until
        // 1.4.  Rather than risking a getopt error, we'll just assume that
        // this isn't supported.  The remote will just have to provide its own
        // uprobes.ko in SYSTEMTAP_RUNTIME or already loaded.

        // Run the module on the remote.
        if (rc == 0) {
          // We don't know the actual version, but all <=1.3 are approx equal.
          vector<string> staprun_cmd = make_run_command(*s, tmpdir, "1.3");
          staprun_cmd[0] = "staprun"; // NB: The remote decides its own path
          // NB: PR13354: we assume legacy installations don't have
          // staprun -r support, so we ignore staprun_r_arg.
          auto cmd = ssh_command({ "-t", cmdstr_join(staprun_cmd) });
          pid_t pid = stap_spawn(s->verbose, cmd);
          if (pid > 0)
            child = pid;
          else
            {
              cerr << _F("failed to run the module on %s : ret=%d",
                         host.c_str(), pid) << endl;
              rc = -1;
            }
        }

        return rc;
      }

    int finish()
      {
        int rc = 0;

        if (child > 0)
          {
            rc = stap_waitpid(s->verbose, child);
            child = 0;
          }

        if (!tmpdir.empty())
          {
            // Remove the tempdir.
            // XXX need to make sure this runs even with e.g. CTRL-C exits
            auto cmd = ssh_command({ "-t", "rm -r " + cmdstr_quoted(tmpdir) });
            int rc2 = stap_system(s->verbose, cmd);
            if (rc2 != 0)
              cerr << _F("failed to delete the tempdir on %s : rc=%d",
                         host.c_str(), rc2) << endl;
            if (rc == 0)
              rc = rc2;
            tmpdir.clear();
          }

        close_control_master();

        return rc;
      }

  public:
    friend class ssh_remote;

    virtual ~ssh_legacy_remote()
      {
        close_control_master();
      }
};


// Try to establish a stapsh connection to the remote, but fallback
// to the older mechanism if the command is not found.
remote*
ssh_remote::create(systemtap_session& s, const string& target)
{
  string port, host = target;
  size_t i = host.find(':');
  if (i != string::npos)
    {
      port = host.substr(i + 1);
      host.erase(i);
    }

  unique_ptr<ssh_remote> p (new ssh_remote(s));
  int rc = p->connect(host, port);
  if (rc == 0)
    return p.release();
  else if (rc == 127) // stapsh command not found
    return new ssh_legacy_remote(s, host, port); // try legacy instead
  return NULL;
}

remote*
ssh_remote::create(systemtap_session& s, const uri_decoder& ud)
{
  if (!ud.has_authority || ud.authority.empty())
    throw runtime_error(_("ssh target requires a hostname"));
  if (!ud.path.empty() && ud.path != "/")
    throw runtime_error(_("ssh target URI doesn't support a /path"));
  if (ud.has_query)
    throw runtime_error(_("ssh target URI doesn't support a ?query"));
  if (ud.has_fragment)
    throw runtime_error(_("ssh target URI doesn't support a #fragment"));

  return create(s, ud.authority);
}


remote*
remote::create(systemtap_session& s, const string& uri, int idx)
{
  remote *it = 0;
  try
    {
      if (uri.find(':') != string::npos)
        {
          const uri_decoder ud(uri);

          // An ssh "host:port" is ambiguous with a URI "scheme:path".
          // So if it looks like a number, just assume ssh.
          if (!ud.has_authority && !ud.has_query &&
              !ud.has_fragment && !ud.path.empty() &&
              ud.path.find_first_not_of("1234567890") == string::npos)
            it = ssh_remote::create(s, uri);
          else if (ud.scheme == "direct")
            it = new direct(s);
          else if (ud.scheme == "stapsh")
            it = new direct_stapsh(s);
          else if (ud.scheme == "unix")
            it = new unix_stapsh(s, ud);
          else if (ud.scheme == "libvirt")
            it = new libvirt_stapsh(s, ud);
          else if (ud.scheme == "ssh")
            it = ssh_remote::create(s, ud);
          else
            throw runtime_error(_F("unrecognized URI scheme '%s' in remote: %s",
                                   ud.scheme.c_str(), uri.c_str()));
        }
      else
        // XXX assuming everything else is ssh for now...
        it = ssh_remote::create(s, uri);
    }
  catch (std::runtime_error& e)
    {
      cerr << e.what() << " on remote '" << uri << "'" << endl;
      it = 0;
    }

  if (it && idx >= 0) // PR13354: remote metadata for staprun -r IDX:URI
    {
      stringstream r_arg;
      r_arg << idx << ":" << uri;
      it->staprun_r_arg = r_arg.str();
    }

  return it;
}

int
remote::run(const vector<remote*>& remotes)
{
  // NB: the first failure "wins"
  int ret = 0, rc = 0;

  for (unsigned i = 0; i < remotes.size() && !pending_interrupts; ++i)
    {
      remote *r = remotes[i];
      r->s->verbose = r->s->perpass_verbose[4];
      if (r->s->use_remote_prefix)
        r->prefix = lex_cast(i) + ": ";
      rc = r->prepare();
      if (rc)
        return rc;
    }

  for (unsigned i = 0; i < remotes.size() && !pending_interrupts; ++i)
    {
      rc = remotes[i]->start();
      if (!ret)
        ret = rc;
    }

  // mask signals while we're preparing to poll
  {
    stap_sigmasker masked;

    // polling loop for remotes that have fds to watch
    for (;;)
      {
        vector<pollfd> fds;
        for (unsigned i = 0; i < remotes.size(); ++i)
          remotes[i]->prepare_poll (fds);
        if (fds.empty())
          break;

        rc = ppoll (&fds[0], fds.size(), NULL, &masked.old);
        if (rc < 0 && errno != EINTR)
          break;

        for (unsigned i = 0; i < remotes.size(); ++i)
          remotes[i]->handle_poll (fds);
      }
  }

  for (unsigned i = 0; i < remotes.size(); ++i)
    {
      rc = remotes[i]->finish();
      if (!ret)
        ret = rc;
    }

  return ret;
}


/* vim: set sw=2 ts=8 cino=>4,n-2,{2,^-2,t0,(0,u0,w1,M1 : */
