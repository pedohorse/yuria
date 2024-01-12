## Julia Language wrangle for SideFx Houdini

This is a slowly moving personal project.

There is a bunch of unix-specific hacks in here that cannot be trivially translated to windows, so there is **currently no windows version available**.

#### Installing

check the [release](https://github.com/pedohorse/yuria/releases) section for latest release.

release files are typically named like: `build-v1.0.2-h20.0.547-j1.10.0.zip`, where 
- `v1.0.2` is the plugin's version
- `h20.0.547` is the houdini version for which the plugin was compiled, though the build number (last number) does not matter, this will work for any houdini 20.0 build.
- `j1.10.0` is the julia version for which the plugin was compiled. You will need to download that exact version of julia for the plugin to work.

##### to your user dir
1. find appropriate archive from the [release](https://github.com/pedohorse/yuria/releases) page.
2. copy `dso` dir from it to your houdini userdir, like `houdini19.0`, so you get `houdini19.0/dso/sop_julia.so`
3. you need to put appropriate release of Julia into `dso/julia`. there is an `.sh` file that will do that automatically if you run it.
4. Alternatively, to download Julia manually, download appropriate version from [julia's homepage](https://julialang.org/downloads/).
   If you used release file `build-v1.0.2-h20.0.547-j1.10.0.zip` - you will need Julia 1.10.0.
   - Unpack the contents into `dso/julia` dir.
your file structure should look like this:  
```
houdini19.0
      |\dso
         |\sop_julia.so
         |\julia
             |\bin
             |\lib
             |\share
             ....
```
##### anywhere else
You can put in anywhere DSOs are scanned for in your setup,  
Just make sure that wherever you put the `sop_julia.so` - the `julia` release dir (see above) is right next to it.

#### using
The plugin provides a SOP node haned `juliasnippet`, there you can put initialization code (usings, includes and declarations), and the code of a function to run every cook

Attribute bindings happen bu name, according to attribute masks in node parameters  
So if you map P - P will be available in function code (not initialization code) as a 3xN matrix  
If you rebind it - you will loose connection to houdini. So instead - modify it inplace only
DO NOT RESIZE BINDED ATTRIBUTES INPLACE - houdini will not catch that (yet)

Vertex, Point, Primitive, Detail attributes - all are considered for binding (and in that order)  
You should not have different class attributes with same names, only first one (in order above) will be binded in that case.

If you want julia to run **multithreaded** - provide environment variable to houdini `JULIA_NUM_THREADS=auto` (or any specific value instead of auto)  
~~**BUTT BEWARE** - in multithreaded mode the **problem** below may apply.~~  

If that variable is not provided - julia will start in single-threaded mode, you can check number of threads with `Threads.nthreads()`

#### Environment variables:
All julia environment variables will work as expected, nothing is set differently from the defaults.  
Therefore it's important to set `JULIA_NUM_THREADS=auto` in case you want multithreaded julia

while all julia variables start with `JULIA_`  
all plugin-specific veriables start with `YURIA_`

* `YURIA_DEBUG` - if set all debug output about plugin's work will be printed to stdout.

#### installing Julia packages
For now there is no embedded into houdini julia console.  
So you will need to start julia shell of the same version you use for houdini (also the same `JULIA_PROJECT` if set, and other config environment variables)

install all the packages you need the standard way through julia's interactive shell. After that you will be able to import them into houdini snippets.

#### Building
##### Linux
* download [julia release](https://julialang.org/downloads/) you need
* put it's contents into:
  * julia directory in the root of the repository - this is needed just for includes
  * julia directory in `hfs/<version>/dso/julia` - compiled .so will be looking for julia library there
* set `HFS` environment variable to point your Houdini intallation location.
* `make install` will build yuria plugin, and put `sop_julia.so` into `hfs/<verison>/dso/.`

should work. now you can add path to `hfs/<version>` directory to your `HOUDINI_PATH` (or move it somewhere already in `HOUDINI_PATH`), and houdini will load it

use `HOUDINI_DSO_ERROR=2` to catch so loading errors

##### other OSes
not yet.

due to some os-specific things in julia it might not be as simple as just recompiling it. MacOS has higher chance to not require any modifications, while windows handles signals completely different.


#### Known Problems:
~~There is a problem of (as it seems) julia's GC sometimes conflicting with houdini~~ the problem seem to be mitigated, you can see discussion [here](https://discourse.julialang.org/t/segfault-and-crash-embedding-when-julia-runs-multithreaded-gc/75221)

[why yuria?](https://youtu.be/frlyZzYG1So?t=35)
