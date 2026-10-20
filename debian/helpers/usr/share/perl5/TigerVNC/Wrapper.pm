package TigerVNC::Wrapper;

# Below is documentation for your module. You'd better edit it!
=pod

=head1 NAME

TigerVNC::Wrapper - TigerVNC server management

=head1 SYNOPSIS

  use TigerVNC::Config;
  use TigerVNC::Wrapper;

  my $options = { wrapperMode => 'tigervncserver' };

  # First, we ensure that we're operating in a sane environment.
  exit 1 unless &sanityCheck($options);

  # Next, parses the system /etc/tigervnc/vncserver-config-defaults and the user
  # ~/.config/tigervnc/config.pl configuration file as well as processes the command line.
  &getConfig($options);

  if ($options->{'kill'}) {
    my $err = &killVncServers($options);
    exit($err ? 1 : 0);
  } elsif ($options->{'list'}) {
    &listVncServers(\*STDOUT, $options);
    exit 0;
  } else {
    exit &startVncServer($options);
  }

=head1 DESCRIPTION

This module starts either a B<Xtigervnc> or B<X0tigervnc> server.

=cut

use strict;
use warnings;

use File::Spec;
use File::ReadBackwards;
use File::stat;
use File::Temp;
use File::Copy qw(cp);
use File::Basename qw(dirname basename);
use DirHandle;
use IO::File;
use Fcntl qw(SEEK_SET SEEK_CUR SEEK_END S_ISREG);
use Socket;
use Time::HiRes qw(usleep);
use Errno qw(:POSIX);
use POSIX qw(:sys_wait_h :fcntl_h setsid);

use TigerVNC::Common;
use TigerVNC::Config;

=pod

=head1 EXPORTS

=over 4

=item listVncServers

=item killVncServers

=item startVncServer

=back

=cut

use Exporter qw(import);

# Items to export into callers namespace by default. Note: do not export
# names by default without a very good reason. Use EXPORT_OK instead.
# Do not simply export all your public functions/methods/constants.
our @EXPORT = qw(
  listVncServers
  killVncServers
  startVncServer
);

our @EXPORT_OK = qw(
);

# This allows declaration
#
#   use UDNSC::ConfigParser ':all';
#
# If you do not need this, moving things directly into @EXPORT or @EXPORT_OK
# will save memory.
our %EXPORT_TAGS = (
# 'all' => \@EXPORT_OK,
);

our $VERSION = '1.12-1';

#
# Set global constants
#

#=pod
#
#=head1 GLOBALS
#
#=over 4
#
#=item $MAGIC
#
#Magic prefix inside the logfile to identify currently running server information.
#
#=back
#
#=cut

our $MAGIC = '3NI3X0 ';

=pod

=head1 FUNCTIONS

=cut

#
# Check if tcp port is available
#
sub checkTCPPortUsed {
  my ($port) = @_;
  my $proto  = 6; # TCP protocol, i.e., getprotobyname('tcp');

  socket(S, PF_INET, SOCK_STREAM, $proto) || die "$PROG: socket failed: $!";
  setsockopt(S, SOL_SOCKET, SO_REUSEADDR, pack("l", 1)) || die "$PROG: setsockopt failed: $!";
  if (!bind(S, sockaddr_in($port, INADDR_ANY))) {
    # print "$PROG: bind ($port) failed: $!\n";
    close(S);
    return 1;
  }
  close(S);
  return 0;
}

#
# checkRFBPortUsed checks if the given RFB port is used by vnc.
# A RFB port n is used if something is listening on the VNC server port
# (5900+n).
#

sub checkRFBPortUsed{
  my ($n) = @_;
  return &checkTCPPortUsed(5900 + $n);
}

#
# checkDisplayNumberAvailable checks if the given display number is available.
# A display number n is taken if something is listening on the X server port
# (6000+n) or at least one of the X11 lock files /tmp/.X$n-lock or
# /tmp/.X11-unix/X$n is present.
#

sub checkDisplayNumberAvailable {
  my ($n) = @_;

  return 0 if &checkTCPPortUsed(6000 + $n);

  my $displayLock = 0;
  my $unixDisplaySocketPath = "/tmp/.X11-unix/X$n";

  socket(S, PF_UNIX, SOCK_STREAM, 0) || die "$PROG: socket failed: $!";
  if (connect(S, pack_sockaddr_un $unixDisplaySocketPath)) {
    close(S);
    return 0;
  } elsif (-e $unixDisplaySocketPath) {
    print "\nWarning: $HOSTFQDN:$n is taken because of /tmp/.X11-unix/X$n\n";
    print "Remove this file if there is no X server $HOSTFQDN:$n\n";
    $displayLock = 1;
  }
  close(S);

  if (-e "/tmp/.X$n-lock") {
    print "\nWarning: $HOSTFQDN:$n is taken because of /tmp/.X$n-lock\n";
    print "Remove this file if there is no X server $HOSTFQDN:$n\n";
    $displayLock = 1;
  }

  return $displayLock ? 0 : 1;
}

#
# getDisplayNumber gets the lowest available display number. A display number
# n is taken if something is listening on the VNC server port (5900+n) or the
# X server port (6000+n).
#

sub getDisplayNumber($) {
  my ($rfbport) = @_;

  if (defined($rfbport) && $rfbport >= 5900 && $rfbport <= 5999) {
    my $n = $rfbport - 5900;
    return $n if &checkDisplayNumberAvailable($n);
  }
  foreach my $n (1..99) {
    if (&checkDisplayNumberAvailable($n)) {
      return $n if defined $rfbport;
      return $n if !&checkTCPPortUsed(5900 + $n);
    }
  }

  print STDERR "$PROG: no free display number on $HOSTFQDN.\n";
  exit -1;
}

#
# Check unix socket up
#
sub checkUnixSocketUp {
  my ($socket) = @_;

  socket(S, PF_UNIX, SOCK_STREAM, PF_UNSPEC) || die "$PROG: socket failed: $!";
  if (!connect(S, pack_sockaddr_un($socket))) {
    # print "$PROG: connect ($socket) failed: $!\n";
    close(S);
    return 0;
  }
  close(S);
  return 1;
}

sub x509CertFiles {
  my ($options) = @_;
  return (
    "$options->{'vncUserDir'}/${HOSTFQDN}-SrvCert.pem",
    "$options->{'vncUserDir'}/${HOSTFQDN}-SrvKey.pem");
}

sub rsaKeyFile {
  my ($options) = @_;
  return
    "$options->{'vncUserDir'}/${HOSTFQDN}-SrvRsaKey.pem";
}

sub getUniqueID($) {
  my ($options) = @_;
  if (defined $options->{'uniqueID'}) {
    return $options->{'uniqueID'};
  } else {
    my $uniqueID = $options->{'displayNumber'};
    defined($uniqueID) or die "Missing display number!";
    if ($options->{'wrapperMode'} eq 'x0tigervncserver') {
      my ($pid, $stale) = &readPidFile($options, $uniqueID);
      if (defined($pid) && !$stale) {
        if (defined($options->{'rfbport'}) && $options->{'rfbport'} > 0) {
          $uniqueID = $options->{'displayNumber'}.'-'.$options->{'rfbport'};
        } else {
          my $rfbunix = $options->{'rfbunixpath'};
          defined($rfbunix) or die "Missing RFB unix path!";
          $rfbunix = basename($rfbunix);
          $uniqueID = $options->{'displayNumber'}.'-'.$rfbunix;
          ($pid, $stale) = &readPidFile($options, $uniqueID);
          if (defined($pid) && !$stale) {
            my ($fh, $fn) = mkstemps(
              File::Spec->catfile($options->{'vncUserDir'},
                "${HOSTFQDN}:${uniqueID}-XXXX"), ".pid");
            close($fh);
            $fn = basename($fn);
            $fn =~ m/^\Q$HOSTFQDN\E:(\d+-.*)\.pid$/;
            $uniqueID = $1;
          }
        }
      }
    }
    return $options->{'uniqueID'} = $uniqueID;
  }
}

sub pidFileStd {
  my ($options, $uniqueID) = @_;
  $uniqueID = &getUniqueID($options) unless defined $uniqueID;
  return File::Spec->catfile($options->{'vncUserDir'},
    "${HOSTFQDN}:${uniqueID}.pid");
}

sub desktopLog {
  my ($logFile) = @_;
  $logFile =~ s{\.pid$}{};
  return $logFile . ".log";
}

sub readPidFile($$) {
  my ($options, $uniqueID) = @_;
  my $pidFileStd = &pidFileStd($options, $uniqueID);
  my $pidFile = readlink $pidFileStd;
  if (defined $pidFile) {
    $pidFile = File::Spec->rel2abs($pidFile, dirname($pidFileStd));
  } else {
    $pidFile = $pidFileStd;
  }
  my $pidFileFh = IO::File->new($pidFile, "r");
  unless (defined $pidFileFh) {
    unless ($! == ENOENT) {
      print STDERR "$PROG: Warning: Can't open pid file '$pidFile': $!\n";
    }
    return (undef, undef, undef);
  }
  unless (($pidFileFh->getline()//"") =~ m/^([0-9]+)$/) {
    print STDERR "$PROG: Warning: Can't parse pid file '$pidFile'!\n";
    return (undef, undef, undef);
  }
  my $pid   = int($1);
  my $stale = !kill(0, $pid);
  return ($pid, $stale, $pidFile);
}

sub CreatePidLogFiles($) {
  my ($options) = @_;

  my $pidFileStd = &pidFileStd($options);
  my $pidFile = $options->{'vncPidFile'} // $pidFileStd;
  $pidFile = File::Spec->rel2abs($pidFile, $options->{'vncUserDir'});
  my $logFile = &desktopLog($pidFile);

  my $pidFileFh  = IO::File->new($pidFile, "w", 0644);
  unless (defined $pidFileFh) {
    print STDERR "$PROG: Can't create pid file '$pidFile': $!\n";
    unlink($pidFile);
    return (undef, undef);
  }
  unlink($pidFile) if $options->{'dry-run'};

  if ($pidFileStd ne $pidFile) {
    my $pidFileRel = File::Spec->abs2rel($pidFile, dirname($pidFileStd));
    unless(symlink $pidFileRel, $pidFileStd) {
      print STDERR "$PROG: Can't symlink '$pidFileStd' to '$pidFileRel': $!\n";
      unlink($pidFile);
      unlink($pidFileStd);
      return (undef, undef);
    }
  }

  my $logFileFh = IO::File->new($logFile, "a+");
  unless (defined $logFileFh && seek($logFileFh, 0, SEEK_END)) {
    print STDERR "$PROG: Can't open log file '$logFile' for append: $!\n";
    unlink($pidFile);
    unlink($pidFileStd);
    return (undef, undef);
  }

  return ($pidFileFh, $logFileFh, $logFile)
}

sub cleanStale($$$) {
  my ($options, $runningVncServers, $nr) = @_;
  my $usedDisplay = $runningVncServers->{$nr}->{'usedDisplay'};
  my $stale       = $runningVncServers->{$nr}->{'stale'};
  my $server      = $runningVncServers->{$nr}->{'server'};
  my $pidFileStd  = &pidFileStd($options,$nr);

  # vnc pidfile stale
  my $msg = "";
  if (-e $pidFileStd) {
    my $pidFile = readlink $pidFileStd;
    if (defined $pidFile) {
      $pidFile = File::Spec->rel2abs($pidFile, dirname($pidFileStd));
      unless ($options->{'dry-run'} || unlink($pidFileStd) || $! == ENOENT) {
	print STDERR "$PROG: Warning: Can't clean stale pidfile '$pidFileStd': $!\n";
      }
    } else {
      $pidFile = $pidFileStd;
    }
    unless ($options->{'dry-run'} || unlink($pidFile) || $! == ENOENT) {
      print STDERR "$PROG: Warning: Can't clean stale pidfile '$pidFile': $!\n";
    } elsif ($stale) {
      print "Cleaning stale pidfile '$pidFile'!\n";
    }
  }
  if ($server eq 'Xtigervnc') {
    if (!$stale || !&checkTCPPortUsed(6000 + $usedDisplay)) {
      my @X11Locks = ("/tmp/.X$usedDisplay-lock", "/tmp/.X11-unix/X$usedDisplay");
      foreach my $entry (grep { -e $_ } @X11Locks) {
        unless ($options->{'dry-run'} || unlink($entry) || $! == ENOENT) {
          print STDERR "$PROG: Warning: Can't clean stale X11 lock '$entry': $!\n";
        } else {
          print "Cleaning stale X11 lock '$entry'!\n";
        }
      }
    }
  }
}

sub runningVncServers {
  my ($options) = @_;
  my %runningVncServers = ();

  my $d = DirHandle->new($options->{'vncUserDir'});
  if (defined $d) {
    while (defined(my $entry = $d->read)) {
      next unless $entry =~ m/^\Q$HOSTFQDN\E:((\d+)(?:-.*)?)\.pid$/;
      my ($uniqueID, $nr) = ($1, $2);
      my $pidFileStd = &pidFileStd($options, $uniqueID);
      my ($pid, $stale, $pidFile) = &readPidFile($options, $uniqueID);
      next unless defined $pid;
      my ($usedDisplay, $rfbport, $rfbunixpath) = (undef, -1, undef);
      if ($nr ne $uniqueID) {
        $usedDisplay = $nr;
      } elsif ($nr <= 99) {
        $usedDisplay = $nr;
        $rfbport     = $nr + 5900;
      } else {
        $rfbport     = $nr;
        $usedDisplay = $nr - 5900 if $nr >= 5900 && $nr <= 5999;
      }
      my $client  = undef;
      my $server  = "Xtigervnc";
      my ($name, $DISPLAY) = (undef, undef);
      if (defined $usedDisplay) {
        $name    = "$HOSTFQDN:$usedDisplay";
        $DISPLAY = -e "/tmp/.X11-unix/X${usedDisplay}"
          ? ":${usedDisplay}"
          : "$HOSTFQDN:${usedDisplay}";
      }
      my $logFile = &desktopLog($pidFile);
      {
        my $logFileFh = File::ReadBackwards->new($logFile);
        if (defined $logFileFh) {
          my $line;
          while (defined ($line = $logFileFh->readline)) {
            chomp $line;
            if ($line =~ m/Listening for VNC connections.* port\s+(\d+)/) {
              $rfbport = $1; last;
            } elsif ($line =~ m/^\Q$MAGIC\ENew (\w+) server '([^']*)' on (?:port (\d+)(?: and )?)?(?:unix socket (.*))? for display (.*)\.$/) {
             # 3NI3X0 New X0tigervnc server 'xerstin.jfalk.de:21 (joachim)' on port 5921 for display :20.
             $server = $1; $name = $2;
             $rfbport = $3 // -1; $rfbunixpath = $4;
             $DISPLAY = $5;
             $usedDisplay = $1 if $DISPLAY =~ m/:(\d+)(?:\.\d+)?$/;
             last;
            } elsif ($line =~ m/^\Q$MAGIC\EUse (.*) to connect to the VNC server\.$/) {
              # 3NI3X0 Use xtigervncviewer -SecurityTypes X509Plain -X509CA /home/joachim/.config/tigervnc/xerstin.jfalk.de-SrvCert.pem xerstin.jfalk.de:21 to connect to the VNC server.
              $client = $1;
            }
          }
        }
      }
      unless (defined $client) {
        # Example client connection
        $client = $rfbport > 0
          ? "xtigervncviewer $HOSTFQDN:$rfbport"
          : "xtigervncviewer $rfbunixpath";
      }
      if ($rfbport > 0) {
        $stale = 1 if $stale || !&checkTCPPortUsed($rfbport);
      }
      my $vncServerEntry = {
          'name'        => $name,
          'server'      => $server,
          'client'      => $client,
          'pid'         => $pid,
          'pidFileStd'  => $pidFileStd,
          'pidFile'     => $pidFile,
          'logFile'     => $logFile,
          'DISPLAY'     => $DISPLAY,
          'usedDisplay' => $usedDisplay,
          'rfbport'     => $rfbport,
          'rfbunixpath' => $rfbunixpath,
          'stale'       => $stale,
        };

      if ($options->{'cleanstale'} && $stale) {
        &cleanStale($options, { $uniqueID => $vncServerEntry }, $uniqueID);
        next;
      }
      # running VNC server if $options->{'cleanstale'}
      $runningVncServers{$uniqueID} = $vncServerEntry;
    }
    undef $d;
  }
  return \%runningVncServers;
}

sub matchVncServers($$$) {
  my ($options, $runningVncServers, $includeStale) = @_;

  my $dn      = $options->{'displayNumber'};
  my $rfbport = $options->{'rfbport'};
  my $rfbunix = $options->{'rfbunixpath'};
  $rfbport = -1 if defined($rfbport) && $rfbport <= 0;

  my @allVNCs = keys %{$runningVncServers};
  unless ($includeStale) {
    @allVNCs = grep { !$runningVncServers->{$_}->{'stale'} } @allVNCs;
  }
  my @vncs = @allVNCs;
  if (defined($dn) && $dn ne '*') {
    @vncs = grep {
      $runningVncServers->{$_}->{'usedDisplay'} == $dn } @vncs;
  }
  if (defined $rfbport) {
    @vncs = grep {
      $runningVncServers->{$_}->{'rfbport'} == $rfbport } @vncs;
  } elsif (@vncs == 0 && defined($dn) && $dn ne '*') {
    $rfbport = $dn <= 99 ? 5900 + $dn : $dn;
    @vncs = grep {
      $runningVncServers->{$_}->{'rfbport'} eq $rfbport } @allVNCs;
  }
  if (defined $rfbunix) {
    $rfbunix = File::Spec->rel2abs($rfbunix);
    @vncs = grep {
      defined($runningVncServers->{$_}->{'rfbunixpath'}) &&
      ($runningVncServers->{$_}->{'rfbunixpath'} eq $rfbunix) } @vncs;
  }
  return @vncs;
}

sub statusReport($) {
  my ($status) = @_;

  if (WIFEXITED($status)) {
    my $exitval = WEXITSTATUS($status);
    return $exitval == 0
      ? "cleanly exited"
      : "exited with status $exitval";
  } elsif (WIFSIGNALED($status)) {
    my $termsig = WTERMSIG($status);
    return "died with signal $termsig";
  } else {
    return "died ($status)";
  }
}

=pod

=over 4

=item listVncServers

List the specified VNC server.

=cut

sub listVncServers {
  my ($fh, $options, $vncs, $runningVncServers) = @_;

  unless (defined $runningVncServers) {
    $runningVncServers = &runningVncServers($options);
  }
  unless (defined $vncs) {
    $vncs = [&matchVncServers($options, $runningVncServers, 1)];
  }
  # Sort running VNC server list
  my $maxRFBUnixPathLen = 13; # This is length of "RFB UNIX PATH".
  $vncs = [sort {
      my $av = $runningVncServers->{$a}->{'usedDisplay'};
      my $bv = $runningVncServers->{$b}->{'usedDisplay'};
      return -1 if $av < $bv;
      return  1 if $av > $bv;
      $av = $runningVncServers->{$a}->{'rfbport'} // -1;
      $bv = $runningVncServers->{$b}->{'rfbport'} // -1;
      return -1 if $av < $bv;
      return  1 if $av > $bv;
      return  0;
    } @{$vncs}];
  foreach my $vnc (@{$vncs}) {
    my $rfbunix = $runningVncServers->{$vnc}->{'rfbunixpath'} // "";
    my $len = length($rfbunix);
    $maxRFBUnixPathLen = $len if $len > $maxRFBUnixPathLen;
  }
  printf $fh
    "\n".
    "TigerVNC server sessions:\n".
    "\n".
    "%-11s\t%-10s\t%-${maxRFBUnixPathLen}s\t%-12s\t%s\n",
    "X DISPLAY #", "RFB PORT #", "RFB UNIX PATH", "PROCESS ID #", "SERVER";
  foreach my $vnc (@{$vncs}) {
    my $stale   = $runningVncServers->{$vnc}->{'stale'}
      ? " (stale)" : "";
    my $rfbport = $runningVncServers->{$vnc}->{'rfbport'} // -1;
    $rfbport = "----" unless $rfbport > 0;
    my $rfbunix = $runningVncServers->{$vnc}->{'rfbunixpath'} // "";
    my $dn      = $runningVncServers->{$vnc}->{'usedDisplay'};
    my $pid     = $runningVncServers->{$vnc}->{'pid'};
    my $server  = $runningVncServers->{$vnc}->{'server'};
    printf $fh "%-10d\t%-10s\t%-${maxRFBUnixPathLen}s\t%-12s\t%s\n", $dn, $rfbport, $rfbunix, $pid.$stale, $server;
  }
}

=pod

=item killVncServers

Kill the specified VNC server.

=cut

#
# killVncServers
#

sub killVncServers {
  my ($options, $vncs, $runningVncServers) = @_;
  my $retval = 0;

  unless (defined $runningVncServers) {
    $runningVncServers = &runningVncServers($options);
  }
  unless (defined $vncs) {
    $vncs = [&matchVncServers($options, $runningVncServers)];
    if (@{$vncs} == 0) {
      if (!defined $options->{'rfbport'} &&
          !defined $options->{'rfbunixpath'} &&
          (!defined $options->{'displayNumber'} ||
           $options->{'displayNumber'} eq '*')) {
        print STDERR "$PROG: No VNC server running for this user!\n";
      } else {
        print STDERR "$PROG: No matching VNC server running for this user!\n";
      }
      $retval = 1;
    } elsif (@{$vncs} > 1 && ($options->{'displayNumber'}//'undef') ne '*') {
      print STDERR "$PROG: This is ambiguous. Multiple VNC servers are running for this user!\n";
      &listVncServers(\*STDERR, $options, $vncs, $runningVncServers);
      $retval = 1;
      $vncs = [];
    }
  }

  $SIG{'CHLD'} = 'IGNORE';
  foreach my $vnc (@{$vncs}) {
    my $stale       = \$runningVncServers->{$vnc}->{'stale'};
    my $pid         = $runningVncServers->{$vnc}->{'pid'};
    my $logFile     = $runningVncServers->{$vnc}->{'logFile'};
    my $server      = $runningVncServers->{$vnc}->{'server'};
    my $usedDisplay = $runningVncServers->{$vnc}->{'usedDisplay'};

    next unless defined $pid;
    print "Killing $server process ID $pid...";
    unless ($options->{'dry-run'}) {
      if (kill('TERM', $pid)) {
        my $i = 10;
        for (; $i >= 0; $i = $i-1) {
          last unless kill(0, $pid);
          usleep 100000;
        }
        if ($i >= 0) {
          print " success!\n";
        } else {
          $retval = 1;
          print " which seems to be deadlocked. Using SIGKILL!\n";
          unless (kill('KILL', $pid) || $! == ESRCH) {
            print STDERR "Can't kill '$pid': $!\n";
            next;
          }
        }
      } elsif ($! == ESRCH) {
        print " which was already dead\n";
        $$stale = 1;
      } else {
        $retval = 1;
        print STDERR "\nCan't kill '$pid': $!\n";
        next;
      }
    }
    &cleanStale($options, $runningVncServers, $vnc);

    # If option -clean is given, also remove the logfile
    if (!$options->{'dry-run'} && $options->{'clean'}) {
      unless (unlink($logFile) || $! == ENOENT) {
        $retval = 1;
        print STDERR "Can't remove '$logFile': $!\n";
      }
    }
  }
  $SIG{'CHLD'} = 'DEFAULT';
  return $retval;
}

# Make an X server cookie
sub CreateMITCookie {
  my ( $options ) = @_;
  my $displayNumber  = $options->{'displayNumber'};
  my $xauthorityFile = $options->{'xauthorityFile'};
  my $cookie = `mcookie`; # try mcookie

  unless (defined $cookie) {
    # mcookie failed => make an X server cookie the old fashioned way
    srand(time+$$+unpack("L",`cat $options->{'vncPasswdFile'}`));
    $cookie = "";
    for (1..16) {
      $cookie .= sprintf("%02x", int(rand(256)));
    }
  } else {
    chomp $cookie;
  }
  system(getCommand("xauth"), "-f", "$xauthorityFile", "add", "$HOSTFQDN:$displayNumber", ".", "$cookie");
  system(getCommand("xauth"), "-f", "$xauthorityFile", "add", "$HOST/unix:$displayNumber", ".", "$cookie");
}

# Make sure the user has a password.
sub CreateVNCPasswd {
  my ( $options ) = @_;

  my $passwordArgSpecified =
    ($options->{'src'}{'vncPasswdFile'}//"undef") eq "cmdline";

  # Check whether VNC authentication is enabled, and if so, prompt the user to
  # create a VNC password if they don't already have one.
  return if !$options->{'vncAuthEnabled'} || $passwordArgSpecified;
  my $vncPasswdFile = $options->{'vncPasswdFile'};
  my $st = stat($vncPasswdFile);

  if (!defined($st) || !S_ISREG($st->mode)) {
    print "\nYou will require a password to access your desktops.\n\n";
    unless (unlink($vncPasswdFile) || $! == ENOENT) {
      print STDERR "Can't remove old vnc passwd file '$vncPasswdFile': $!!\n";
      exit 1;
    }
    system(getCommand("tigervncpasswd"), $vncPasswdFile);
    exit 1 if (($? >> 8) != 0);
  } elsif (defined($st) && ($st->mode & 077)) {
    print STDERR "$vncPasswdFile must NOT be accessible by others. Set permission to 0600!\n";
    exit 1;
  }
}

# Make sure the user has a x509 certificate.
sub CreateX509Cert {
  my ( $options ) = @_;

  # Check whether X509 encryption is enabled, and if so, create
  # a self signed certificate if not already present or specified
  # on the command line.
  return if !$options->{'x509CertRequired'} ||
            defined $options->{'X509Cert'} ||
            defined $options->{'X509Key'};
  ($options->{'X509Cert'}, $options->{'X509Key'}) =
    &x509CertFiles($options);

  my $st = stat($options->{'X509Key'});
  if (!defined($st) || ($st->mode & 077) || !-f $options->{'X509Cert'}) {
    print "\nYou will require a certificate to use X509None, X509Vnc, or X509Plain.\n";
    print "I will generate a self signed certificate for you in $options->{'X509Cert'}.\n\n";
    unless (unlink($options->{'X509Cert'}) || $! == ENOENT) {
      print STDERR "Can't remove old X509Cert file '$options->{'X509Cert'}': $!!\n";
      exit 1;
    }
    unless (unlink($options->{'X509Key'}) || $! == ENOENT) {
      print STDERR "Can't remove old X509Key file '$options->{'X509Key'}': $!!\n";
      exit 1;
    }
    my $toSSLFh;
    my @CMD = split(/\s+/, $options->{'sslAutoGenCertCommand'});
    $CMD[0] = &getCommand($CMD[0]);
    push @CMD, "-config", "-" unless grep { $_ eq "-config" } @CMD;
    push @CMD, "-out", $options->{'X509Cert'} unless grep { $_ eq "-out" } @CMD;
    push @CMD, "-keyout", $options->{'X509Key'} unless grep { $_ eq "-keyout" } @CMD;
    unless (defined open($toSSLFh, "|-", @CMD)) {
      print STDERR "Can't start openssl pipe: $!!\n";
      exit 1;
    }
    my $configSSLFh;
    unless (defined open($configSSLFh, "<", "$SYSTEMCONFIGDIR/openssl.cnf")) {
      print STDERR "Can't open openssl configuration template $SYSTEMCONFIGDIR/openssl.cnf: $!\n";
      exit 1;
    }
    while (my $line = <$configSSLFh>) {
      $line =~ s/\@HostName\@/$HOSTFQDN/;
      $line =~ s/\@vncUserDir\@/$options->{'vncUserDir'}/;
      print $toSSLFh $line;
    }
    close $configSSLFh;
    close $toSSLFh;
    if ($? != 0) {
      unlink $options->{'X509Cert'};
      unlink $options->{'X509Key'};
      print STDERR "The openssl command ", join(' ', @CMD), " failed: $?\n";
      exit 1;
    }
  }
}

# Make sure the user has a RSA key if required.
sub CreateRSAKey {
  my ( $options ) = @_;

  # Check whether RSA-AES authentication is enabled, and if so, create
  # an RSA key if not already specified on the command line.
  return if !$options->{'rsaKeyRequired'} ||
            defined $options->{'RSAKey'};
  $options->{'RSAKey'} = &rsaKeyFile($options);

  my $st = stat($options->{'RSAKey'});
  if (!defined($st) || ($st->mode & 077)) {
    print "\nYou will require an RSA key to use RA2, RA2ne, RA2_256, or RA2ne_256.\n";
    print "I will generate one for you in $options->{'RSAKey'}.\n\n";
    unless (unlink($options->{'RSAKey'}) || $! == ENOENT) {
      print STDERR "Can't remove old RSAKey file '$options->{'RSAKey'}': $!!\n";
      exit 1;
    }
    my $toSSLFh;
    my @CMD = split(/\s+/, $options->{'sslAutoGenRSAKeyCommand'});
    @CMD = map { $_ eq 'RSAKeyFile' ? $options->{'RSAKey'} : $_ } @CMD;
    $CMD[0] = &getCommand($CMD[0]);
    $! = 0;
    if (system(@CMD)) {
      if ($!) {
        print STDERR "Can't start openssl command ", join(' ', @CMD), ": $!!\n";
      } else {
        print STDERR "The openssl command ", join(' ', @CMD), " failed: $?\n";
      }
      unlink $options->{'RSAKey'};
      exit 1;
    }
  }
}

sub CreateTmpVncDir {
  my ( $options ) = @_;
  my $tmpVncDir = File::Temp->newdir("tigervnc.XXXXXX",
    DIR => File::Spec->tmpdir(), CLEANUP => 1);

  # The following copies of the certificate and its key are required to
  # support NFS mounted /home directories protected by Kerberos security. In
  # this case, the Kerberos ticket might expire, making the certificate and
  # its key in the /home directory unaccessible to the TigerVNC server.
  # However, if there is a new incoming VNC connection, the VNC server needs
  # to read these files if X509 security is enabled. If the Kerberos ticket
  # is expired, these files will be unaccessible, and the VNC connection will
  # fail to connect. To prevent this, we copy these files to a temporary
  # directory where they will stay accessible.
  foreach my $fileOption (qw(X509Cert X509Key RSAKey)) {
    if (defined $options->{$fileOption}) {
      my $oldName = $options->{$fileOption};
      my $newName = File::Spec->catfile($tmpVncDir, basename($oldName));
      cp($oldName, $newName) or
        die "Can't copy $oldName to $tmpVncDir: $!";
      $options->{$fileOption} = $newName;
    }
  }
  return $tmpVncDir;
}

sub TouchTmpVncDir {
  my ( $options ) = @_;

  my $now = time();
  # Touch files in $tmpVncDir to keep same active in case of /tmp cleaner.
  foreach my $fileOption (qw(X509Cert X509Key RSAKey)) {
    my $tmpName = $options->{$fileOption};
    utime($now,$now,$tmpName) if defined $tmpName;
  }
}

=pod

=item startVncServer

Start an I<Xtigervnc> or I<X0tigervnc> server.

  &startVncServer($options);

=cut

# Now start the X VNC Server
sub startVncServer {
  my ($options) = @_;

  # Read in mandatory configuration information
  &readConfigFile($options, "mandatory");

  unless (defined $options->{'PlainUsers'}) {
    $options->{'PlainUsers'} = $USER;
  }
  unless (defined $options->{'PAMService'}) {
    if (-f '/etc/pam.d/vnc') {
      $options->{'PAMService'} = 'vnc';
    } else {
      # Default vnc service not present. Hence, we fall back to our own tigervnc service.
      $options->{'PAMService'} = 'tigervnc';
    }
  }

  unless (defined $options->{'vncPasswdFile'}) {
    $options->{'vncPasswdFile'} =
      File::Spec->catfile($options->{'vncUserDir'}, "passwd");
  }
  if (defined $options->{'session'} &&
      ref($options->{'session'}) eq '') {
    $options->{'session'} = [split(qr{\s+}, $options->{'session'})];
  } elsif (!defined $options->{'session'} ||
           ref($options->{'session'}) ne 'ARRAY') {
    $options->{'session'} = [];
  }
  unless (defined $options->{'SecurityTypes'}) {
    if (!defined($options->{'localhost'}) || $options->{'localhost'}) {
      $options->{'SecurityTypes'} = 'VncAuth';
      $options->{'localhost'}     = 1;
    } else {
      $options->{'SecurityTypes'} = 'VncAuth,TLSVnc';
      $options->{'localhost'}     = 0;
    }
  }
  $options->{'vncAuthEnabled'} = 0;
  $options->{'noneAuthEnabled'} = 0;
  $options->{'plainAuthEnabled'} = 0;
  $options->{'rsaKeyRequired'} = 0;
  $options->{'x509CertRequired'} = 0;
  $options->{'haveSSLEncryption'} = 0;
  foreach my $securityType (split(',', $options->{'SecurityTypes'})) {
    $options->{'vncAuthEnabled'} = 1    if $securityType =~ m/^(?:.*vnc|vncauth)$/i;
    $options->{'noneAuthEnabled'} = 1   if $securityType =~ m/none$/i;
    $options->{'plainAuthEnabled'} = 1  if $securityType =~ m/plain$/i;
    $options->{'rsaKeyRequired'} = 1    if $securityType =~ m/^ra2/i;
    $options->{'x509CertRequired'} = 1  if $securityType =~ m/^x509/i;
    $options->{'haveSSLEncryption'} = 1 if $securityType =~ m/^(?:x509|tls)/i;
  }
  if ($options->{'rsaKeyRequired'}) {
    if ($options->{'RequireUsername'}) {
      $options->{'plainAuthEnabled'} = 1;
    } else {
      $options->{'vncAuthEnabled'} = 1;
    }
  }

  if ($options->{'plainAuthEnabled'} &&
      $options->{'PAMService'} eq 'tigervnc' &&
      ! -f '/etc/pam.d/tigervnc') {
    print STDERR "$PROG: The tigervnc PAM servcice required for the security types\n";
    print STDERR "\tPlain, TLSPlain, or X509Plain is not installed.\n";
    &installPackageError("tigervnc-common");
  }

  unless (defined $options->{'localhost'}) {
    # If we have no encrypted VNC connection security types or
    # we have at least one *None security type in there, then
    # we better only server VNC on localhost to be tunneled via
    # ssh.
    if ($options->{'noneAuthEnabled'}) {
      $options->{'localhost'} = 1;
    } elsif ($options->{'haveSSLEncryption'}) {
      $options->{'localhost'} = 0;
    } elsif ($options->{'rsaKeyRequired'}) {
      # RSA-AES encryption is enabled
      $options->{'localhost'} = 0;
    } else {
      # no encrypted VNC connection security types
      $options->{'localhost'} = 1;
    }
  }
  # PREVENT THE USER FROM EXPOSING A VNC SESSION WITHOUT AUTHENTICATION
  # TO THE WHOLE INTERNET!!!
  if (!$options->{'localhost'} && $options->{'noneAuthEnabled'} &&
      !$options->{'I-KNOW-THIS-IS-INSECURE'}) {
    print STDERR "$PROG: YOU ARE TRYING TO EXPOSE A VNC SERVER WITHOUT ANY\n";
    print STDERR "AUTHENTICATION TO THE WHOLE INTERNET! I AM REFUSING TO COOPERATE!\n\n";
    print STDERR "If you really want to do that, add the --I-KNOW-THIS-IS-INSECURE option!\n";
    return -1;
  }
  if ($options->{'noneAuthEnabled'} &&
      !$options->{'I-KNOW-THIS-IS-INSECURE'}) {
    print STDERR "Please be aware that you are exposing your VNC server to all users on the\n";
    print STDERR "local machine. These users can access your server without authentication!\n";
  }

  unless ($options->{'vncAuthEnabled'}) {
    delete $options->{'vncPasswdFile'};
  }
  unless ($options->{'plainAuthEnabled'}) {
    delete $options->{'PAMService'};
    delete $options->{'PlainUsers'};
  }
  unless ($options->{'rsaKeyRequired'}) {
    delete $options->{'RSAKey'};
    delete $options->{'RequireUsername'};
  }
  unless ($options->{'x509CertRequired'}) {
    delete $options->{'X509Cert'};
    delete $options->{'X509Key'};
  }

  my $runningVncServers = &runningVncServers($options);
  my $haveOld = undef;
  if ($options->{'useold'}) {
    my @vncs = &matchVncServers($options, $runningVncServers);
    if (@vncs == 1) {
      $haveOld = $runningVncServers->{$vncs[0]};
      $options->{'displayNumber'} = $haveOld->{'usedDisplay'};
      $options->{'rfbport'}       = $haveOld->{'rfbport'};
    } elsif (@vncs > 1) {
      print STDERR "$PROG: This is ambiguous. Multiple vncservers are running for this user!\n";
      &listVncServers(\*STDERR, $options, \@vncs, $runningVncServers);
      return 1;
    }
  }
  unless (defined $options->{'displayNumber'}) {
    # Find display number.
    $options->{'displayNumber'} = &getDisplayNumber($options->{'rfbport'});
  }
  if (defined $options->{'rfbport'}) {
    if ($options->{'rfbport'} <= 0 && !defined $options->{'rfbunixpath'}) {
      print STDERR "$PROG: Either -rfbport or -rfbunixpath must be specified!\n";
      return 1;
    }
  } else {
    $options->{'rfbport'} = 5900 + $options->{'displayNumber'};
  }
  unless (defined $options->{'desktopName'}) {
    my $displayNumber = $options->{'displayNumber'};
    $options->{'desktopName'} = "${HOSTFQDN}:$displayNumber ($USER)";
  }
  if (defined $haveOld) {
    my $DISPLAY = $haveOld->{'DISPLAY'};
    print "\nReusing old VNC server '$options->{desktopName}' for display $DISPLAY.\n";
    print "Use $haveOld->{'client'} to connect to the VNC server.\n";
    return 0;
  }
  if ($options->{'wrapperMode'} eq 'tigervncserver') {
    my $dn = $options->{'displayNumber'};
    my @vncs = ();
    foreach my $vnc (keys %{$runningVncServers}) {
      next unless $runningVncServers->{$vnc}->{'usedDisplay'} eq $dn;
      next unless $runningVncServers->{$vnc}->{'server'} eq 'Xtigervnc';
      if ($runningVncServers->{$vnc}->{'stale'}) {
        &cleanStale($options, $runningVncServers, $vnc);
      } else {
        push @vncs, $vnc;
      }
    }
    if (@vncs > 0) {
      print STDERR "A Xtigervnc server is already running for display :$dn on machine $HOSTFQDN.\n";
      return 1;
    } elsif (!&checkDisplayNumberAvailable($dn)) {
      print STDERR "A X11 server is already running for display :$dn on machine $HOSTFQDN.\n";
      return 1;
    }
  }
  if (defined($options->{'rfbport'}) && $options->{'rfbport'} > 0) {
    if (&checkTCPPortUsed($options->{'rfbport'})) {
      my $rfbport = $options->{'rfbport'};
      my @vncs = grep {
          !$runningVncServers->{$_}->{'stale'} &&
          ($runningVncServers->{$_}->{'rfbport'} eq $rfbport)
        } keys %{$runningVncServers};
      if ($rfbport >= 5900 && $rfbport <= 5999) {
        $rfbport -= 5900;
        print STDERR "A VNC server is already running as :$rfbport on machine $HOSTFQDN.\n";
      } elsif (@vncs > 0) {
        print STDERR "A VNC server is already listening at port $rfbport on machine $HOSTFQDN.\n";
      } else {
        print STDERR "Something else is already listening at port $rfbport on machine $HOSTFQDN.\n";
      }
      return 1;
    }
  }
  if (defined($options->{'rfbunixpath'})) {
    $options->{'rfbunixpath'} = File::Spec->rel2abs($options->{'rfbunixpath'});
    if (&checkUnixSocketUp($options->{'rfbunixpath'})) {
      my $rfbunixpath = $options->{'rfbunixpath'};
      my @vncs = grep {
          !$runningVncServers->{$_}->{'stale'} &&
          defined($runningVncServers->{$_}->{'rfbunixpath'}) &&
          ($runningVncServers->{$_}->{'rfbunixpath'} eq $rfbunixpath)
        } keys %{$runningVncServers};
      if (@vncs > 0) {
        print STDERR "A VNC server is already listening at unix socket $rfbunixpath on machine $HOSTFQDN.\n";
      } else {
        print STDERR "Something else is already listening at unix socket $rfbunixpath on machine $HOSTFQDN.\n";
      }
      return 1;
    }
  }

  # Make sure the user has a password if required.
  &CreateVNCPasswd($options);
  # Make sure the user has a x509 certificate if required.
  &CreateX509Cert($options);
  # Make sure the user has a RSA key if required.
  &CreateRSAKey($options);
  &CreateMITCookie($options) if $options->{'wrapperMode'} eq 'tigervncserver';

  my $vncStartup = $options->{'vncStartup'};


  my ($pidFileFh, $desktopLogFh, $desktopLog) = &CreatePidLogFiles($options);
  # Return on error
  return 1 unless defined $pidFileFh;

  my $terminate = 0;
  $SIG{TERM} = sub { $terminate = 1; };
  $SIG{INT}  = sub { $terminate = 1; };
  $SIG{HUP}  = sub { $terminate = 1; };

  pipe STATUS_RH, STATUS_WH or die "Can't open pipe: $!";
  fcntl(STATUS_RH, F_SETFD, FD_CLOEXEC) or
    print STDERR "$PROG: Oops, setting close on exec failed: $!\n";
  fcntl(STATUS_WH, F_SETFD, FD_CLOEXEC) or
    print STDERR "$PROG: Oops, setting close on exec failed: $!\n";

  my $childPid = $options->{'fg'} ? 0 : fork();

  if ($childPid == 0) {
    # I am the child
    close STATUS_RH unless $options->{'fg'};

    my %childStatus;
    $SIG{CHLD} = sub {
        while ((my $child = waitpid(-1, WNOHANG)) > 0) {
          $childStatus{$child} = $?;
        }
      };

    # PID of the Xtigervnc or X0tigervnc server.
    my $xvncServerPid;
    # PID of the script starting the applications running in the VNC session.
    my $vncSessionPid;
    # X DISPLAY shared by the VNC server.
    my $DISPLAY;
    # Error flag for reporting to parent.
    my $error = 0;

    my $tmpVncDir = CreateTmpVncDir($options);

    # Starting up the Xtigervnc or X0tigervnc server.
    {
      my @cmd;
      if ($options->{'wrapperMode'} eq 'tigervncserver') {
        push @cmd, getCommand("Xtigervnc");
        push @cmd, ":".$options->{'displayNumber'};
      } else {
        push @cmd, getCommand("X0tigervnc");
      }
      foreach my $optionParseEntry (@{&getOptionParseTable($options)}) {
        my ($flags, $optname, $store) = @{$optionParseEntry};
        if ($options->{'wrapperMode'} eq 'x0tigervncserver') {
          next unless $flags & &OPT_X0TIGERVNC;
        } else {
          next unless $flags & &OPT_XTIGERVNC;
        }
        $optname =~ m/^([^:=|]*)/;
        my $name = $1;
        my $value = &{$store}($name);
        if ($optname =~ m/:/) {
          push @cmd, "-$name=$value" if defined $value;
        } elsif ($optname =~ m/=/) {
          push @cmd, "-$name", $value if defined $value;
        } else {
          die "Oops, can't parse $optname format!";
        }
      }
  #   push @cmd, '-pn';
      push @cmd, map { @{$_->{'args'}} } @{$options->{'vncServerExtraArgs'}};

      if ($options->{'verbose'} || $options->{'dry-run'}) {
        print "Starting ",join(" ",@cmd), "\n";
      }
      $xvncServerPid = fork();
      die "Failed to fork: $!" if $xvncServerPid < 0;

      if ($xvncServerPid == 0) {
        # I am the child
        close STATUS_RH;
        close STATUS_WH;

        # Detach ourselves from the terminal
        setsid() or die "Cannot detach from controlling terminal: $!";
        # Prevent possibility of acquiring a controlling terminal
        $SIG{'HUP'} = 'IGNORE';

        $desktopLogFh->close();
        $desktopLogFh = undef;
        open(OLDERR, '>&', \*STDERR); # save old STDERR
        open(STDOUT, '>>', $desktopLog);
        open(STDERR, '>>', $desktopLog);
        OLDERR->autoflush(1);
        STDERR->autoflush(1);
        STDOUT->autoflush(1);
        fcntl(OLDERR, F_SETFD, FD_CLOEXEC) or
          print STDERR "$PROG: Oops, setting close on exec failed: $!\n";
        chdir($tmpVncDir) or
          print STDERR "$PROG: Can't change directory to $tmpVncDir: $!\n";

        exit 0 if $options->{'dry-run'};
        exec {$cmd[0]} (@cmd) or
          print OLDERR "$PROG: Can't exec '".$cmd[0]."': $!\n";
        exit 1;
      }
      $pidFileFh->print($xvncServerPid."\n");
      $pidFileFh->close();

      $runningVncServers = {
          $options->{'uniqueID'} => {
              'name'        => "$HOSTFQDN:".$options->{'displayNumber'},
              'server'      => $options->{'wrapperMode'} eq 'tigervncserver'
                                 ? "Xtigervnc" : "X0tigervnc",
              'stale'       => 0,
              'pid'         => $xvncServerPid,
              'rfbport'     => $options->{'rfbport'},
              'rfbunixpath' => $options->{'rfbunixpath'},
              'usedDisplay' => $options->{'displayNumber'},
            }
        };
      # Wait for Xtigervnc/X0tigervnc to start up
      unless ($options->{'dry-run'}) {
        my $i = 300;
        if (defined($options->{'rfbport'}) && $options->{'rfbport'} > 0) {
          for (; $i >= 0; $i = $i-1) {
            if (&checkTCPPortUsed($options->{'rfbport'})) {
              last; # success
            }
            if (defined $childStatus{$xvncServerPid}) {
              $i = -2; last; # error
            }
            if ($terminate) {
              $i = -3; last; # error
            }
            usleep 100000;
          }
        }
        if (defined($options->{'rfbunixpath'})) {
          for (; $i >= 0; $i = $i-1) {
            if (&checkUnixSocketUp($options->{'rfbunixpath'})) {
              last; # success
            }
            if (defined $childStatus{$xvncServerPid}) {
              $i = -2; last; # error
            }
            if ($terminate) {
              $i = -3; last; # error
            }
            usleep 100000;
          }
        }
        if ($options->{'wrapperMode'} eq 'tigervncserver') {
          for (; $i >= 0; $i = $i-1) {
            if (-e "/tmp/.X11-unix/X$options->{'displayNumber'}" ||
                &checkTCPPortUsed(6000 + $options->{'displayNumber'})) {
              last; # success
            }
            if (defined $childStatus{$xvncServerPid}) {
              $i = -2; last; # error
            }
            if ($terminate) {
              $i = -3; last; # error
            }
            usleep 100000;
          }
        }
        if ($i < 0) {
          if (kill(0, $xvncServerPid)) {
            &killVncServers($options, [$options->{'uniqueID'}], $runningVncServers);
          } else {
            &cleanStale($options, $runningVncServers, $options->{'uniqueID'});
          }
          if ($i >= -2) {
            my $header = "=================== tail $desktopLog ===================";
            print STDERR "\n${header}\n";
            while (my $line = <$desktopLogFh>) {
              chomp $line;
              print STDERR $line, "\n";
            }
            print STDERR ("=" x length $header)."\n\n";
            $error = 1;
          }
          if ($i == -1) {
            print STDERR "$PROG: $cmd[0] did not start up, please look into '$desktopLog' to determine the reason! $i\n";
          } elsif ($i == -2) {
            my $status = &statusReport($childStatus{$xvncServerPid});
            print STDERR "$PROG: $cmd[0] $status, please look into '$desktopLog' to determine the reason! $i\n";
          }
          $xvncServerPid = undef;
          $runningVncServers = {};
        }
      }
      # Check if Xtigervnc/X0tigervnc has been started up successfully.
      if (defined $xvncServerPid) {
        # Xtigervnc/X0tigervnc is running. Thus, report some connection information.

        # If the unix domain socket exists then use that (DISPLAY=:n) otherwise use
        # TCP (DISPLAY=host:n)
        $DISPLAY = -e "/tmp/.X11-unix/X$options->{'displayNumber'}"
          ? ":$options->{'displayNumber'}"
          : "$HOSTFQDN:$options->{'displayNumber'}";
        my @status;
        {
          my $server = $options->{'wrapperMode'} eq 'tigervncserver'
            ? 'Xtigervnc' : 'X0tigervnc';
          my $desc = "";
          if (defined($options->{'rfbport'}) && $options->{'rfbport'} > 0) {
            $desc .= "port $options->{'rfbport'}";
          }
          if (defined $options->{'rfbunixpath'}) {
            $desc .= " and " if $desc ne "";
            $desc .= "unix socket $options->{'rfbunixpath'}";
          }
          push @status, "New $server server '$options->{desktopName}' on $desc for display $DISPLAY.";
        }
        {
          my @cmd = ("xtigervncviewer");
          push @cmd, "-SecurityTypes", $options->{'SecurityTypes'};
          push @cmd, "-X509CA", $options->{'X509Cert'} if $options->{'x509CertRequired'};
          push @cmd, "-passwd", $options->{'vncPasswdFile'} if $options->{'vncAuthEnabled'};
          if (defined($options->{'rfbport'}) && $options->{'rfbport'} > 0) {
            my $rfbport = $options->{'rfbport'};
            $rfbport -= 5900 if $rfbport >= 5900 && $rfbport <= 5999;
            push @cmd, $options->{'localhost'}
              ? ":$rfbport" : "$HOSTFQDN:$rfbport";
          } else {
            push @cmd, $options->{'rfbunixpath'};
          }
          push @status, "Use ".join(" ", @cmd)." to connect to the VNC server.";
        }
        print "\n";
        foreach my $status (@status) {
          $desktopLogFh->print($MAGIC.$status."\n");
          print $status."\n";
        }
        print "\n";
        $desktopLogFh->flush();
      }
    }
    if (defined($xvncServerPid) &&
        $options->{'wrapperMode'} eq 'tigervncserver') {
      if (defined $vncStartup) {
        # Run the X startup script.

        if ($options->{'verbose'}) {
          print "Starting session",
            (map { " ".&quotedString($_) } @{$options->{'session'}}),
            " via ", &quotedString($vncStartup), "\n";
          print "Log file is $desktopLog\n\n";
        }

        seek($desktopLogFh, 0, SEEK_END);

        my @cmd = ($vncStartup);
        push @cmd, @{$options->{'session'}};

        $vncSessionPid = fork();
        die "Failed to fork: $!" if $vncSessionPid < 0;

        if ($vncSessionPid == 0) {
          # I am the child
          close STATUS_RH;
          close STATUS_WH;

          # Detach ourselves from the terminal
          setsid() or die "Cannot detach from controlling terminal: $!";
          # Prevent possibility of acquiring a controlling terminal
          $SIG{'HUP'} = 'IGNORE';

          $desktopLogFh->close();
          $desktopLogFh = undef;
          open(OLDERR, '>&', \*STDERR); # save old STDERR
          open(STDOUT, '>>', $desktopLog);
          open(STDERR, '>>', $desktopLog);
          OLDERR->autoflush(1);
          STDERR->autoflush(1);
          STDOUT->autoflush(1);
          fcntl(OLDERR, F_SETFD, FD_CLOEXEC) or
            print STDERR "$PROG: Oops, setting close on exec failed: $!\n";

          $ENV{DISPLAY}    = $DISPLAY;
          $ENV{VNCDESKTOP} = $options->{'desktopName'};

          # Environment cleanup
          delete $ENV{XDG_MENU_PREFIX};
          if (defined $options->{'sessionName'}) {
            $ENV{GDMSESSION} = $options->{'sessionName'};
            $ENV{DESKTOP_SESSION} = $options->{'sessionName'};
            $ENV{XDG_SESSION_DESKTOP} = $options->{'sessionName'};
          } else {
            delete $ENV{GDMSESSION};
            delete $ENV{DESKTOP_SESSION};
            delete $ENV{XDG_SESSION_DESKTOP};
          }
          $ENV{XDG_SESSION_TYPE} = 'x11';
          if (defined $options->{'sessionDesktop'}) {
            $ENV{XDG_CURRENT_DESKTOP} = $options->{'sessionDesktop'};
          } else {
            delete $ENV{XDG_CURRENT_DESKTOP};
          }
          $ENV{XDG_SESSION_CLASS} = 'user';
          delete $ENV{XDG_DATA_DIRS};

          @cmd = qw(sleep 6) if $options->{'dry-run'};
          exec {$cmd[0]} (@cmd) or
            print OLDERR "$PROG: Can't exec '".$cmd[0]."': $!\n";
          exit 1;
        }
        # Wait for three seconds for erros to appear.
        {
          my $alarm = 0;
          $SIG{'ALRM'} = sub { $alarm = 1; };
          alarm 3;
          while (!$alarm && !$terminate &&
                 !defined $childStatus{$vncSessionPid}) {
            # Wait some more
            sleep 3600;
          }
          $SIG{'ALRM'} = 'DEFAULT';
        }
        if (!$terminate && defined $childStatus{$vncSessionPid}) {
          my $header = "=================== tail $desktopLog ===================";
          print STDERR "\n${header}\n";
          while (my $line = <$desktopLogFh>) {
            chomp $line;
            print STDERR $line, "\n";
          }
          print STDERR ("=" x length $header);
          print STDERR "\n\nSession startup via ",
            join(" ", map { &quotedString($_); } @cmd);
          my $status = &statusReport($childStatus{$vncSessionPid});
          if ($childStatus{$vncSessionPid} != 0) {
            print STDERR " $status!\n";
          } else {
            print STDERR " $status too early (< 3 seconds)!\n";
          }
          print STDERR "\nMaybe try something simple first, e.g.,\n";
          print STDERR "\ttigervncserver -xstartup /usr/bin/xterm\n";
          $error = 1;
        }
      } else { # !defined $vncStartup
        # Nothing to start. Check if autokill is enabled. Then, the Xtigervnc
        # server must be terminated.
        $terminate = 2 if $options->{'autokill'};
      }
    }
    if (defined $xvncServerPid) {
      chdir($tmpVncDir) or
        print STDERR "$PROG: Can't change directory to $tmpVncDir: $!\n";
      unless ($terminate || $error || $options->{'fg'}) {
        # Detach ourselves from the terminal
        setsid() or die "Cannot detach from controlling terminal: $!";
        # Prevent possibility of acquiring a controlling terminal
        $SIG{'HUP'} = 'IGNORE';

        $desktopLogFh->close();
        undef $desktopLogFh;
        open(STDOUT, '>>', $desktopLog);
        open(STDERR, '>>', $desktopLog);
        STDERR->autoflush(1);
        STDOUT->autoflush(1);
        syswrite STATUS_WH, ($error ? "ERR" : "OK!");
      }
      while (
        # Check for terminate flag
        !$terminate &&
        # Check for error flag
        !$error &&
        # Check that the VNC server is still running.
        !defined($childStatus{$xvncServerPid}) &&
        # Check that the applications are still running if they were started.
        (!defined($vncSessionPid) || !defined($childStatus{$vncSessionPid})))
      {
        TouchTmpVncDir($options);
        # Wait for SIGCHLD
        sleep 3600;
      }
      if (defined($vncSessionPid) && defined($childStatus{$vncSessionPid})) {
        $error = 1 if $childStatus{$vncSessionPid} != 0;
        my $status = &statusReport($childStatus{$vncSessionPid});
        print "The X session $status!\n";
      }
      if (!$options->{'autokill'}) {
        while (
          # Check for terminate flag
          !$terminate &&
          # Check for error flag
          !$error &&
          # Check that the VNC server is still running.
          !defined($childStatus{$xvncServerPid}))
        {
          TouchTmpVncDir($options);
          # Wait for SIGCHLD
          sleep 3600;
        }
      }
      if (defined $childStatus{$xvncServerPid}) {
        $error = 1 if $childStatus{$xvncServerPid} != 0;
        my $server = $runningVncServers->{$options->{'uniqueID'}}->{'server'};
        my $status = &statusReport($childStatus{$xvncServerPid});
        print "The $server server $status!\n";
        &cleanStale($options, $runningVncServers, $options->{'uniqueID'});
      } else {
        &killVncServers($options, [$options->{'uniqueID'}], $runningVncServers);
      }
    }
    chdir(File::Spec->rootdir());
    undef $tmpVncDir;
    syswrite STATUS_WH, ($error ? "ERR" : "OK!");
    exit 0 unless $options->{'fg'};
  }
  # I am the parent
  close STATUS_WH;
  my $status = "";
  do {
    $! = 0;
    sysread STATUS_RH, $status, 3;
  } while ($! == EINTR);
  $status = 'ERR' if $status eq "";
  return ($status eq 'OK!') ? 0 : -1;
}

1;
__END__

# -- documentation -----------------------------------------------------------

=pod

=back

=head1 AUTHOR

Joachim Falk E<lt>joachim.falk@gmx.deE<gt>

=head1 COPYRIGHT AND LICENSE

Copyright (C) 2004-2022 Joachim Falk <joachim.falk@gmx.de>

Copyright (C) 2017 Philipp Wolski <philipp.wolski@kisters.de>

Copyright (C) 2004 Ola Lundqvist <opal@debian.org>

Copyright (C) 2004 Marcus Brinkmann <Marcus.Brinkmann@ruhr-uni-bochum.de>

Copyright (C) 2004 Dirk Eddelbuettel <edd@debian.org>

Copyright (C) 2002-2003 RealVNC Ltd.

Copyright (C) 1999 AT&T Laboratories Cambridge.  All Rights Reserved.

Copyright (C) 1997, 1998 Olivetti & Oracle Research Laboratory

This is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

=cut
