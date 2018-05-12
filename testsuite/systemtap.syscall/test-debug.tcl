#!/usr/bin/env wish
package require Expect

set syscall_dir ""
set current_dir ""

proc syscall_cleanup {} {
    global syscall_dir current_dir
    if {$current_dir != ""} {
        cd $current_dir
	if {$syscall_dir != ""} {exec rm -rf $syscall_dir}
	set current_dir ""
    }
    exit 0
}

proc usage {progname} {
    puts "Usage: $progname testname"
    syscall_cleanup
}

proc bgerror {error} {
    puts "ERROR: $error"
    syscall_cleanup
}
trap {syscall_cleanup} SIGINT
set testname [lindex $argv 0]
if {$testname == ""} {
    usage $argv0
    exit
}

set filename [lindex $argv 1]
if {$filename == ""} {
    set filename "${testname}.c"
    set sys_prog "../sys.stp"
} else {
    set sys_prog "[file dirname [file normalize $filename]]/sys.stp"
}
set cmd "stap -c ../${testname} ${sys_prog}"

# extract the expected results
# Use the preprocessor so we can ifdef tests in and out

set ccmd "gcc -E -C -P $filename"
catch {eval exec $ccmd} output

set ind 0
foreach line [split $output "\n"] {
  if {[regsub {//staptest//} $line {} line]} {
    set line "$testname: [string trimleft $line]"

    # We need to quote all these metacharacters
    regsub -all {\(} $line {\\(} line
    regsub -all {\)} $line {\\)} line
    regsub -all {\|} $line {\|} line
    # + and * are metacharacters, but should always be used
    # as metacharacters in the expressions, don't escape them.
    #regsub -all {\+} $line {\\+} line
    #regsub -all {\*} $line {\\*} line

    # Turn '[[[[' and ']]]]' into '(' and ')' respectively.
    # Because normally parens get quoted, this allows us to
    # have non-quoted parens.
    regsub -all {\[\[\[\[} $line {(} line
    regsub -all {\]\]\]\]} $line {)} line

    regsub -all NNNN $line {[\-0-9]+} line
    regsub -all XXXX $line {[x0-9a-fA-F]+} line

    set results($ind) $line
    incr ind
  }
}

if {$ind == 0} {
    puts "UNSUPP"
    syscall_cleanup
    exit
}

if {[catch {exec mktemp -d staptestXXXXXX} syscall_dir]} {
    puts stderr "Failed to create temporary directory: $syscall_dir"
    syscall_cleanup
}

set current_dir [pwd]
cd $syscall_dir
catch {eval exec $cmd} output

set i 0
foreach line [split $output "\n"] {
    if {[regexp $results($i) $line]} {
	incr i
	if {$i >= $ind} {break}
    }
}
if {$i >= $ind} {
    puts "PASS"
} else {
    puts "FAIL"
}


text .t1 -width 60 -height 20 -wrap none \
    -xscrollcommand {.x1bar set} \
    -yscrollcommand {.ybar set}
text .t2 -width 60 -height 20 -wrap none \
    -xscrollcommand {.x2bar set} \
    -yscrollcommand {.ybar set}

scrollbar .x1bar -orient horizontal -command {.t1 xview}
scrollbar .x2bar -orient horizontal -command {.t2 xview}
scrollbar .ybar -orient vertical -command [list bindyview [list .t1 .t2]]

proc bindyview {lists args} {
    foreach l $lists {
	eval {$l yview} $args
    }
}

grid .t1 .t2 .ybar -sticky nsew
grid .x1bar .x2bar -sticky nsew
grid columnconfigure . 0 -weight 1
grid columnconfigure . 1 -weight 1
grid rowconfigure . 0 -weight 1
.t1 tag configure blue -foreground blue
.t2 tag configure blue -foreground blue
.t1 tag configure red -foreground red
.t2 tag configure red -foreground red

set i 0
foreach line [split $output "\n"] {
    if {[regexp "${testname}: " $line]} {
	if {[regexp $results($i) $line]} {
	    .t1 insert end ${line} blue
	    .t2 insert end $results($i) blue
	    incr i
	    if {$i >= $ind} {break}
	} else {
	    .t1 insert end ${line}
	}
	.t1 insert end "\n"
	.t2 insert end "\n"
    }
}

for {} {$i < $ind} {incr i} {
    .t2 insert end $results($i) red
    .t1 insert end "\n"
    .t2 insert end "\n"
}

bind . <Destroy> {syscall_cleanup}

