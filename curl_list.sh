#!/bin/bash
# 为了快速下载代码目录
list=(
	counterprob.c
	fork.c
	forkprob0.c
	forkprob1.c
	forkprob2.c
	forkprob3.c
	forkprob4.c
	forkprob5.c
	forkprob6.c
	forkprob7.c
	forkprob8.c
	hello-asm64.sa
	kill.c
	procmask1.c
	procmask2.c
	restart.c
	setjmp.c
	shellex.c
	sigint.c
	sigintsafe.c
	signal1.c
	signal2.c
	signalprob0.c
	sigsuspend.c
	waitforsignal.c
	waitpid1.c
	waitpid2.c
	waitprob0.c
	waitprob1.c
	waitprob3.c
)
for((i=0; i<${#list[*]}; i++));
do
	echo ${list[$i]}
	`curl -s -o ${list[$i]} http://csapp.cs.cmu.edu/3e/ics3/code/conc/${list[$i]}`
done


