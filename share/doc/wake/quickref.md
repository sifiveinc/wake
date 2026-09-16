

## Wake Quickref

### Invoking wake

1. A database must be created: `wake --init .`

1. The commands available to wake are from
the wake library (share/wake/lib) and all files *.wake in
the wake.db directory and below.

1. wake is invoked normally with a command-line target and its arguments.
   Targets are any wake function in scope with a single `args: List String` parameter.
    ```wake
    wake runThing "a string"
    ```

1. Arbitrary wake expressions can be invoked with `-x`,
   although the entire expression is generally quoted with single quotes to avoid interactions with the shell.
    ```wake
    wake -x 'runThing "a string"'
    ```

1. Other useful flags
    1. `wake -xv <varible>` to get type information for a variable or function that is in scope
    1. Getting information about jobs. For all these commands, adding `-v` will
       include the jobs' stdout/stderr, and adding `-d` will include the Wake
       stack trace of when the job was run.
        1. `wake -o <file>` to get dependency information for jobs which write `file`
        1. `wake -i <file>` to get dependency information for jobs whose visible files include `file`
        1. `wake -g` to get a list of all global symbols
        1. `wake --failed` to get a list of failed jobs
        1. `wake --last` to get a list of the most recent jobs, regardless of status


### Syntax

- Variables and functions defined the same way.

    ```
    def foo x =
        x + 2 

    def bar = 
        foo 7
    ```

* Only one statement is allowed per function. Multiple defs are allowed.

    ```
    def foo x =
        def two = 2
        def add2 y =
            y + two
        add2 x
    ``` 

* To make any definition visible in another file, use global

    ```
    global def foo a b =
        a + b
    ```

* `|`, `.`
    * the pipe character feeds an argument to a function, like in shell scripts.
        * `a 0 (b 1 2 (c 3 d))` becomes `d | c 3 | b 1 2 | a 0`
    * `.` is similar, but is used to make calls look like OOP if there is only one argument:
        * `length string` would become `string.length`
    * In other words, these three are all equivalent:
        * `length string`
        * `string.length`
        * `string | length`

* `→`
    * wake fully supports Unicode. A common use of this is in creating a Pair object
    * `a → b` is the same as `Pair a b`
    * Be aware that there other supported Unicode functions.

### Functional programming constructs

Wake is generally functional.

* first class function/partials
    * All functions are first class object. Partials can be created by leaving off
    arguments to a function.
    
    ```
    def foo a b = 
      a + b
        
    def foo2 = foo 2
       
    def bar a =
      foo2 a
    ```

* pure functions
    * functions usually have no side effects and are reordered.
    
* statements
    * all commands return a value, and can be used interchangeably
    
    ```
    def x = if False 6 else 4
    ```

* pattern matching
    * There is a match statement
    
    ```
    def fib x = match x
      0 = 0
      1 = 1
      2 = 2
      x = fib(x-1) + fib(x-2)
        
    def collatz = match _
      1 = 4
      2 = 1
      4 = 2
      x if x % 2 == 0 = x / 2
      x = 3 * x + 1
    ```

* anonymous functions (lambda). Both forms work.

    ```
    (\x x + 2)
    
    (_ + 2)
    ```
    
* common list functions (meanings are standard)

|       |     |     |     |     |
| ---   | --- | --- | --- | --- |
| empty |   head | tail | map | foldl |
|scanl | foldr | scanr | ++ (cocatenate lists) | reverse |
| flatten | len | exists | forall | filter| 
| zip| 

* printing output (for debugging)
    * `println thing`
    * `print thing`
    * to format an object for printing: `format thing`

### Data Types

#### Primitives
* String -> "A String" : a string interpreted as unicode
    * a "" string also allows string interpolation using {}
        * for example  `"{foo}{str (4 + 3)}"`
        * also \ is interpreted in a double quoted string
    * 'a binary string' : a raw binary string
    * Multiline strings are also supported
        ```
        def foo = "%
          This string can include "double quotes" safely.
          That's because the string will not end until we match the token %.
           This line is indented by one space. It is illegal to use less spaces.
          Also, this \n is not interpreted.
          However, this %\n will have a line feed between "this" and "will".
          The expression {abcdef} is not interpreted by wake.
          However, %{str (55+5)} == 60, like you'd expect.
          Any terminator can be chosen; replace % with any character you like.
          %"
        ```
* List a -> 1,2,3,Nil (a list of items seperated by commas ended by nil)
* Integer -> 7
* Double -> 3.2
* Tuple -> see below
* JSON -> see below
* Job -> see below
* Path -> see below

* There are tree and vector type. Read the code if these sound useful.

#### Tuples

Tuple are how wake defines record types, much like a struct in c/c++. The name of
a tuple data type begins with a capital letter.

```bash

global tuple Bob =
    global First: Integer
    global Second: Double
    global Third: String

```

Defining a tuple creates several methods for each field:

```
set<tuple name><field name>
get<tuple name><field name>
edit<tuple name><field name>

setBobFirst a b
getBobFirst a
editBobFirst a b
```

#### Json

```bash
# The JSON data type
global data JValue =
  JString  String
  JInteger Integer
  JDouble  Double
  JBoolean Boolean
  JNull
  JObject  (List (Pair String JValue))
  JArray   (List JValue)
```

Wake has many useful function for extracting data from a json structure.


#### Jobs

Jobs are how wake runs external programs. They are created by executing a Plan with 
a Runner. The Plan is a tuple describing how to run the program, and the Runner is 
a function which reads the Plan and runs the program accordingly. 

Jobs provide wake's central useful functionality. Wake can be viewed as a functional
programming language for running jobs.

```
from wake import _
from plan_scorer import _

global def planit =
  "touch foo"
  | makePlan "create file" Nil

global def runit _ =
  planit
  | runJob
  | getJobStdout
  | format
  | println
```
#### Runners

Note: all definitions and functions referenced in this section can be found in [this directory](https://github.com/sifive/wake/blob/master/share/wake/lib/system).

Runners are responsible for executing a Plan to run an external program. There are two built-in base runners: `localRunner` and `defaultRunner`. `defaultRunner` creates a sandbox for jobs to run in while `localRunner` runs jobs in the actual workspace. An additional distinction is `localRunner` does not detect output files by itself, instead requiring that list be populated manually.

Runners are chosen for a given Plan based on the value returned by their `score` function and the plan's `RunnerFilter` field. Multiple runners may be able to run a given Plan, so the `score` function selects the most appropriate one. 

* In `runJob`,  `subscribe runner` is called to retrieve the list of all published runners. The `RunnerFilter` function inside the Plan tuple can be set to filter out runners that the plan wants to exclude from consideration. Then the `score` function of each runner is called and runners that return Fail or a score <= 0.0 are excluded. Of the remaining runners, the one with the highest score is picked.
* The local runners can only be used via `runJobWith localRunner`, or a custom wrapper.

Runners are created using `makeRunner`. `makeRunner` is defined as follows:

```global def makeRunner name score pre post (Runner _ _ run)```

* The `score` argument is of type `Plan → Result Double String` and is called by `runJob` to produce a score representing the priority of a runner with respect to the given Plan. For example, if `plan.getPlanResources` returns list `("python/3.7.1", Nil)` then the RISC-V runner probably cannot provide that resource and should return something like `Fail RISCVRunner: cannot provide resource: python/3.7.1`. If the plan resources is `("riscv-tools/2019.02.0", Nil)` then the `score` function should return `Pass 1.0` or some other positive number.
* The `pre` argument is of type `Result RunnerInput Error → Pair (Result RunnerInput Error) a`. The `RunnerInput` tuple is a subset of the Plan tuple, serving to restrict the fields that the runner can access. The `pre` function is called before the job is run, allowing the runner to modify the input to provide the requested resources. For example, the RISC-V runner would run `runnerInput | editRunnerInputEnvironment addRISCVEnvironment` where `addRISCVEnvironment` is a function that sets the RISCV environment variable. The return value of `pre` is the modified `RunnerInput` paired with a free `a` type that can contain any sideband data that the `pre` function needs to provide to the `post` function.
* The `post` argument is of type `Pair (Result RunnerOutput Error) a → Result RunnerOutput Error` and is similar to the `pre` function but is called after the job has run. `post` is for editing the reported outputs/usage of the job. For example, the fuse runner uses the `post` hook to set the output files reported by the FUSE filesystem.
* The last argument is the base runner that the current runner is built on top of. This is because all runners must be built on top of a preexisting runner. For example, in environment-example-sifive, localRISCVRunner and defaultRISCVRunner are built on top of localRunner and defaultRunner respectively.
* `publish runner` must be executed for Wake to recognize a runner

#### Using environment packages 

Environment packages can be added to a workspace to provide tools (using runners) for running the workflow. They are composed of Wake files that define and publish runners. Environment packages should be defined for each unique running environment to decouple Wake rules from how tools are installed in each environment. You can check out [environment-example-sifive](https://github.com/sifive/environment-example-sifive) as an example of an environment package that contains runners for Wake-based workflows. 

#### Sources and `Path` objects

Source files are files in a Git repository that are either committed or staged. A `Path` in Wake is either

* a source file or
* a file produced from a build step.

The `Path` type has no public constructor function to help enforce this. To get a `Path` value for a source file, use the

* `source: (file: String) => Result Path Error`
* `sources: (dir: String) => (filterRegexp: RegExp) => Result (List Path) Error`

functions. For example, to get all the header files under the current Wake root:

```wake
sources "." `.*\.h`
```

Note that the directory given to `sources` is always relative to the Wake root and not the current directory of a source file. So the above expression, when evaluated, has the same effect whether it is run in a `.wake` file in any directory under the root or as `` wake -x 'sources "." `.*\.h`' ``.

When working inside a file, `@here: String` is a built-in macro that expands to the relative path from the workspace root to the directory containing the source file that contains `@here`. This can be useful when only wanting to source files in the current directory that your source file is in and in any subdirectory.

Examples for what relative directory is returned based upon the source file's location:

```
./build.wake @here -> "."
./src/other.wake @here -> "src"
./src/test/another.wake @here -> "src/test"
```

As a further example, suppose we had a source file `share/wake/lib/gcc_wake/test.wake` in the Wake repository that contained

```wake
package sourcesExample
export def sourcesExample = sources @here `.*\.wake`
```

Then `sourcesExample` would have a value of

```bash
$ wake -x 'sourcesExample' --in sourcesExample
Pass (Path "share/wake/lib/gcc_wake/gcc.wake", Path "share/wake/lib/gcc_wake/pkgconfig.wake", Path "share/wake/lib/gcc_wake/test.wake", Nil)
```

at the time of writing. Note that if `test.wake` was a new, unstaged or uncommitted file, then it would *not* be returned in the result.

### Wakisms

* database
    * wake uses an sqlite3 database to track dependencies, job information and 
    various other detail. This should be treated as an read-only resource.
* FUSE
    * wake uses a FUSE filesystem to track what files are actually opened when running
    a job. 
    * this can be disabled for systems that can't run FUSE
* no guarantee of ordering
    * There is no guarantee of ordering if two statements don't depend on each other. This is
    wake is able to parallelize programs.
* publish/subscribe
    * the publish and subscribe commands are used for global values.
    ```
    topic xfoo: String
    publish xfoo = "bar", Nil
    publish xfoo = "baz", Nil
    
    global def readit _ =
        subscribe xfoo
    ```

* environment
    * the environment is in a published topic, and does not pick up the environment
    from the parent process. It can also be accessed from the global variable `environment`.
    

### Operator precedence table

| Kind       | Order        | Operator (precedence based solely on first character) |
| ---        | ---          | ---                                                   |
| Dot        | Left         | . |
| Application| Left         | (f x) |
| Quantifier | Left-prefix  | √∛∜∏⋂⨀⨂⨅⨉∐∑∫∮∱∲∳⋃⨁⨃⨄⨆⨊⨋⨍⨎⨏⨐⨑⨒⨓⨔⨕⨖⨗⨘⨙⨚⨛⨜⫿⋀⋁∀∁∃∄∎∴∵∷ |
| Exponent   | Right-prefix | ^ |
| MulDiv     | Left-prefix  | */%×∙∩≀⊓⊗⊙⊛⊠⊡⋄⋅⋇⋈⋉⋊⋋⋌⋒⟐⟕⟖⟗⟡⦁⦻⦿⧆⧑⧒⧓⧔⧕⧢⨝⨯⨰⨱⨲⨳⨴⨵⨶⨷⨻⨼⨽⩀⩃⩄⩋⩍⩎÷⊘⟌⦸⦼⧶⧷⨸⫻⫽∘⊚⋆⦾⧇ |
| AddSub     | Left-prefix  | -+~¬±∓∔∪∸∸∹∺∻≂⊌⊍⊎⊔⊕⊖⊞⊟⊹⊻⋓⧺⧻⧾⧿⨢⨣⨤⨥⨦⨧⨨⨩⨪⨫⨬⨭⨮⨹⨺⨿⩁⩂⩅⩊⩌⩏⩐⩪⩫⫬⫭⫾ |
| Test       | Left-prefix  | ∈∉∋∌∝∟∠∡∢∥∦≬⊾⊿⋔⋲⋳⋵⋶⋸⋹⋺⋻⋽⋿⍼⟊⟒⦛⦜⦝⦞⦟⦠⦡⦢⦣⦤⦥⦦⦧⦨⦩⦪⦫⦬⦭⦮⦯⦶⦷⦹⦺⩤⩥⫙⫚⫛⫝̸⫝⫡⫮⫲⫳⫴⫵⫶⫼<≤≦≨≪≮≰≲≴≶≸≺≼≾⊀⊂⊄⊆⊈⊊⊏⊑⊰⊲⊴⊷⋐⋖⋘⋚⋜⋞⋠⋢⋤⋦⋨⋪⋬⟃⟈⧀⧏⧡⩹⩻⩽⩿⪁⪃⪅⪇⪉⪋⪍⪏⪑⪓⪕⪗⪙⪛⪝⪟⪡⪣⪦⪨⪪⪬⪯⪱⪳⪵⪷⪹⪻⪽⪿⫁⫃⫅⫇⫉⫋⫍⫏⫑⫓⫕⫷⫹>≥≧≩≫≯≱≳≵≷≹≻≽≿⊁⊃⊅⊇⊉⊋⊐⊒⊱⊳⊵⊶⋑⋗⋙⋛⋝⋟⋡⋣⋥⋧⋩⋫⋭⟄⟉⧁⧐⩺⩼⩾⪀⪂⪄⪆⪈⪊⪌⪎⪐⪒⪔⪖⪘⪚⪜⪞⪠⪢⪧⪩⪫⪭⪰⪲⪴⪶⪸⪺⪼⪾⫀⫂⫄⫆⫈⫊⫌⫎⫐⫒⫔⫖⫸⫺ |
| InEqual    | Right-prefix | !=≃≄≅≆≇≈≉≊≋≌≍≎≏≐≑≒≓≔≕≖≗≘≙≚≛≜≝≞≟≠≡≢≣≭⊜⋍⋕⧂⧃⧎⧣⧤⧥⧦⧧⩆⩇⩈⩉⩙⩦⩧⩨⩩⩬⩭⩮⩯⩰⩱⩲⩳⩷⩸⪤⪥⪮⫗⫘ |
| And        | Left-prefix  | &∧⊼⋏⟎⟑⨇⩑⩓⩕⩘⩚⩜⩞⩞⩟⩟⩠⩠ |
| Or         | Left-prefix  | \||∨⊽⋎⟇⟏⨈⩒⩔⩖⩗⩛⩝⩡⩢⩣ |
| Dollar     | Right-prefix | $♯¢£¤¥֏؋৲৳৻૱௹฿៛₠₡₢₣₤₥₦₧₨₩₪₫€₭₮₯₰₱₲₳₴₵₶₷₸₹₺₻₼₽₾꠸﷼﹩＄￠￡￥￦ |
| Ascription | Right        | : |
| Assignment | Right-prefix | :↔↮⇎⇔⇵⇹⇼⇿⟚⟛⟠⟷⟺⤄⤡⤢⤫⤬⥈⥊⥋⥌⥍⥎⥏⥐⥑⥦⥧⥨⥩⥮⥯⫩]←↑↚⇷⇺⇽⊣⊥⟣⟥⟰⟲⟵⟸⟻⟽⤂⤆⤉⤊⤌⤎⤒⤙⤛⤝⤟⤣⤦⤧⤪⤱⤲⤴⤶⤺⤽⤾⥀⥃⥄⥆⥉⥒⥔⥖⥘⥚⥜⥞⥠⥢⥣⥪⥫⥳⥶⥷⥺⥻⥼⥾⫣⫤⫥⫨⫫⬰⬱⬲⬳⬴⬵⬶⬷⬸⬹⬺⬻⬼⬽⬾⬿⭀⭁⭂⭉⭊⭋→↓↛↠↣↦⇏⇒⇴⇶⇸⇻⇾⊢⊤⊦⊧⊨⊩⊪⊫⊬⊭⊮⊯⊺⟢⟤⟱⟳⟴⟶⟹⟼⟾⟿⤀⤁⤃⤅⤇⤈⤋⤍⤏⤐⤑⤓⤔⤕⤖⤗⤘⤚⤜⤞⤠⤤⤥⤨⤩⤭⤮⤯⤰⤳⤵⤷⤸⤹⤻⤼⤿⥁⥂⥅⥇⥓⥕⥗⥙⥛⥝⥟⥡⥤⥥⥬⥭⥰⥱⥲⥴⥵⥸⥹⥽⥿⧴⫢⫦⫧⫪⭃⭄⭇⭈⭌ |
| Comma      | Right-suffix | , ; |
