#include "tempo.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MAXC 8
#define WIN 512
static tempo_clock CL[3][MAXC]; static tempo_aligner AL[3];
static int64_t TB[3][MAXC][WIN]; static uint32_t KB[3][MAXC][WIN];
int main(int argc,char**argv){
  FILE*f=fopen(argv[1],"r"); double fps=argc>2?atof(argv[2]):100; char line[512];
  int map[64],nc=0,i; for(i=0;i<64;i++)map[i]=-1;
  int use[64]; for(i=0;i<64;i++)use[i]=(argc<=3);
  if(argc>3){char*s=strdup(argv[3]),*t;for(t=strtok(s,",");t;t=strtok(NULL,","))use[atoi(t)]=1;}
  if(!fgets(line,sizeof line,f))return 1; long pos=ftell(f);
  while(fgets(line,sizeof line,f)){int c;double t;if(sscanf(line,"%d,%lf",&c,&t)==2&&use[c]&&map[c]<0)map[c]=nc++;}
  fseek(f,pos,SEEK_SET);
  int64_t nom=(int64_t)(1e9/fps);
  for(int p=0;p<3;p++){tempo_init(&AL[p],CL[p],(uint8_t)nc,nom);for(int c=0;c<nc;c++)tempo_clock_attach(&AL[p],(uint8_t)c,TB[p][c],KB[p][c],WIN);
    tempo_set_policy(&AL[p],p==0?TEMPO_POLICY_INDEX:p==1?TEMPO_POLICY_NEAREST:TEMPO_POLICY_PREDICTED);}
  long nb[3]={0,0,0}; tempo_batch b;
  printf("#map");for(i=0;i<64;i++)if(map[i]>=0)printf(" %d->%d",i,map[i]);printf("\n");
  while(fgets(line,sizeof line,f)){int c;double t;if(sscanf(line,"%d,%lf",&c,&t)!=2||!use[c])continue;
    int64_t ns=(int64_t)(t*1e9+0.5);
    for(int p=0;p<3;p++){tempo_push(&AL[p],(uint8_t)map[c],ns);
      while(tempo_next(&AL[p],&b)==TEMPO_OK){printf("%d,%ld,%lld,%lld",p,nb[p]++,(long long)b.uncertainty_ns,(long long)b.spread_ns);
        for(int k=0;k<nc;k++)printf(",%lld",(long long)b.arrival_ns[k]);printf("\n");}}}
  return 0;}
