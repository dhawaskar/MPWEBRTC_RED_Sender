import sys
import numpy as np

if(len(sys.argv)<2):
    print ("Usage error")
    exit(0)
filename=sys.argv[1]
fd=open(filename,"r")

Lines = fd.readlines()
de=[]
bits=[]
# Strips the newline character
for line in Lines:
	line=line.split("\n")[0]
	id1=line.split(" ")
	de.append(int(id1[-1]))
	bits.append(int(id1[4]))

print ("RTT ms= ",sum(de)/len(de)," max= ",max(de)," sending rate= ",sum(bits)/len(bits)," max= ",max(bits))
