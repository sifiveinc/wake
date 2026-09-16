# Wake Tutorial

This tutorial assumes you have wake and git installed in your path.
Code sections are intended to either be copy-pasted into a terminal, or to be
added to a `tutorial.wake` file. If you're reading the raw file, these are
indicated above the code block; if the Markdown has already been rendered, it
shouldn't be too difficult to guess.

## Table of Contents

  - [Invoking wake](#invoking-wake)
  - [Functions](#functions)
    - [Anonymous functions](#anonymous-functions)
  - [Data types and pattern matching](#data-types-and-pattern-matching)
    - [More complex types](#more-complex-types)
    - [Named accessors](#named-accessors)
  - [Dealing with failure](#dealing-with-failure)
    - [Option](#option)
    - [Result](#result)
  - [Executing shell jobs](#executing-shell-jobs)
    - [Customizing job invocation](#customizing-job-invocation)
  - [Build rules with file visibility](#build-rules-with-file-visibility)
  - [Building targets from multiple files](#building-targets-from-multiple-files)
    - [Map and partial function evaluation](#map-and-partial-function-evaluation)
  - [Supplemental file visibility](#supplemental-file-visibility)
  - [Publish/Subscribe](#publishsubscribe)
  - [Downloading and parsing files](#downloading-and-parsing-files)
  - [Ignore wake source files](#ignore-wake-source-files)

## Invoking wake

Unlike some other build systems, wake stores data to the filesystem between runs
in order to cache and optimize future builds; it therefore needs a (small) bit
of setup before working.

```console
$ mkdir ~/tutorial
$ cd ~/tutorial
$ wake --init .
$ wake -x '5 + 6'
11
```

This sequence of commands creates a new workspace managed by wake.
The `--init` option is used to create an initial `wake.db` to record the state
of the build in this workspace.
Whenever you run wake, it searches for a `wake.db` in parent directories.
The first `wake.db` found defines what wake considers to be the workspace.
You can thus safely run wake in any sub-directory of `tutorial` and wake
will be aware of all the relevant dependencies and rules.

The output of wake run on `-x 'expression'` is the result of evaluating that expression.
In this case, `5 + 6` results in value `11`.
Wake will report more information when run in verbose mode: `wake -v`.

```console
$ wake -vx '5 + 6'
5 + 6: Integer = 11
```

The verbose output is of the form `expression: type = value`. The above
will give `5 + 6: Integer = 11`. As before, `5 + 6` results in `11`,
which is an `Integer`.

Next, create a file with a `.wake` extension (the rest of the tutorial assumes
you call it `tutorial.wake`) containing two lines:

```wake
export def hello =
    "Hello World"
```

The syntax `def x = y` introduces a new variable `x` whose value is equal to
`y`; in this case, `y` is just a `String`.  The `export` keyword is involved
with wake's package visibility system, and doesn't do much here (beyond
preempting what would be a warning), but can come into play with more complex
project setups.

With that file in the current directory, you can access anything named in it.

```console
$ wake -x 'hello'
"Hello World"
```

Wake processes all wake files (`*.wake`) which are in the workspace,
so our new file `tutorial.wake` is available to us from the command-line; since
it's the *only* wake file here, some additional handling happens to make it even
simpler to access from, but the details about that are better described later.
For now, just know that any wake code should be added to this *same* file.

## Functions

While wake is first and foremost a build system, it's been written to gracefully
handle very complex projects.  In service of that goal, wake provides a full
programming language for use in writing equally complex build rules.  As may be
expected, functions play an integral role in that language.

```wake
export def increment i =
    i + 1
```

```console
$ wake -x 'increment 3'
4
```

Wake uses a syntax more closely related to ML and other functional languages.
Most notably, functions in wake are applied to their arguments with simple
spaces rather than parentheses, so `f x y` is read as "function `f` run
on `x` and `y`"; in C that would be `f(x, y)`.  Looking at the example,
`def increment i = ...` introduces a function `increment` which takes a single
argument `i`, while `increment 3` calls that function with `i` equal to `3`.

```console
$ wake -vx increment
increment: (i: Integer) => Integer = <tutorial.wake:5:[5-9]>
```

Notice that we didn't have to specify anything for wake to know what types
`increment` accepts and returns.  Wake is strongly-typed and will definitely
complain if you try to pass a function the wrong type of object, but it is
pretty good about guessing what things are (technically, using a Hindley-Milner
type system).  It's often a good practice to explicitly list types anyway -- to
both help when you're reading the code and to catch any mistakes that might slip
in -- but for brevity most examples in this tutorial will skip the annotations.

```wake
export def decrement (i: Integer): Integer =
    i - 1
```

Hopefully unsurprisingly, wake can handle more complex functions than simple
addition and subtraction.

```wake
export def countHex (until: Integer): List String =
    def numbers =
        seq until
    map strHex numbers
```

```console
$ wake -x 'countHex 20'
"0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "a", "b", "c", "d", "e", "f", "10", "11", "12", "13", Nil
```

Note that `Nil` represents an empty list in wake, and any values are just built
on top of it; `x, y, Nil` is a `List` comprised of the two objects `x` and `y`.

In wake, the last line is always the value of the define.  As `countHex` is
a function, you could say the last line is the return value.  Only one
standalone expression -- that last one -- is allowed in any block; wake does not
allow the imperative (C, Java, etc.) style of listing multiple statements or
expressions in a single function, to be executed one after another.  Even so, by
gathering the intermediate calculations into a series of `def` blocks, wake can
be just as powerful.

Beyond that, `seq` is a function which takes an `Integer` and generates a
`List Integer` containing every number from zero to one below the argument (in
other words, a `List` starting at zero whose length is determined by the
argument).  Wake uses linked lists rather than arrays for most things, which
means that adding items to or taking items from the *start* of a `List` is very
fast and we don't have to worry about running out of indices and having to copy
memory, but also means that doing anything to the *end* of a `List` can be
expensive.

Finally, `map` takes another function and applies it to every element in a list;
in this case, `strHex` will render each number into a (hexadecimal) `String`.
Functions are just another type of object in wake and can be passed around very
easily, so many tasks which might typically be written as a loop are instead
written using functions like `map`.

### Anonymous functions

Generally, one should define functions with a name.
This makes code more readable for other people.
However, sometimes the function is really just an after-thought.
For these cases wake makes it possible to define functions inline.

```console
$ wake -vx '\x x^2'
\x x^2: (x: Integer) => Integer = <<command-line>:1:[12-14]>
$ wake -x 'map (\x x^2) (seq 10)'
0, 1, 4, 9, 16, 25, 36, 49, 64, 81, Nil
```

The backslash syntax is an easy-to-type stand-in for the lambda symbol, λ.
While we've not talked about it, the wake language implements a "typed lambda
calculus". This syntax for inline functions is wake's homage to its roots.

Multiple arguments can be used by simply prefixing all of them with `\`, and you
can pattern-match on any of them; the following two functions are equivalent:

```wake
export def lambda x (Pair y z) =
    x + y + z
export def expression =
    lambda 1 (Pair 2 3)
```

```console
$ wake -x '(\x \(Pair y z) x + y + z) 1 (Pair 2 3)'
6
```

To make inline functions even easier to define, wake also supports a syntax
where one specifies the holes `_` in an expression and a function is created
which fills the holes from left to right.

```console
$ wake -vx '(_ + 4)'
(_ + 4): Integer => Integer = <<command-line>:1:[10-14]>
$ wake -x 'map (_ + 4) (seq 8)'
4, 5, 6, 7, 8, 9, 10, 11, Nil
$ wake -x 'seq 1000 | filter (_ % 55 == 0) | map str | catWith " "'
"0 55 110 165 220 275 330 385 440 495 550 605 660 715 770 825 880 935 990"
```

This hole-based syntax is not as powerful as lambda expressions, because
each argument can only be used once.  Furthermore, the functions are created
at block boundaries, which include `()`s, which can limit their usefulness.
Nevertheless, this syntax can be convenient.

The last example also demonstrates the syntax `x | f 0 | g`. This should be read
like the pipe operator in a shell script, feeding the value on the left into the
final argument of the function on the right.  Ultimately, it will be evaluated
as `g (f 0 x)`, but when chaining together multiple transformations to data, the
pipe syntax becomes more readable.

## Data types and pattern matching

So far, we've gotten a lot done with primitive types (`Integer`, `String`,
...) and `Lists`. However, wake does allow you to define your own data types.
These can then be analyzed using pattern matching.

```wake
export data Animal =
    Cat (name: String)
    Dog (age: Integer)
```

The `data` keyword introduces a new type, `Animal`.
Types are always capitalized, and use a different namespace from variables.
As defined, `Animal` can either be a `Cat` or a `Dog`,
where `Cat`s have names and `Dog`s have ages.
If we want the new type available to other files, we put an `export` in
front of the `data`, just like with values and functions.

```console
$ wake -vx 'Cat'
Cat: (name: String) => Animal = <tutorial.wake:16:[5-7]>
```

As wake informs us, `Cat` is a function that takes a `String` and returns an
`Animal`. However, unlike normal functions, `Cat` is also a type constructor.
Type constructors differ from normal variables in that they are capitalized
and can be used in pattern matches.

```wake
export def strAnimal a = match a
    Cat x = "a cat called {x}"
    Dog y = "a {str y}-year-old dog"
```

```console
$ wake -x 'strAnimal (Dog 12)'
"a 12-year-old dog"
$ wake -x 'strAnimal (Cat "Fluffy")'
"a cat called Fluffy"
```

The function `strAnimal` is an example of pattern matching. It consists of the
keyword `match` followed by the value to pattern match. On the following lines
we indent and provide one or more patterns.

In many ways, this is similar to a `switch` statement in other languages, but
can be used with any type by giving a constructor along with new value names
corresponding to each of its arguments.  In `match`, these values are followed
by `=` and then an expression that is the result of when the pattern is matched.
For example, in `Cat x = "a cat called {x}"`, the `x` is a new variable binding
corresponding to `name` in the declaration, thus it is a `String`. After `=` we
have a `String` that is the result whenever `strAnimal` is passed a `Cat`.

Note that when matching, the patterns must exhaustively catch all possibilities.
For example, it would be illegal to `match` but only provide a pattern for
`Cat`.  A useful pattern is to provide a "default" pattern with `_` to match
anything:

```wake
export def matchWithDefault a = match a
    Cat x = "a cat called {x}"
    _     = "not a cat!"
```

When, as above, we only care about a single constructor of a type, we can use
the `require` keyword instead. If the object on the right-hand side of the `=`
is of a different constructor, the value is passed up a level and the evaluation
is returned from early (similar to a `return` statement in C-derived languages).

```wake
export def requireCat pet =
    require Cat x = pet
    else "not a cat!"

    "a cat called {x}"
```

Note that this destructor pattern (`Dog y`, `Cat x`) not only works for
`match` and `require`, but also `def` when the type only has a single
constructor/destructor.

In cases where the value returned from the surrounding block happens to be the
same type as the value in the `require`, we can leave out the `else` clause and
anything with a different constructor (in the case below, every `Cat`) will be
passed up unchanged.

```wake
export def addDogYears years pet =
    require Dog age = pet
    Dog (age + years * 7)
```

```console
$ wake -x 'addDogYears 3 (Dog 12)'
Dog 33
$ wake -x 'addDogYears 3 (Cat "Fluffy")'
Cat "Fluffy"
```

### More complex types

The previous example's `Animal` type provides a relatively simple union tagging
one of two potential values.  Types can also collect multiple values together,
or both.

```wake
export data Customer =
    Customer (name: String) (id: Identifier)
export data Identifier =
    Authenticated (login: String) (pass: String)
    Basic (email: String)
```

Note also that we can use `Identifier` before it has been defined.  For the most
part, wake allows a definition (type or variable) to happen anywhere at or above
the level of its usage, though there are still a couple restrictions.

When destructing a multi-parameter type, the number of new variable names needs
to match the number of parameters to the constructor, but `_` or something
starting with it can be used for anything which you don't care about using.

```wake
export def greetCustomer customer =
    def Customer name _id = customer
    "Hello, {name}!"
```

When a type is used for structural purposes rather than data itself, it's often
better to not specify what type(s) some or all of its values have.  However,
this abstraction isn't as simple as just leaving off the type annotation in the
constructor; wake still needs to be explicitly told everything about the type.
Instead, we give a placeholder "type variable" after the type name:

```wake
export data UnsortedTree value =
    Node (left: UnsortedTree value) (right: UnsortedTree value)
    Leaf (this: value)
```

By not referencing any one type, `UnsortedTree` is able to collect literally any
values, so long as they all have the same type -- note that we need to specify
what inner type is used for both `left` and `right` in their annotations; we
can't simply say that something is an `UnsortedTree`, we have to say it's an
`UnsortedTree String` or an `UnsortedTree Animal` (or even an
`UnsortedTree someOtherTypeVariable` if we want to include it within another
abstract type).  Additionally, the capitalization here is important.  When
type-checking, wake assumes that everything starting with an uppercase letter is
an actual type and everything starting with a lowercase letter is a type
variable, so both `data UnsortedTree Value` and `data unsortedTree value` would
be invalid.

```console
$ wake -vx filter
filter: (f: a => Boolean) => List a => List a = </usr/local/share/wake/lib/core/list.wake:577:[12-35]>
```

We used abstract types previously when looking at `Lists`.  The same rules for
type variables apply to functions, which allows us to work with structures we
might not know everything about; wake is able to implement `filter` for every
type of `List` because every `a` in the function's type annotation must refer to
the same type.  No matter how complex or simple an `a` might be, we know that if
we pass one to `f` we will get either a `True` or a `False`, and we know how to
deal with those.

### Named accessors

As types grow larger, it can become difficult to keep track of what value is in
which position; it is even more difficult to edit or replace one value among
many.  To aid with this, wake provides the `tuple` keyword as a counterpart to
`data`.

```wake
export tuple Module =
    Name: String
    Imports: List Module
    Sources: List Path
```

```console
$ wake -vx Module
Module: (Name: String) => (Imports: List Module) => (Sources: List Path) => Module = <tutorial.wake:45:[14-19]>
```

In this case, `Name`, `Imports`, and `Sources` are the *fields* of the type
constructed by `Module` rather than three separate *constructors* of it.
Notably, three functions each are created behind the scenes:

```console
$ wake -vx getModuleName
getModuleName: Module => String = <tutorial.wake:46:[5-8]>
$ wake -vx setModuleName
setModuleName: (Name: String) => Module => Module = <tutorial.wake:46:[5-8]>
$ wake -vx editModuleName
editModuleName: (fnName: String => String) => Module => Module = <tutorial.wake:46:[5-8]>
```

With these, we can easily work with types collecting dozens of values, at least
as far as the raw code goes.  It is important to note that all objects in wake
are immutable -- the so-called `editModuleName` does not change the existing
`Module`, but instead creates a new object with all values but `Name` copied
from the original.  There is no way to pass-by-reference or by pointer which
modifies objects in-place.

## Dealing with failure

Given wake's easily parametric -- and not inheritable -- types, it's generally
considered bad form to have failures be represented by "magic" values which
could be confused with a success (e.g. null pointers or empty strings) or
brushing them off to a side channel (e.g. exception throw/catch).  Instead,
they're explicitly marked in the type system through `data` types.

### Option

The simplest and most common way we deal with failure in wake is with the
`Option` type:

```wake
export def firstOrZeroWhenEmpty list = match (head list)
    Some first = first
    None       = 0
```

As in the above, we can use pattern matching on `Option` to deal with the
possibility of `Some` or `None`. In this case, `firstOrZeroWhenEmpty` accepts a
`List` of `Integers` and returns the first element of the list (its "head", as
opposed to the longer "tail" behind it) or `0` if the list is empty.

```console
$ wake -vx firstOrZeroWhenEmpty
firstOrZeroWhenEmpty: (l: List Integer) => Integer = <tutorial.wake:42:[16-18]>
```

A rich selection of functions exists for operating on `Option` values.  For
example, a more concise way of accomplishing the same goal is to use `getOrElse`
which will return the value in a `Some` or a default if the `Option` is `None`.

```wake
export def firstOrZeroWhenEmpty2 l =
    getOrElse 0 (head l)
```

### Result

`Options` are a great way to deal with things that can fail in one obvious way
(eg. `head` fails when the `List` is empty), but sometimes an operation may have
multiple ways of failing (eg. reading a file, it may not exist or permission may
be denied).  wake provides a type called `Result` for dealing with such cases.
`Result` is similar to `Option`: it can be either a `Pass p`, where `p`
represents the value of correct operation, or a `Fail f`, where `f` is a
description of how an operation failed.

```console
$ wake -x 'stringToRegExp ".?xp"'
Pass (RegExp `.?xp`)
$ wake -x 'stringToRegExp "*bad*"'
Fail (Error "no argument for repetition operator: *" Nil)
```

Don't pay too much attention to the exact strings, we're currently mainly
interested in the output structure of the functions.  Namely, that the first
command returned a `Pass` containing an accurate (if verbose) representation of
what we gave it, while the second returned a `Fail` with some `Error` describing
what was wrong with the input.  We can see those same inner types reflected in
the type of the function:

```console
$ wake -vx stringToRegExp
stringToRegExp: (str: String) => Result RegExp Error = </usr/local/share/wake/lib/core/regexp.wake:43:[12-39]>
```

Like any other type, we can `match` on `Results`:

```wake
export def regexOrOnlyEmpty regexString =
    match (stringToRegExp regexString)
        Pass regex = regex
        Fail _ = `^$`
```

This will attempt to parse a regex from an unrestricted `String`, and fall back
on a regex representing an empty string if the parse failed for any reason.
Similarly to `getOrElse`, there is a function `getWhenFail` that will return the
value in a `Pass` or a default in the case of `Fail`.  We can rewrite the above
as simply:

```wake
export def regexOrOnlyEmpty2 regexString =
    getWhenFail `^$` (stringToRegExp regexString)
```

You may have noticed that the `Results` above contain a type called `Error`.
`Error` is a type that contains a `String` "cause", and a `List` of `Strings`
stack trace.  You might also have noticed that the cause is `"no argument for
repetition operator: *"`, but the stack is just `Nil`.  Wake does not actually
maintain a call stack like traditional languages, so by default `Errors` will
contain an empty `List` for the stack.  If you run wake with `-d` (or
`--debug`), it will simulate a stack:

```console
$ wake -dx 'stringToRegExp "*bad*"'
Fail (Error "no argument for repetition operator: *" ("stringToRegExp@wake: /usr/local/share/wake/lib/core/regexp.wake:45:[12-16]", "top: src/optimizer/tossa.cpp:185:1", Nil))
```

You can construct an `Error` directly, or use `makeError` which simply takes a
`String` cause and will record the Stack. There is even another function
`failWithError` which takes that same `String` and returns a `Result a Error`
for the very common case where you'd wrap the `Error` in a `Fail` immediately
after creating it.

## Executing shell jobs

In writing build instructions, we frequently want to execute arbitrary commands
in the system shell.  Wake provides this ability through the job system, in
order to allow their execution to be cached.

```wake
from wake import _
from plan_scorer import _

export def infoH _args =
    def cmdline =
        "{which "uname"} -sr"
    def plan =
        makePlan "get kernel" Nil cmdline
    def os =
        runJob plan
    def str =
        getWhenFail "" os.getJobStdout
    def body =
        "#define OS {str}\n#define WAKE {version}\n"
    write "{@here}/info.h" body    # created with mode: rw-r--r--
```

This example creates a header file suitable for inclusion in some C/C++ project.
To understand what's happening in this example, let's break down all the new
methods being leveraged.

As before, `def` introduces `infoH` as a new function with a single argument
`_args`, which it ignores (as before, the leading underscore silences a compiler
warning).  Making it a function like this even though it just discards the
argument serves two purposes.  First, wake evaluates objects (without arguments)
as soon as they're encountered but functions only when they're fully called, so
if we were to write it `export def infoH =` then any time we did *anything* with
wake it would write an `info.h` file.  The second we'll look at soon.

`which` is a function which searches wake's path for the named program.  On
most systems, `which "uname" = "/bin/uname"`, but this may not always be the
case.  Using `which` buys us a bit of indirection and is usually good form to
use with jobs.

`makePlan` is the main method wake uses to set up calls to external shell
scripts or tools.  It takes three arguments: a readable name for what the job is
doing, a list of paths visible to the job, and a string for the
command-line.  We will go into more detail on that and `runJob` (which is where
the command is actually invoked) in the next section.

The values returned by `runJob` can be accessed in many ways. In this case, we use
`getJobStdout` to get the standard output from the command; since an empty
string is a reasonable default for our use, we use `getWhenFail ""` to recover
in case the job failed. There is other information we can get from a `Job`
object: `getJobStatus` is an `Integer` equal to the job's exit status,
`getJobOutputs` returns a `List` of `Paths` created by the job, among others.

`version` is just a `String` with the current wake version, and `@here` is a
`String` with the directory of the wake file.  We also have an example of a
comment, `# ...`, reminding us of the default permissions used by `write`.

```console
$ wake infoH
Pass (Path "info.h")
$ cat info.h
#define OS Linux 5.10.16.3-microsoft-standard-WSL2

#define WAKE 0.25.0
```

Next, notice that we did not use `-x` when invoking wake this time.
The default operation of wake invokes the subcommand (in this case
`infoH`) specified on the command-line, with any additional command-line
arguments passed to that function.  This is the second reason for the `_args`:
this form of invocation requires a function which can take a `List String`
containing any following command-line arguments.  In this case, it was `Nil`
since nothing followed the `wake infoH`, but if we had written
`wake infoH example` then `_args` would have been `"example", Nil`.

As a side effect of this feature, any command-line arguments you want to pass to
`wake` itself -- rather than to the function invoked -- must be given *before*
the function name: `wake -v infoH` will increase the verbosity as expected,
while `wake infoH -v` will not.

Each executed job -- in other words, when wake calls out to an external program
-- is recorded, and we can retrieve all the information about them as desired.

```console
$ wake --last
Job 3:
  Command-line: /bin/uname -sr
  Environment:
    PATH=/usr/bin:/bin
...
```

Notice that the listed environment is much simpler than the actual environment
of your system; wake tries to minimize anything which might cause two runs to
give different results, and that includes any local environment variables.

### Customizing job invocation

Using `runJob`, we run processes with the default execution plan.
This means that all environment variables are removed and the job is
executed in the root of the workspace. However, it is possible to
customize the environment used.

```wake
from wake import _
from plan_scorer import _

export def showEnv _ =
    def plan =
        "echo $HAX $FOO"
        | makePlan "print from environment" Nil
        | editPlanEnvironment ("HAX=peanut", "FOO=bar", _)
    def output =
        runJob plan
        | getJobStdout
        | getWhenFail ""
    println output
```

```console
$ wake showEnv
peanut bar

Unit
$ wake --last
Job 6 (print from environment):
  Command-line: /bin/dash -c 'echo $HAX $FOO'
  Environment:
    HAX=peanut
    FOO=bar
    PATH=/usr/bin:/bin
...
```

The underlying job execution model of wake uses two phases. First one
constructs a `Plan` object which describes what should be executed.
The `Plan` is then transformed into a `Job` by `runJob`. Before one
calls `runJob`, one can change various properties, like the environment
variables in this example.

In this case, we construct the `Plan` using `makePlan` which, in addition to the
command to run and the files the command may access, also asks for a string it
can use to label the resulting job; we can see it show up in the first line of
the `--last` output, which can often be easier to debug than needing to read the
command line itself.  Other methods of making `Plan` objects -- such as
`makeExecPlan` -- may or may not ask for a label, but one can always be added
with `setPlanLabel`.

Finally, `println` is very useful when debugging wake code. However,
be forewarned that the execution order of wake is not sequential!
This can result in `print` output that does not appear to follow the
definition order of your build program.

## Build rules with file visibility

To illustrate wake's use as a build system, we'll use a few simple programs
written in C++.  This is certainly not the only language which wake can be used
for, but it's the only one which the wake standard library provides builtin
functions to handle.

```wake
from wake import _
from gcc_wake import compileC linkO

def variant = "native-cpp11-release"
export def buildSimple _ =
    require Pass mainSrc =
        source "main.cpp"
    require Pass main =
        compileC variant ("-I.", Nil) Nil mainSrc
    linkO variant Nil main "simple" Nil
```

Let's ignore the contents of `tutorial.wake` briefly and instead focus on how
wake behaves.

```console
$ git init .
$ echo 'int main() { return 0; }' > main.cpp
$ git add main.cpp
$ wake buildSimple
Pass (Path "simple.native-cpp11-release", Nil)
```

(The git commands are truly important; by default wake finds files through the
git index.  Additionally, the `./simple.native-cpp11-release` the `Path` refers
to is a complete and executable file, even if it does literally nothing at the
moment.)

As the build instructions become more complex, the `--last` output becomes
larger and harder to sort through. You can always pipe it through less or your
favorite pager, or you can filter it by the specific files involved:

```console
$ wake --input main.cpp
Job 9 (compile c++ main.native-cpp11-release.o):
...
$ wake --output tutorial.native-cpp11-release
Job 13 (link c++ simple):
  Command-line: /usr/bin/c++ -o simple.native-cpp11-release main.native-cpp11-release.o -std=c++11
  Environment:
    PATH=/usr/bin:/bin
  Directory: .
...
```

If the resulting file is removed, or `main.cpp` modified, then wake will,
of course, rebuild the output.

```console
$ rm simple.native-cpp11-release
$ wake buildSimple
Pass (Path "simple.native-cpp11-release", Nil)
$ wake --last
Job 17 (link c++ simple):
...
```

Because of the underlying database, wake can be a bit smarter about rebuilds
when the contents of the file haven't changed:

```console
$ touch main.cpp
$ wake buildSimple
Pass (Path "simple.native-cpp11-release", Nil)
$ wake -o simple.native-cpp11-release
Job 17 (link c++ simple):
...
```

Notice that wake does not rebuild the object file in this case (the job ID
in my output is still 17).  It checked that the hash of the input has not
changed and concluded that the existing output would have been exactly
reproduced by gcc.

Now let's turn our attention back to the wake file:

```console
$ tail -n 10 tutorial.wake
from wake import _
from gcc_wake import compileC linkO

def variant = "native-cpp11-release"
export def buildSimple _args =
    require Pass mainSrc =
        source "main.cpp"
    require Pass main =
        compileC variant ("-I.", Nil) Nil mainSrc
    linkO variant ("-lm", Nil) main "simple" Nil
```

The `from ... import ...` lines indicate that we want to use something from an
external package. Any file which doesn't specify *any* imports will
automatically import everything in the standard `wake` package, but as soon as
we explicitly add an `import` , we need to explicitly list the `wake` import
ourselves.  The package system is described in more detail in its
[own documentation](tour/packages.adoc).

While this is not a tutorial about C/C++, it's also worth noting a few things
about how the C/C++ build process works before continuing.  First, you'll notice
that the above code has split it into the two steps of `compileC` and `linkO`;
this directly reflects the underlying process where every source file is first
compiled to object code individually, and then afterward the multiple object
files are linked into a single binary.  GCC typically hides that complexity, but
you can still see it reflected in some basic Makefiles as well:

```makefile
OBJ = main.o
main: ${OBJ}
	gcc -o $@ ${OBJ}
.cpp.o:
	gcc -c $<
```

The `def variant = "native-cpp11-release"` line similarly refers to a predefined
set of flags that wake provides for C++ code.  Since we'll only ever access it
from within functions in this file, we don't need to mark it `export`.  The full
range and implications of the variant system are outside the scope of this
tutorial, but just know that the `String` will show up in several places through
the output.

The `source` function verifies that a "source file" exists, returning a
`Result Path Error` pointing to it if it does. This is also where the git
commands come into play, as `source` only searches the git index to find files.
Don't forget to add anything you create!

Since we need to use the inner value of the `Result` rather than the wrapper
itself, we use the common destructor pattern `require Pass x = ...` to obtain
it.  If `source` does indeed have trouble and returns a `Fail`, the `require`
will stop the rest of `buildSimple` from being run and instead simply return
that `Fail` at the top level; otherwise we assign the `Path` to `mainSrc`.

Now that we have the source file, `buildSimple` invokes the compileC function:

```console
$ wake --in gcc_wake -vx compileC
compileC: (variant: String) => (extraFlags: List String) => (headers: List Path) => (cfile: Path) => Result (List Path) Error = </usr/local/share/wake/lib/gcc_wake/gcc.wake:78:[12-37]>
```

The type of `compileC` is a bit more complex than others we've looked at
previously, but taking one element at a time, it should be read as "a function
that takes a `String` named `variant`, then a `List` of `Strings` named
`extraFlags`, then a `List` of `Paths` named `headers`, another Path named
`cfile`, and finally returns a `List Path` if nothing failed but an `Error` if
something did."

Indeed, we can see in our use of `compileC`, we passed a `String` for the
first argument and `mainSrc` -- which is indeed a `Path` -- for the last
argument, while the second and third arguments are `Lists`.

This returns a `Result (List Path) Error` pointing to the object file, which,
through the same sequence as the source file, gets passed to `linkO` for
assembly into a binary.  As this is then the last command in the `def` block,
the output is returned as the function result.

## Building targets from multiple files

Of course, most programs are going to split across several files.

```wake
export def buildMultiple _ =
    def mainResult =
        require Pass mainSrc =
            source "main.cpp"
        compileC variant ("-I.", Nil) Nil mainSrc
    def helpResult =
        require Pass helpSrc =
            source "help.cpp"
        compileC variant ("-I.", Nil) Nil helpSrc
    def multipleResult =
        require Pass main = mainResult
        require Pass help = helpResult
        linkO variant ("-lm", Nil) (main ++ help) "multiple" Nil
    multipleResult
```

```console
$ echo -e '#include "stdio.h"\nvoid helper() { printf("Built with wake\\n"); }' > help.cpp
$ git add help.cpp
```

While most of the changes are minor, the separation of `mainResult` and
`helpResult` into the two steps of `def` followed by `require` is the most
obvious.  This may initially seem like it creates an unnecessary intermediate
variable, but it is in fact very important for achieving the best performance.
Namely, in order for `require` to define values or return early based on the
exact constructor it's passed, it enforces a degree of serialization on the
code.  `def` doesn't have the same restriction, so by starting the computation
in `def` blocks, we allow wake to compile `main.cpp` and `help.cpp` in parallel.
(The `require` unwrapping within `multipleResult` ensures they're both compiled
before being linked, even though each `def` block can be started out of order.)

```console
$ wake buildMultiple
Pass (Path "multiple.native-cpp11-release", Nil)
$ wake --last
Job 25 (compile c++ help.native-cpp11-release.o):
  Command-line: /usr/bin/c++ -std=c++11 -Wall -O2 -I. -c help.cpp -frandom-seed=help.native-cpp11-release.o -o help.native-cpp11-release.o
...
Job 29 (link c++ multiple):
  Command-line: /usr/bin/c++ -o multiple.native-cpp11-release main.native-cpp11-release.o help.native-cpp11-release.o -std=c++11
...
```

Note that there's no entry for compiling `main.cpp`.  Because wake invokes the
compilation and linking steps separately, it was able to recognize that
`main.cpp` had already been compiled -- even though that compilation happened in
a different function in a previous invocation.

```console
$ rm *.o
$ wake buildMultiple
Pass (Path "multiple.native-cpp11-release", Nil)
$ wake --last
Job 31 (compile c++ main.native-cpp11-release.o):
...
Job 32 (compile c++ help.native-cpp11-release.o):
...
```

Similarly, wake did NOT re-link the objects of the program despite needing to
build them again.  That's because wake remembers the hashes of the objects it
gave to the linker last time.  The rebuilt object files have the same hashes, so
there was no need to re-link the program.  Similarly, whitespace-only changes to
the files will not cause a re-link.

```console
$ echo 'int main() { return 2; }' > main.cpp
$ wake buildMultiple
Pass (Path "multiple.native-cpp11-release", Nil)
$ wake --last
Job 39 (compile c++ main.native-cpp11-release.o):
...
Job 43 (link c++ multiple):
...
```

Of course, if you change `main.cpp` meaningfully, it will be recompiled (without
also recompiling `help.cpp`) and the program re-linked.

### Map and partial function evaluation

Having to list all cpp files is cumbersome. You have probably organized
your codebase so that all the files in the current directory should be
linked together.  This example demonstrates how to support that.

```wake
export def buildAll _ =
    def compile =
        compileC variant ("-I.", Nil) Nil
    def objectsResult =
        require Pass srcFiles =
            sources @here `.*\.cpp`
        map compile srcFiles
        | findFail
    def allResult =
        require Pass objects = objectsResult
        linkO variant ("-lm", Nil) (flatten objects) "all" Nil
    allResult
```

Notice that we've defined `compile` to be `compileC` with every argument
supplied *except* the `Path` of the file to compile.
This is known as "partial function evaluation" or "currying".
Thus, `compile` is a function that takes a `Path` and returns a `Path`.
We could equivalently express `compile` as:

```wake
def compile src =
    compileC variant ("-I.", Nil) Nil src
```

The argument `src` here is a bit more explicit, but not strictly necessary.

In either case, this allows us to write `compile cppFile` to
compile a single cpp file, saving some typing.
However, we can use the `map` function to save even more! We use the
`sources` function to find all the `.cpp` files. That gives us a `List` of
`Paths`. Recall that `compile` is a function that takes one argument, a `Path`.
`map` applies the function supplied as its first
argument to every element of the `List` supplied as its second argument.

```console
$ wake buildAll
Pass (Path "all.native-cpp11-release", Nil)
$ wake --last
...
Visible:
  60cde6e2 help.native-cpp11-release.o
  31745228 main.native-cpp11-release.o
Outputs:
  209de066 all.native-cpp11-release
```

After the `map`, we're left with a `List (Result (List Path) Error)`, or several
compilations which would each return some `Paths` if they succeed, but which
may individually fail. However, `require` can only unwrap the outermost type, so
we need to switch the `List` and the `Result`. This is exactly what `findFail`
does: if any of the inner computations failed, then the entire thing fails, but
otherwise it returns the successes as a `Result (List (List Path)) Error`.
Once we unwrap that, we can pass the list of lists to `flatten` (which has type
`List (List a) => List a`, concatenating all of the lists) to get the
`List Path` that we need to link.

Our wake file is now both smaller and will automatically work when new `.cpp`
files are added.

## Supplemental file visibility

Recall that the third argument to `compileC` is a list of additional visible
files.  Wake forbids jobs from reading files in the workspace that are not
visible to the job.  This means that if you include header files, they must be
listed in the files passed to compileC or the compile will fail.

```wake
export def buildHeaders _ =
    require Pass headers =
        sources @here `.*\.h`
    def compile =
        compileC variant ("-I.", Nil) headers
    def objectsResult =
        require Pass srcFiles =
            sources @here `.*\.cpp`
        map compile srcFiles
        | findFail
    def headersResult =
        require Pass objects = objectsResult
        linkO variant ("-lm", Nil) (flatten objects) "headers" Nil
    headersResult
```

```console
$ echo 'void helper();' > help.h
$ echo -e '#include "help.h"\nint main() { helper(); }' > main.cpp
$ git add help.h
$ wake buildHeaders
Pass (Path "headers.native-cpp11-release", Nil)
```

Indeed, we can see that failure if we try to run `buildSimple` or
`buildMultiple` now that `main.cpp` depends on a header -- if you run into
trouble with jobs missing files, checking the "Visible" list will give you a
better understanding than your local filesystem:

```console
$ wake buildSimple
main.cpp:1:10: fatal error: help.h: No such file or directory
 #include "help.h"
          ^~~~~~~~
compilation terminated.
Fail (Error "Non-zero exit status (Exited 1) for '/usr/bin/c++ -std=c++11 -Wall -O2 -I. -c main.cpp -frandom-seed=main.native-cpp11-release.o -o main.native-cpp11-release.o'" Nil)
$ wake -v --failed
...
Visible:
  0b10435dd8947e57cbad4f4326d65dd0909c026b8e2bcaa2f87c9e6018507451 main.cpp
Outputs:
Stderr:
  main.cpp:1:10: fatal error: help.h: No such file or directory
   #include "help.h"
            ^~~~~~~~
  compilation terminated.
```

In `buildHeaders`, we've used the `sources` command to find all the header
files in the same directory and pass them as visible files to gcc -- the
keyword `@here` expands to the directory of the `.wake` file.  The second
argument to `sources` is a regular expression to select which files to
return. We've used ``` `` ```s (backticks) here which define regular expression literals
with the [standard syntax](https://github.com/google/re2/wiki/Syntax).
In addition, the parser verifies that regular expression literals are legal.

Note that "source files" are those files tracked by git. Wake will never
return built files from a call to `sources`, helping repeatability. We can see
this through the fact that the `info.h` file we generated is not listed in the
visible list, despite the regex supposedly matching all `.h` files in the
current directory. If we did want to use it, we'd need to reference the `Path`
returned when it is created (or retrieved from the cache):

```wake
def infoResult =
    infoH Nil
def compile =
    require Pass info = infoResult
    def visible =
        info, headers
    compileC variant ("-I.", Nil) visible
```

In a classic Makefile it would be considered bad form to list all header
files as dependencies for all cpp files.  That's because make would
recompile every cpp file whenever any header file changes.  In wake, we
don't have this problem.  Wake monitors jobs to see which files they
actually used and remembers this for later builds.  Therefore, it's best in
wake to err on the side of caution (and convenience) by just listing all the
headers in directories that are interesting to the cpp files.

```console
$ wake -o main.native-cpp11-release.o
...
Visible:
  d2a7bfde help.h
  9d220741 main.cpp
Outputs:
  810e17cb main.native-cpp11-release.o
```

For this file, wake recorded that it needed both `main.cpp` and `help.h`.

```console
$ wake -o help.native-cpp11-release.o
...
Visible:
  5b8beead help.cpp
Outputs:
  13350319 help.native-cpp11-release.o
```

For this file, wake recorded that it only needed `help.cpp`, despite
`help.h` being a legal input.

## Publish/Subscribe

Wake includes a publish/subscribe interface to support accumulating information
between multiple files. `publish x = y` adds `y` to the `List` of things which
will be returned by a `subscribe x` expression.

```wake
topic animal: String
publish animal =
    "Cat", Nil
publish animal =
    "Dog", "Wolf", Nil
publish animal =
    replace `u` "o" "Mouse", Nil
export def animals =
    subscribe animal
```

```console
$ wake -x 'animals'
"Cat", "Dog", "Wolf, "Moose", Nil
```

Note that `animal` is not a variable; it is a topic, which is in a different
namespace than normal variables. Note also that `y` must be a `List`.

For example, this API can be used to accumulate all the unit tests in the
workspace into a single location that runs them all at once.  Keep in mind that
the published `List` can be of any type (including functions and data types), so
the types of workspace-wide information that can be accumulated this way is
wide open.  However, all publishes to a particular topic must agree to use
the same type in the `List`, or the files will not type check.

## Downloading and parsing files

Consider a build setup where the local project depends on the sources of some
external one.  Especially for security-sensitive dependencies where using the
most recent version (no matter what version that may be), this is the sort of
example that becomes very difficult very quickly in a system like make, but
is fairly straight-forward in wake:

```wake
from wake import _
from plan_scorer import _

def curl url extension =
    def outputFile =
        # This construction is somewhat specific to GitHub's url scheme, which
        # names release tarballs according to the tag version, but doesn't
        # include the `.tar.gz` (or `.json`) extension.
        "{@here}/{basename url}.{extension}"
    def cmdline =
        "{which "curl"} -o {outputFile} {url}"
    def curl =
        makePlan "download {url}" Nil cmdline
        | runJob
    curl.getJobOutput

export def downloadGithubRelease projectList =
    require (project, Nil) = projectList
    else failWithError "Exactly one project must be downloaded per call"
    def releasesResult =
        # Query the GitHub microservice for a structured list of releases.
        # Note that this is rate-limited, so you might want a different setup if
        # including this as part of CI for a large project.
        curl "https://api.github.com/repos/{project}/releases" "json"
    def releaseTarballsResult =
        require Pass releases = releasesResult
        require Pass releasesData =
            parseJSONFile releases
        def releases =
            # Search through the list of all releases to retrieve the values of
            # the `tarball_url` field.  Don't spend too much time memorizing
            # this particular JSON interface; it will be replaced soon.
            releasesData // `tarball_url`
            | getJArray
            | getOrElse Nil
        Pass (mapPartial getJString releases)
    def mostRecentUrlResult =
        require Pass releaseTarballs = releaseTarballsResult
        # The returned JSON results are ordered newest-first.
        head releaseTarballs
        | getOrFail "No GitHub releases found for {project}".makeError
    def mostRecentTarballResult =
        require Pass mostRecentUrl = mostRecentUrlResult
        curl mostRecentUrl "tar.gz"
    mostRecentTarballResult
```

```console
$ wake downloadGithubRelease sifive/wake
  % Total    % Received % Xferd  Average Speed   Time    Time     Time  Current
                                 Dload  Upload   Total   Spent    Left  Speed
100  385k    0  385k    0     0   501k      0 --:--:-- --:--:-- --:--:--  501k
  % Total    % Received % Xferd  Average Speed   Time    Time     Time  Current
                                 Dload  Upload   Total   Spent    Left  Speed
  0     0    0     0    0     0      0      0 --:--:-- --:--:-- --:--:--     0
Pass (Path "v0.25.2.tar.gz")
```

As we've covered before, `job` is used to launch curl to download the table.
The `curl` function also makes use of `replace` with a regular expression
to split the filename out of the URL. `replace` accepts a "replacement" `String`
which it substitutes for every substring that matches the regular expression. We also use
the `simplify` function which transforms paths into canonical form.  In this,
case `simplify` removes the leading `"./"`.

The `require project, Nil =` line is helpful for restricting a list to contain
exactly one item.  We could also use `head` and `require Some` or
`getOrElse`/`getOrFail` to discard any arguments after the first, but in this
case we have chosen to fail loudly.

The GitHub API returns a JSON document, for which wake has built-in handling.
The full library will be described elsewhere, but for a brief overview of what
is used in this example, `parseJSONFile` does as the name suggests and reads a
file (here retrieved by `curl`, but it could also be from `source` or any other
call) into an internal representation which we then search through for all
`tarball_url` keys.

By passing the resulting URL to a second call of `curl`, we're able to retrieve
the required files *determined by* the output of a previous job.  This ability
for the complex interweaving of programming language features with build-system
output caching is where the full power of wake truly shines.

## Ignore wake source files

By default, wake will recursively search the entire workspace for build files
(`*.wake`). However, there are occasions where you want it to ignore some of
them: for example, during testing where you could exclude some files from
regular usage, or a directory structure that includes duplicate repository
checkouts, where duplicate symbol definitions would raise an error.

To allow these, wake looks for files named `.wakeignore` containing patterns.
The pattern language is shell filename globbing.
One pattern per line, each relative to the path of the `.wakeignore` file.

The concrete syntax is:
- empty lines are ignored
- lines starting with a `#` are comments and are ignored
- `?` matches a single non-slash character
- `*` matches any number (including zero) of non-slash characters
- `[a-z]` matches a single lower-case character
- `/**/` in an expression like `foo/**/bar` stands in for any number of directories (including zero)
- `foo/**` recursively matches all contents of the directory `foo`
- `**/bar` matches all files `bar` contained in this directory or any subdirectory

An example:

    workspace
        ├── wake.db
        ├── repo1
        │   └── foo.wake
        └── repo2
            ├── .wakeignore
            └── repo1
                └── foo.wake

If `repo1` is checked-out out twice like above, then if a `.wakeignore` file
at path `workspace/repo2` contained `repo1/**` then `foo.wake` would not
be read twice.

The following patterns in `workspace/repo2/.wakeignore` would all match
the above `foo.wake`:

```ignore
repo1/foo.wake
repo1/**
repo1/**/foo.wake
**/[a-z]?[o].wake
```
