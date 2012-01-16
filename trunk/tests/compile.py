#! /usr/bin/env python

import sys
import os

passPath = "/home/igor/workspace/llvm-3.0/Debug/lib/"

if len(sys.argv) == 2:
    fileName, fileExtension = os.path.splitext(sys.argv[1])
    print("Compiling with Clang")
    print("clang -c -emit-llvm "+fileName+fileExtension+" -o "+fileName+".bc")
    os.system("clang -c -emit-llvm "+fileName+fileExtension+" -o "+fileName+".bc")
    print("Generating eSSA")
    print("opt -instnamer -mem2reg "+fileName+".bc -o "+fileName+".bc")
    os.system("opt -instnamer -mem2reg "+fileName+".bc -o "+fileName+".bc")
    print("opt -load "+passPath+"vSSA.so -vssa "+fileName+".bc -o "+fileName+".essa.bc")
    os.system("opt -load "+passPath+"vSSA.so -vssa "+fileName+".bc -o "+fileName+".essa.bc")
    print("Running the Range Analysis Pass")
    print("opt -load "+passPath+"RangeAnalysis.so -range-analysis "+fileName+".essa.bc")
    os.system("opt -load "+passPath+"RangeAnalysis.so -range-analysis "+fileName+".essa.bc")
else:
    print len(sys.argv)
