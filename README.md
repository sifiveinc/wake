[![][release-badge]][release] [![][ci-badge]][ci] [![][apache-2]](LICENSE)

# What is this?

Wake is a build orchestration tool and language.
If you have a build whose steps cannot be adequately expressed in
make/tup/bazel/etc, you probably need wake.
If you don't want to waste time rebuilding things that don't need it,
or that your colleagues already built, you might appreciate wake.

# Wake features:
  - dependent job execution

    Which jobs to run next can depend on the results of previous jobs.  For
    example, when you run configure in a traditional automake system, this
    typically affects what will be built by make.  Similarly cmake.  These
    two-stage build systems are necessary because make job selection cannot
    depend on the result of a prior build step.  In complicated builds,
    two-stages are sometimes not enough. In wake, all jobs may be dependent.

  - dependency analysis

    In classic build systems, you must specify all the inputs and outputs of
    a job you want to run.  If you under-specify the inputs, the build is
    not reproducible; it might fail to compile files that need recompilation
    and the job might fail non-deterministically on systems which run more
    jobs simultaneously.  If you over-specify the inputs, the build performs
    unnecessary recompilation when those inputs change.  In wake, if you
    under-specify the inputs, the build fails every time.  If you
    over-specify the inputs, wake automatically prunes the unused
    dependencies so the job will not be re-run unless it must.  You almost
    never need to tell wake what files a job builds; it knows.

  - build introspection

    When you have a built workspace, it is helpful to be able to trace the
    provenance of build artefacts.  Wake keeps a database to record what it
    did.  You can query that database at any time to find out exactly how a
    file in your workspace got there.

  - intrinsically-parallel language

    While your build orchestration files describe a sequence of compilation
    steps, the wake language automatically extracts parallelism.  Everything
    runs at once.  Only true data dependencies cause wake to sequence jobs.
    Wake handles parallelism for you, so you don't need to think about it.

  - shared build caching

    You just checked-out the master branch and started a build.  However,
    your system runs the same software as your colleague who authored that
    commit.  If wake can prove it's safe, it will just copy the prebuilt
    files and save you time.  This can also translate into pull requests
    whose regression tests pass immediately, increasing productivity.

# Installing dependencies

On Debian/Ubuntu (wheezy or later):

    sudo apt-get install makedev fuse libfuse-dev libsqlite3-dev libgmp-dev libncurses5-dev pkg-config git g++ gcc libre2-dev dash

On Redhat (6.6 or later):

    sudo yum install epel-release epel-release centos-release-scl
    # On RHEL6: sudo yum install devtoolset-6-gcc devtoolset-6-gcc-c++
    sudo yum install makedev fuse fuse-devel sqlite-devel gmp-devel ncurses-devel pkgconfig git gcc gcc-c++ re2-devel dash

On FreeBSD (12 or later):

    pkg install gmake pkgconf gmp re2 sqlite3 fusefs-libs dash
    echo 'fuse_load="YES"' >> /boot/loader.conf
    echo 'vfs.usermount=1' >> /etc/sysctl.conf
    pw groupmod operator -m YOUR-NON-ROOT-USER
    reboot

On Alpine Linux (3.14.0 or later):

    apk add g++ make pkgconf git gmp-dev re2-dev sqlite-dev fuse-dev ncurses-dev dash

  Alpine releases as old as 3.11.5 may work depending on the use case, but due
  to a limitation in older musl versions some jobs may be rebuilt unnecessarily.

On Mac OS with Mac Ports installed:

    sudo port install osxfuse sqlite3 gmp re2 ncurses pkgconfig dash

On Mac OS with Home Brew installed:

    brew install gmp re2 pkgconfig dash

Fuse is slightly more complicated, it requires permissions.

    brew tap homebrew/cask
    brew cask install osxfuse

You should see something like the following, and MacOS may ask for your password.

    You must reboot for the installation of osxfuse to take effect.

    System Extension Blocked
    "The system extension required for mounting FUSE volumes could not be loaded.
    Please open the Security & Privacy System Preferences pane, go to the General preferences and allow loading system software from developer "Benjamin Fleischer".

    Then try mounting the volume again."

Give FUSE permission to run as stated in the instructions and you should be good to go.

# Building wake

    git clone https://github.com/sifive/wake.git
    cd wake
    git tag                 # See what versions exist
    #git checkout master    # Use development branch (e.g. recent bug fix)
    #git checkout v0.24     # Check out a specific version, like v0.24
    make
    ./bin/wake install $HOME/stuff # or wherever


| Name                                                     | Version | License       |
| -------------------------------------------------------- | ------- | ------------- |
| **External dependencies**                                |         |               |
| [c++ 11](https://www.gnu.org/software/gcc/)              | >= 4.7  | GPLv3         |
| [dash](http://gondor.apana.org.au/~herbert/dash/)        | >= 0.5  | BSD 3-clause  |
| [sqlite3-dev](https://www.sqlite.org/)                   | >= 3.6  | public domain |
| [libgmp-dev](https://gmplib.org)                         | >= 4.3  | LGPL v3       |
| [libfuse-dev](https://github.com/libfuse/libfuse)        | >= 2.8  | LGPL v2.1     |
| [libre2-dev](https://github.com/google/re2)              | >= 2013 | BSD 3-clause  |
| [libncurses5-dev](https://www.gnu.org/software/ncurses/) | >= 5.7  | MIT           |
| [m4](https://www.gnu.org/software/m4/)                   | >= 1.4  | GPLv3         |
| **Optional dependencies**                                |         |               |
| [re2c](http://re2c.org)                                  | >= 1.0  | public domain |
| [utf8proc](https://juliastrings.github.io/utf8proc/)     | >= 2.0  | MIT           |
| **Internal dependencies**                                |         |               |
| [lemon](https://www.sqlite.org/lemon.html)               | 2021-09 | public domain |
| [gopt](http://www.purposeful.co.uk/software/gopt/)       | 10.0    | TFL           |
| [SipHash](https://github.com/veorq/SipHash)              | 2017-02 | CC0           |
| [BLAKE2](https://github.com/BLAKE2/libb2)                | 2018-07 | CC0           |
| [whereami](https://github.com/gpakosz/whereami)          | 2018-09 | WTFPLV2       |

# Configuring wake

Certain characteristics of wake execution can be configured for all invocations
of the wake tool. For example, the repo may set the minimum wake version or a
user may set the verbosity of a log message. This is achieved  via two config
files: .wakeroot and the user config. The user config overrides .wakeroot if
two values conflict. Certain values may only be specified in a specific
location. For example, min version may only be set in the .wakeroot. Both files
contain JSON5 source where the root object may contain the following keys.

| Key         | Description                                                                 | Required | Type                         | .wakeroot | user config | Default                                                                                     |
| ----------- | --------------------------------------------------------------------------- | -------- | ---------------------------- | --------- | ----------- | ------------------------------------------------------------------------------------------- |
| version     | SemVer compatible  with the repo                                            | No       | SemVer string                | Yes       | No          | ""                                                                                          |
| user_config | Path to user config for this repo. Allows a different config for each repo. | No       | shell expandable path string | Yes       | No          | `$XDG_CONFIG_HOME/wake.json` if `$XDG_CONFIG_HOME` set, `$HOME/.config/wake.json` otherwise |
| log_header | A string containing `$stream` and `$source` that will prepend every line of a jobs output | No | An interpolated string | Yes      | Yes | `'[$stream] $source: '`
| log_header_source_width | An integer that specifies the width of the `$source` variable in `log_header` | No | Positive Integer | Yes | Yes | 25
| log_header_align | A boolean that specifies whether or not to align log header output | No | Boolean | Yes | Yes | False
| max_cache_size | The number of bytes after which the shared cache will start a collection | No | Integer | Yes | Yes | 25GB
| low_cache_size | The number of bytes that the cache tries to reach during a collection | No | Integer | Yes | Yes | 15 GB
| cache_miss_on_failure | if `true` shared cache will report a cache miss instead of terminating when something goes wrong | No | Boolean | Yes | Yes | False

Below is a full example

```json5
// .wakeroot
{
  "version": "0.31.0",
  "user_config": "~/.config/wake.myrepo.json"
}

// ~/.config/wake.myrepo.json
{
  // Right now there are no implemented user keys so this file is always empty
  // This will be updated once log verbosity or another user key is implemented
}
```

While there are many sources that a config option might come from, the following priority is always observed from lowest
to highest:
1) .wakeroot
2) user config
3) environment variables
4) command line options

So a command line option overides anything, an environment variable overrides user config and wakeroot, user config
overrides wakeroot, and wakeroot overrides nothing

# Documentation

Documentation for wake can be found in [share/doc/wake](share/doc/wake).

 - Try the [Tutorial](share/doc/wake/tutorial.md) for a step-by-step
   introduction.
 - The [Quick Reference Guide](share/doc/wake/quickref.md) is handy overview
   of wake syntax in cheat-sheet form.
 - [Workspace Virtualization and Multiple Wake Invocations](share/doc/wake/workspace-virtualization-and-multi-wake.md)
   explains wake's content-addressable storage (CAS), how the workspace becomes
   a projection of wake's state, running concurrent wake invocations, and
   managing workspace disk usage.
 - The [Annotated Source Code](https://sifiveinc.github.io/wake/) of wake can
   be useful when trying to understand the standard library.
 - The sphinx-generated [reference manual](https://sifiveinc.github.io/wake/contents.html)

[release-badge]: https://img.shields.io/github/tag/sifive/wake.svg?label=release
[release]: https://github.com/sifive/wake/releases/latest
[ci-badge]: https://circleci.com/gh/sifive/wake/tree/master.svg?style=shield
[ci]: https://circleci.com/gh/sifive/wake/tree/master
[apache-2]: https://img.shields.io/badge/license-Apache%202-blue.svg
