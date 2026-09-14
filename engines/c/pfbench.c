/* pfbench.c — prefetch-distance A/B. Five copies of the engine (prefetch off,
 * PF_DIST 16/32/64/128) live in this ONE binary and run interleaved on identical
 * data, so background load affects all variants equally and cannot bias the
 * result. Separate parbench runs cannot answer this question reliably.
 * Usage: ./pfbench            (all CPUs)      LOGOS_THREADS=N ./pfbench */
#define _GNU_SOURCE
#include "logos_psort.h"
#include "bench_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#define DECL(P) void P##_sort_f64(double*,size_t,int); void P##_sort_i32(int32_t*,size_t,int); int P##_num_cpus(void);
DECL(pf0) DECL(pf16) DECL(pf32) DECL(pf64) DECL(pf128)
typedef void (*sf)(double*,size_t,int); typedef void (*si)(int32_t*,size_t,int);
static sf F[5]={pf0_sort_f64,pf16_sort_f64,pf32_sort_f64,pf64_sort_f64,pf128_sort_f64};
static si I[5]={pf0_sort_i32,pf16_sort_i32,pf32_sort_i32,pf64_sort_i32,pf128_sort_i32};
static const char*VN[5]={"off","16","32","64","128"};
static uint64_t RS=7; static uint64_t nr(void){RS^=RS<<13;RS^=RS>>7;RS^=RS<<17;return RS;}
static double fr(void){return (double)(nr()>>11)/(double)(1ULL<<53);}
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1e3+t.tv_nsec/1e6;}
static void gen(double*a,size_t n,int d){for(size_t i=0;i<n;i++){ if(d==0)a[i]=fr()*2e9-1e9; else if(d==1)a[i]=1000.0*pow(1.0-fr()*0.999999,-1.0/1.2); else if(d==2)a[i]=((nr()&1)?1e9:-1e9)+fr()*10.0; else a[i]=(double)(int32_t)nr(); }}
static const char*NM[]={"uniform","heavy_tail","bimodal","int_wide"};
int main(void){
  int P=bench_threads(pf32_num_cpus());
  size_t N=8000000; const int R=3; double*base=malloc(N*8),*a=malloc(N*8);
  printf("prefetch-distance A/B — interleaved in one process, n=8M, best of %d, %d threads\n",R,P);
  printf("(load affects all variants equally here; lowest time per row wins)\n\n");
  printf("  %-12s %3s  %8s %8s %8s %8s %8s   best\n","shape","thr","off","16","32","64","128");
  for(int d=0;d<4;d++){ RS=7; gen(base,N,d);
    for(int pass=0;pass<2;pass++){ int t=pass?P:1; double b[5]={1e300,1e300,1e300,1e300,1e300};
      for(int r=0;r<R;r++) for(int v=0;v<5;v++){ memcpy(a,base,N*8); double s=now(); F[v](a,N,t); double x=now()-s; if(x<b[v])b[v]=x;
        for(size_t i=1;i<N;i++) if(a[i-1]>a[i]){printf("UNSORTED (%s, PF %s)\n",NM[d],VN[v]);return 1;} }
      int best=0; for(int v=1;v<5;v++) if(b[v]<b[best])best=v;
      printf("  %-12s %3d  %7.1fms %7.1fms %7.1fms %7.1fms %7.1fms   %s\n",NM[d],t,b[0],b[1],b[2],b[3],b[4],VN[best]); } }
  { int32_t*bi=malloc(N*4),*ai=malloc(N*4); RS=9; for(size_t i=0;i<N;i++)bi[i]=(int32_t)nr();
    for(int pass=0;pass<2;pass++){ int t=pass?P:1; double b[5]={1e300,1e300,1e300,1e300,1e300};
      for(int r=0;r<R;r++) for(int v=0;v<5;v++){ memcpy(ai,bi,N*4); double s=now(); I[v](ai,N,t); double x=now()-s; if(x<b[v])b[v]=x; }
      int best=0; for(int v=1;v<5;v++) if(b[v]<b[best])best=v;
      printf("  %-12s %3d  %7.1fms %7.1fms %7.1fms %7.1fms %7.1fms   %s\n","int32",t,b[0],b[1],b[2],b[3],b[4],VN[best]); } }
  printf("\n_read the parallel rows (thr=%d). Pick the distance that wins most rows; set it with -DPF_DIST=N.\n",P);
  return 0; }
