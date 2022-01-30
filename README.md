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

#### Problems:
There is a problem of (as it seems) julia's GC sometimes conflicting with houdini, you can see discussion [here](https://discourse.julialang.org/t/segfault-and-crash-embedding-when-julia-runs-multithreaded-gc/75221)

