#! /usr/bin/perl
# Generates index files from examples .meta file info.
# Copyright (C) 2008-2015 Red Hat Inc.
#
# This file is part of systemtap, and is free software.  You can
# redistribute it and/or modify it under the terms of the GNU General
# Public License (GPL); either version 2, or (at your option) any
# later version.

use strict;
use warnings;

use Cwd 'abs_path';
use File::Copy;
use File::Find;
use File::Path;
use Text::Wrap;
use HTML::Entities;
use DBI;

my $inputdir;
if ($#ARGV >= 0) {
    $inputdir = $ARGV[0];
} else {
    $inputdir = ".";
}
$inputdir = abs_path($inputdir);

# get current stap version
open(PIPE, " $inputdir/../../configure -V |") or die ("can't find stap version");
my $version_line = <PIPE>;
$version_line =~ /systemtap configure (.*)/ or die ("can't parse configure -V");
my $version = "For systemtap version ${1}.";
close(PIPE);

my $outputdir;
if ($#ARGV >= 1) {
    $outputdir = $ARGV[1];
} else {
    $outputdir = $inputdir;
}
$outputdir = abs_path($outputdir);

our $driver   = "SQLite";
our $database = "metadatabase.db";
our $dsn = "DBI:$driver:dbname=$database";
our $dbh = DBI->connect($dsn, "", "", { RaiseError => 1 })
                        or die $DBI::errstr;

my $sqlcmd = $dbh->prepare('DELETE FROM metavirt');
$sqlcmd->execute();

my %scripts = ();
print "Parsing .meta files in $inputdir...\n";
find(\&parse_meta_files, $inputdir);

my $meta;
my $keyword;
my %keywords;

$dbh->disconnect();

# Adds a formatted meta entry to a given file handle as text.
sub add_meta_txt(*;$) {
    my($file,$meta) = @_;

    print $file "$scripts{$meta}{name} - $scripts{$meta}{title}\n";

    # Don't output these, the description mentions all these in general.
    #print $file "output: $scripts{$meta}{output}, ";
    #print $file "exits: $scripts{$meta}{exit}, ";
    #print $file "status: $scripts{$meta}{status}\n";

    print $file "keywords: $scripts{$meta}{keywords}\n";

    $Text::Wrap::columns = 72;
    my $description = wrap('  ', '  ', $scripts{$meta}{description});
    print $file "\n$description\n";

    $Text::Wrap::separator = " \\\n";
    my $usage = wrap('', '  ', $scripts{$meta}{test_installcheck});
    if ($usage =~ /[a-z]/) {
        print $file "\n  # $usage\n";
    }
     $Text::Wrap::separator = "\n";

    print $file "\n\n";
}

# Adds a formatted meta entry to a given file handle as text.
sub add_meta_html(*;$) {
    my($file,$meta) = @_;

    my $name = $scripts{$meta}{name};
    print $file "<a href=\"$name\">$name</a> ";
    print $file "- $scripts{$meta}{title}<br>\n";

    # Don't output these, the description mentions all these in general.
    #print $file "output: $scripts{$meta}{output}, ";
    #print $file "exits: $scripts{$meta}{exit}, ";
    #print $file "status: $scripts{$meta}{status}<br>\n";

    print $file "keywords: ";
    foreach $keyword (split(/ /, $scripts{$meta}{keywords})) {
        print $file '<a href="keyword-index.html#' . (uc $keyword) . '">'
            . (uc $keyword) . "</a> ";
    }
    print $file "<br>\n";

    print $file "<p>".encode_entities($scripts{$meta}{description})."</p>";

    my $txt = $name;
    $txt =~ s/.stp$/.txt/;
    if (-e $txt) { # prefer .txt file link
        print $file "<p><i><a href=\"$txt\">sample usage in $txt</i></font>";
    } else {
        $Text::Wrap::separator = " \\\n";
        my $usage = wrap('', '', $scripts{$meta}{test_installcheck});
        $Text::Wrap::separator = "\n";
        if ($usage =~ /[a-z]/) {
            $usage = encode_entities($usage);
            print $file "<p><font size=\"-2\"><pre># $usage</pre></font>";
        }
    }

    print $file "</p>\n";
}

my $HEADER = "SYSTEMTAP EXAMPLES INDEX\n"
    . "(see also keyword-index.txt)\n\n";

my $header_tmpl = "$inputdir/html/html_header.tmpl";
open(TEMPLATE, "<$header_tmpl")
    || die "couldn't open $header_tmpl, $!";
my $HTMLHEADER = do { local $/;  <TEMPLATE> };
close(TEMPLATE);
my $footer_tmpl = "$inputdir/html/html_footer.tmpl";
open(TEMPLATE, "<$footer_tmpl")
    || die "couldn't open $footer_tmpl, $!";
my $HTMLFOOTER = do { local $/;  <TEMPLATE> };
close(TEMPLATE);

# Output full index and collect keywords
my $fullindex = "$outputdir/index.txt";
open (FULLINDEX, ">$fullindex")
    || die "couldn't open $fullindex: $!";
print "Creating $fullindex...\n";
print FULLINDEX $HEADER;
print FULLINDEX "\n$version\n\n";
    
my $fullhtml = "$outputdir/index.html";
open (FULLHTML, ">$fullhtml")
    || die "couldn't open $fullhtml: $!";
print "Creating $fullhtml...\n";
print FULLHTML $HTMLHEADER;
print FULLHTML "<p><em>$version</em></p>";

print FULLHTML "<h2>Best Examples</h2>\n";
print FULLHTML "<ul>\n"; 
foreach $meta (sort keys %scripts) {
    my $isbest = 0;
    foreach $keyword (split(/ /, $scripts{$meta}{keywords})) {
	if ($keyword eq "_best") { $isbest = 1; }
    }
    if ($isbest) {
        print FULLHTML "<li><a href=\"#".$scripts{$meta}{name}."\">";
        print FULLHTML $scripts{$meta}{name}." - ".$scripts{$meta}{title};
        print FULLHTML "</a></li>\n";
    }
}
print FULLHTML "</ul>\n\n";
print FULLHTML "<h2>All " . scalar (keys %scripts) . " Examples</h2>\n";
print FULLHTML "<ul>\n";

foreach $meta (sort keys %scripts) {

    add_meta_txt(\*FULLINDEX, $meta);
    print FULLHTML "<li>";
    print FULLHTML "<a name=\"".$scripts{$meta}{name}."\"></a>";
    print FULLHTML "<a href=\"#".$scripts{$meta}{name}."\">&para;</a> ";
    add_meta_html(\*FULLHTML, $meta);
    print FULLHTML "</li>";

    # Collect keywords
    foreach $keyword (split(/ /, $scripts{$meta}{keywords})) {
	if (defined $keywords{$keyword}) {
	    push(@{$keywords{$keyword}}, $meta);
	} else {
	    $keywords{$keyword} = [ $meta ];
	}
    }
}
print FULLHTML "</ul>\n";
print FULLHTML $HTMLFOOTER;
close (FULLINDEX);
close (FULLHTML);


my $KEYHEADER = "SYSTEMTAP EXAMPLES INDEX BY KEYWORD\n"
    . "(see also index.txt)\n\n";

# Output keyword index
my $keyindex = "$outputdir/keyword-index.txt";
open (KEYINDEX, ">$keyindex")
    || die "couldn't open $keyindex: $!";
print "Creating $keyindex...\n";
print KEYINDEX $KEYHEADER;
print KEYINDEX "\n$version\n\n";

my $keyhtml = "$outputdir/keyword-index.html";
open (KEYHTML, ">$keyhtml")
    || die "couldn't open $keyhtml: $!";
print "Creating $keyhtml...\n";
print KEYHTML $HTMLHEADER;
print KEYHTML "<p><em>$version</em></p>";

print KEYHTML "<h2>Examples by Keyword</h2>\n";

# On top link list
print KEYHTML "<p><tt>";
foreach $keyword (sort keys %keywords) {
    print KEYHTML '<a href="#' . (uc $keyword) . '">'
		  . (uc $keyword)
                  . "(". (scalar keys @{$keywords{$keyword}}). ")</a> ";
}
print KEYHTML "</tt></p>\n";

foreach $keyword (sort keys %keywords) {
    print KEYINDEX "= " . (uc $keyword) . " =\n\n";
    print KEYHTML "<h3>" . '<a name="' . (uc $keyword) . '">'
                  . "<a href=\"#". (uc $keyword) ."\">&para;</a> "
		  . (uc $keyword) . "</a></h3>\n";
    print KEYHTML "<ul>\n";

    foreach $meta (sort @{$keywords{$keyword}}) {
	add_meta_txt(\*KEYINDEX,$meta);
        print KEYHTML "<li>";
	add_meta_html(\*KEYHTML,$meta);
        print KEYHTML "</li>";
    }
    print KEYHTML "</ul>\n";
}
print KEYHTML $HTMLFOOTER;
close (KEYINDEX);
close (KEYHTML);

my @supportfiles
    = ("html/systemtapcorner.gif",
       "html/systemtap.css",
       "html/systemtaplogo.png",
       "README");
if ($inputdir ne $outputdir) {
    my $file;
    print "Copying support files...\n";
    if (! -d "$outputdir/html") {
	mkpath("$outputdir/html", 1, 0711);
    }
    foreach $file (@supportfiles) {
	my $orig = "$inputdir/$file";
	my $dest = "$outputdir/$file";
	print "Copying $file to $dest...\n";
	copy($orig, $dest) or die "$file cannot be copied to $dest, $!";
    }
}

sub parse_meta_files {
    my $file = $_;
    my $filename = $File::Find::name;

    my $sqlcmd = $dbh->prepare('INSERT INTO metavirt (title, name, keywords, description, path) VALUES (?,?,?,?,?)');

    if (-f $file && $file =~ /\.meta$/) {
	open FILE, $file or die "couldn't open '$file': $!\n";

	print "Parsing $filename...\n";
	
	my $title;
	my $name;
	my $keywords;
	my $status;
	my $exit;
	my $output;
	my $description;
	my $test_installcheck;
	while (<FILE>) {
	    if (/^title: (.*)/) { $title = $1; }
	    if (/^name: (.*)/) { $name = $1; }
	    if (/^keywords: (.*)/) { $keywords = $1; }
	    if (/^status: (.*)/) { $status = $1; }
	    if (/^exit: (.*)/) { $exit = $1; }
	    if (/^output: (.*)/) { $output = $1; }
	    if (/^description: (.*)/) { $description = $1; }
	    if (/^test_installcheck: (.*)/) { $test_installcheck = $1; }
	}
	close FILE;

	# Remove extra whitespace
	$keywords =~ s/^\s*//;
	$keywords =~ s/\s*$//;

	# The subdir without the inputdir prefix, nor any slashes.
	my $subdir = substr $File::Find::dir, (length $inputdir);
	$subdir =~ s/^\///;

        $sqlcmd->execute($title,$name,$keywords,$description,$subdir);

	if ($subdir ne "") {
	    $name = "$subdir/$name";
	}

	my $script = {
	    name => $name,
	    title => $title,
	    keywords => $keywords,
	    status => $status,
	    exit => $exit,
	    output => $output,
	    description => $description,
	    test_installcheck => $test_installcheck
	};

	# chop off the search dir prefix.
	$inputdir =~ s/\/$//;
	$meta = substr $filename, (length $inputdir) + 1;
	$scripts{$meta} = $script;

	# Put .stp script in output dir if necessary and create
	# subdirs if they don't exist yet.
	if ($inputdir ne $outputdir) {
	    # The subdir without the inputdir prefix, nor any slashes.
	    my $destdir = substr $File::Find::dir, (length $inputdir);
	    $destdir =~ s/^\///;
	    if ($subdir ne "") {
		if (! -d "$outputdir/$subdir") {
		    mkpath("$outputdir/$subdir", 1, 0711);
		}
	    }
	    my $orig = substr $name, (length $subdir);
	    $orig =~ s/^\///;
	    my $dest = "$outputdir/$name";
	    print "Copying $orig to $dest...\n";
	    copy($orig, $dest) or die "$orig cannot be copied to $dest, $!";
	}
    }
}
