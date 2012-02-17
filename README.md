BVIM, gvim with VS2010 extensions
=======================================================

bvim is a hacked version of gvim which adds a few features which helps working on large Visual Studio 2010 projects.

The goal is to make all common programming actions take less than 500ms on a fast machine.

Bvim is supported on Windows 7 and builds with VS2010.

Build bvim using src/vim_vs2010.sln

boresln <visual studio .sln file>
------------------------------------------------------
Open a solution and build a list of all files that are included in the projects. This must be the first thing done in order to use the other commands.

boreopen
-------------------------------------------------------
Open a help-like window listing all files in the solution. Use / to search for the wanted file and press Enter to open it.

borefind <string>
-------------------------------------------------------
Do a case sensitive search through all files in the solution for <string>. At most 100 hits per file is reported and the total hits is capped to 1000. 

g:bore_filelist_file
-------------------------------------------------------
The filename of the file which contains a list of all files included in the solution. Useful for e.g. building a tags file from all solution files.

