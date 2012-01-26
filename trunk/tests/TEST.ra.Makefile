##===- TEST.ra.Makefile ------------------------------*- Makefile -*-===##
#
# Usage: 
#     make TEST=ra (detailed list with time passes, etc.)
#     make TEST=ra report
#     make TEST=ra report.html
#
##===----------------------------------------------------------------------===##

CURDIR  := $(shell cd .; pwd)
PROGDIR := $(PROJ_SRC_ROOT)
RELDIR  := $(subst $(PROGDIR),,$(CURDIR))

$(PROGRAMS_TO_TEST:%=test.$(TEST).%): \
test.$(TEST).%: Output/%.$(TEST).report.txt
	@cat $<

$(PROGRAMS_TO_TEST:%=Output/%.$(TEST).report.txt):  \
Output/%.$(TEST).report.txt: Output/%.linked.rbc $(LOPT) \
	$(PROJ_SRC_ROOT)/TEST.ra.Makefile 
	$(VERB) $(RM) -f $@
	@echo "---------------------------------------------------------------" >> $@
	@echo ">>> ========= '$(RELDIR)/$*' Program" >> $@
	@echo "---------------------------------------------------------------" >> $@
	@-$(LOPT) -mem2reg -instnamer -inline -internalize -load \
	/home/vhscampos/IC/llvm-3.0/Debug/lib/vSSA.so -vssa \
	-load /home/vhscampos/IC/llvm-3.0/Debug/lib/RangeAnalysis.so -load \
	/home/vhscampos/IC/llvm-3.0/Debug/lib/Matching.so -matching -stats \
	-time-passes -disable-output $< 2>>$@ 

REPORT_DEPENDENCIES := $(LOPT)
