/* Fair crux: LOSER-TREE k-way merge (the good one) vs random scatter, single-thread.
 * A loser tree replays one leaf->root path per element: log2(P) predictable
 * compares, no branchy heap. This is the fastest practical scalar k-way merge. */
#include <vector>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <ctime>
#include <cstdint>
#include <algorithm>
#include <limits>
#include <functional>
static uint64_t RS=7; static uint64_t nr(){RS^=RS<<13;RS^=RS>>7;RS^=RS<<17;return RS;}
static double fr(){return (double)(nr()>>11)/(double)(1ULL<<53);}
static double now(){timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1e3+t.tv_nsec/1e6;}
template<class F> double best(F f,int it){double b=1e300;for(int i=0;i<it;i++){double s=now();f();double x=now()-s;if(x<b)b=x;}return b;}
int main(){
  size_t N=8000000;
  for(int P : {16,32,64}){
    std::vector<double> a(N),out(N); for(auto&v:a)v=fr()*2e9-1e9;
    double mn=-1e9,scale=(double)P/2e9;
    std::vector<size_t> cnt(P+1,0); for(double v:a){int b=(int)((v-mn)*scale);if(b<0)b=0;if(b>=P)b=P-1;cnt[b+1]++;}
    for(int b=0;b<P;b++)cnt[b+1]+=cnt[b];
    auto scatter=[&]{ std::vector<size_t> cur(cnt.begin(),cnt.end());
      for(double v:a){int b=(int)((v-mn)*scale);if(b<0)b=0;if(b>=P)b=P-1;out[cur[b]++]=v;} };
    double t_scatter=best(scatter,7);

    std::vector<double> runs(N); size_t clen=N/P; std::vector<size_t> st(P),en(P);
    for(int r=0;r<P;r++){st[r]=r*clen;en[r]=(r==P-1)?N:(r+1)*clen;for(size_t i=st[r];i<en[r];i++)runs[i]=fr()*2e9-1e9;std::sort(runs.begin()+st[r],runs.begin()+en[r]);}
    /* loser tree */
    int K=1; while(K<P)K<<=1;
    auto merge=[&]{
      std::vector<double> val(K); std::vector<size_t> pos(K); std::vector<int> tree(K);
      const double INF=std::numeric_limits<double>::infinity();
      for(int r=0;r<K;r++){ if(r<P){pos[r]=st[r]; val[r]=(pos[r]<en[r])?runs[pos[r]]:INF;} else val[r]=INF; }
      /* build: winner propagates up, loser stays */
      std::vector<int> win(K);
      for(int i=0;i<K;i++)win[i]=i;
      for(int node=K-1;node>=1;node--){/* not used: build via linear play below */}
      /* simple build: play down */
      std::function<int(int)> build=[&](int node)->int{ if(node>=K) return node-K; int l=build(2*node),r=build(2*node+1);
        if(val[l]<=val[r]){tree[node]=r; return l;} else {tree[node]=l; return r;} };
      int winner=build(1);
      size_t o=0;
      while(val[winner]!=INF){ out[o++]=val[winner];
        pos[winner]++; val[winner]=(pos[winner]<en[winner])?runs[pos[winner]]:INF;
        int cur=winner;
        for(int node=(K+winner)>>1; node>=1; node>>=1){ int opp=tree[node];
          if(val[opp]<val[cur]){ tree[node]=cur; cur=opp; } }
        winner=cur;
      }
    };
    /* verify */
    merge(); bool ok=std::is_sorted(out.begin(),out.end());
    double t_merge=best(merge,5);
    printf("P=%-3d  scatter %.1fms   loser-tree merge %.1fms   merge/scatter %.2fx  %s%s\n",
           P,t_scatter,t_merge,t_merge/t_scatter, t_merge<t_scatter?"MERGE CHEAPER":"merge costlier", ok?"":"  UNSORTED!");
  }
  return 0; }
