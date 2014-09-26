args <- commandArgs(trailingOnly = TRUE)
print(args[1])

file=args[1]

png(filename=paste(file, ".total.png", sep=""))
tab = read.table(paste(file, ".total", sep=""), sep=";")
tab[,3] = c(1,2,3,4,rgb(1,1,1))
pie(tab[,1], labels=tab[,2], col=tab[,3])
usedcpu=sum(tab[,1])-tab[5,1]
dev.off()

png(filename=paste(file, ".proc.png", sep=""))
tab = read.table(paste(file, ".proc", sep=""), sep="|")
tab[tab[,3]==0,3]=99
sel=order(tab[,3])
tab=tab[sel,]

levels(tab[,2]) <- c(levels(tab[,2]), "Misc", "Idle")

small=tab[,1]<1
smallcpu=sum(tab[small,1])
tab=tab[!small,]
tab[length(tab[,1])+1,1] = smallcpu
tab[length(tab[,1]),2] = "Misc"
tab[length(tab[,1]),3] = 10

used=sum(tab[,1])
# Guess number of cores
ncores=c(1,2,4,8)
ncore=ncores[order(abs(usedcpu*ncores-used))[1]]
idle = ncore*100-used

tab[length(tab[,1])+1,1] = idle
tab[length(tab[,1]),2] = "Idle"
tab[length(tab[,1]),3] = rgb(1,1,1)

pie(tab[,1], labels=tab[,2], col=tab[,3])
dev.off()