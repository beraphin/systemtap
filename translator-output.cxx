// translator output context
// Copyright (C) 2005-2013 Red Hat Inc.
// Copyright (C) 2005-2008 Intel Corporation.
// Copyright (C) 2010 Novell Corporation.
//
// This file is part of systemtap, and is free software.  You can
// redistribute it and/or modify it under the terms of the GNU General
// Public License (GPL); either version 2, or (at your option) any
// later version.

#include "translator-output.h"

#include <string>

using namespace std;

translator_output::translator_output (ostream& f):
  buf(0), o2 (0), o (f), tablevel (0), trailer_p(false)
{
}


translator_output::translator_output (const string& filename, size_t bufsize):
  buf (new char[bufsize]),
  o2 (new ofstream (filename.c_str ())),
  o (*o2),
  tablevel (0),
  filename (filename),
  trailer_p (false)
{
  o2->rdbuf()->pubsetbuf(buf, bufsize);
}

void
translator_output::close()
{
  if (o2)
    o2->close();
}


translator_output::~translator_output ()
{
  delete o2;
  delete [] buf;
}


ostream&
translator_output::newline (int indent)
{
  if (!  (indent > 0 || tablevel >= (unsigned)-indent)) o.flush ();
  assert (indent > 0 || tablevel >= (unsigned)-indent);

  tablevel += indent;
  o << "\n";
  for (unsigned i=0; i<tablevel; i++)
    o << "  ";
  return o;
}


void
translator_output::indent (int indent)
{
  if (!  (indent > 0 || tablevel >= (unsigned)-indent)) o.flush ();
  assert (indent > 0 || tablevel >= (unsigned)-indent);
  tablevel += indent;
}


ostream&
translator_output::line ()
{
  return o;
}

/* vim: set sw=2 ts=8 cino=>4,n-2,{2,^-2,t0,(0,u0,w1,M1 : */
