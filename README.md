## Julia Language wrangle for SideFx Houdini

This is a slowly moving personal project.
Tested only on linux with h18.5, h19.0 and julia 1.7.1

#### Building
##### Linux
* download julia release [1.7.1](https://julialang.org/downloads/) (or try later ones) 
* put it's contents into:
  * julia directory in the root of the repository - this is needed just for includes
  * julia directory in `hfs/<version>/dso/julia` - compiled .so will be looking for julia library there
* adjust paths in tasks.json if you are using vscode

should work. now you can add path to `hfs` directory to your `HOUDINI_PATH` (or move it somewhere already in `HOUDINI_PATH`), and houdini will load it

use `HOUDINI_DSO_ERROR=2` to catch so loading errors

ye, it's not yet very streamlined

#### Installing
##### to your user dir
1. find appropriate `sop_julia.so` (or `dll` on windows (or whatever extention mac is using)) for appropriate houdini version. Build should not matter, only major.minor versions are important.
2. copy it to your houdini userdir's dso folder, like `houdini19.0/dso`
3. unpack julia release [1.7.1](https://julialang.org/downloads/) (or the version you built .so with) to `julia` folder in the same place  
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

If you want julia to run multithreaded - provide environment variable to houdini `JULIA_NUM_THREADS=auto` (or any specific value instead of auto)  
**BUTT BEWARE** - in multithreaded mode the **problem** below will apply.  
If that variable is not provided - julia will start in single-threaded mode, you can check number of threads with `Threads.nthreads()`

#### Environment variables:
All julia environment variables will work as expected, nothing is set differently from the defaults.  
Therefore it's important to set `JULIA_NUM_THREADS=auto` in case you want multithreaded julia

while all julia variables start with `JULIA_`  
all plugin-specific veriables start with `YURIA_`

* `YURIA_DEBUG` - if set all debug output about plugin's work will be printed to stdout.

#### Known Problems:
~~There is a problem of (as it seems) julia's GC sometimes conflicting with houdini~~ the problem seem to be mitigated, you can see discussion [here](https://discourse.julialang.org/t/segfault-and-crash-embedding-when-julia-runs-multithreaded-gc/75221)

[why yuria?](https://youtu.be/frlyZzYG1So?t=35)
