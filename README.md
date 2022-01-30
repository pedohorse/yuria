## Julia Language wrangle for SideFx Houdini

This is a slowly moving personal project.
Tested only on linux with h18.5, h19.0 and julia 1.7.1

#### Building
##### Linux
* download julia release [1.7.1](https://julialang.org/downloads/) (or try later ones) 
* put it's contents into:
  * julia directory in the root of the repository - this is needed just for includes
  * julia directory in `hfs/dso/julia` - compiled .so will be looking for julia library there
* adjust paths in tasks.json if you are using vscode

should work. now you can add path to `hfs` directory to your `HOUDINI_PATH` (or move it somewhere already in `HOUDINI_PATH`), and houdini will load it

use `HOUDINI_DSO_ERROR=2` to catch so loading errors

ye, it's not yet very streamlined

#### using
The plugin provides a SOP node haned `juliasnippet`, there you can put initialization code (usings, includes and declarations), and the code of a function to run every cook

Attribute bindings happen bu name, according to attribute masks in node parameters  
So if you map P - P will be available in function code (not initialization code) as a 3xN matrix  
If you rebind it - you will loose connection to houdini. So instead - modify it inplace only
DO NOT RESIZE BINDED ATTRIBUTES INPLACE - houdini will not catch that (yet)

For not it's limited only to binding point attributes, that will be changed in the future

If you want julia to run multithreaded - provide environment variable to houdini `JULIA_NUM_THREADS=auto` (or any specific value instead of auto)  
**BUTT BEWARE** - in multithreaded mode the **problem** below will apply
If that variable is not provided - julia will start in single-threaded mode, you can check number of threads with `Threads.nthreads()`

#### Problems:
There is a problem of (as it seems) julia's GC sometimes conflicting with houdini, you can see discussion [here](https://discourse.julialang.org/t/segfault-and-crash-embedding-when-julia-runs-multithreaded-gc/75221)

