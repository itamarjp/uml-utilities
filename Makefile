SUBDIRS = jailtest mconsole moo port-helper redhat tunctl uml_net uml_router 
UMLVER = $(shell date +%Y%m%d)
TARBALL = uml_utilities_$(UMLVER).tar
BIN_DIR = /usr/bin
LIB_DIR = /usr/lib/uml

export BIN_DIR LIB_DIR

all install: 
	set -e ; for dir in $(SUBDIRS); do $(MAKE) -C $$dir $@; done

tarball : clean spec
	cd .. ; tar cf $(TARBALL) tools ; bzip2 -f $(TARBALL)

clean:
	rm -rf *~
	rm -f uml_util.spec
	set -e ; for dir in $(SUBDIRS); do $(MAKE) -C $$dir $@; done

spec:	
	sed -e 's/__UMLVER__/$(UMLVER)/' < uml_util.spec.in > uml_util.spec
