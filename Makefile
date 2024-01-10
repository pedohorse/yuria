# Either run make from houdini-initialized environment, or set HFS by yourself

DSONAME = sop_julia.so

SOURCES = src/sop_julia.cpp

OPTIMIZER = -O3

INCDIRS = -Ijulia/include/julia/

LIBDIRS = -Ljulia/lib/
LIBS = -ljulia

include $(HFS)/toolkit/makefiles/Makefile.gnu

#INSTDIR = hfs/$(subst hfs,,$(notdir $(HFS)))
INSTDIR = hfs/$(shell (cd $(HFS) && source $(HFS)/houdini_setup_bash && eval "echo \$$HOUDINI_VERSION") | tail -n1)

# now correc
CXXFLAGS := $(CXXFLAGS) -Wno-missing-field-initializers
DSOFLAGS := $(DSOFLAGS) -Wl,-rpath,'$$ORIGIN/julia/lib/'

cleandso::
	rm $(DSONAME)

install::
	@echo "installing into $(INSTDIR)"