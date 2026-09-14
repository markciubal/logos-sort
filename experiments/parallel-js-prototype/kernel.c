typedef unsigned u32; typedef int i32;
__attribute__((export_name("bias")))
void bias(i32* a, int n, u32* out){ for(int i=0;i<n;i++) out[i]=((u32)a[i])+0x80000000u; }
__attribute__((export_name("unbias")))
void unbias(u32* in, int n, i32* out){ for(int i=0;i<n;i++) out[i]=(i32)(in[i]-0x80000000u); }
// MSD partition by top byte: out[] partitioned, BND[0..256] = bucket boundaries
__attribute__((export_name("partition_top")))
void partition_top(u32* in, int n, u32* out, u32* H, u32* BND){
  for(int b=0;b<257;b++)H[b]=0;
  for(int i=0;i<n;i++)H[((in[i]>>24)&0xFF)+1]++;
  for(int b=1;b<257;b++)H[b]+=H[b-1];
  for(int b=0;b<257;b++)BND[b]=H[b];
  for(int i=0;i<n;i++){u32 v=in[i]; out[H[(v>>24)&0xFF]++]=v;}
}
// byte-skip LSD radix on u32 (ascending). In a bucket the top byte is constant,
// so byte-skip drops it automatically -> sorts only the lower active bytes.
__attribute__((export_name("radix_u32")))
void radix_u32(u32* a, int n, u32* src, u32* dst, u32* H){
  u32 *H0=H,*H1=H+257,*H2=H+514,*H3=H+771;
  for(int b=0;b<4*257;b++)H[b]=0;
  u32 orA=0,andA=0xFFFFFFFFu;
  for(int i=0;i<n;i++){u32 v=a[i];src[i]=v;orA|=v;andA&=v;H0[(v&255)+1]++;H1[((v>>8)&255)+1]++;H2[((v>>16)&255)+1]++;H3[((v>>24)&255)+1]++;}
  u32 diff=orA^andA;int a0=(diff&255)!=0,a1=(diff&0xFF00)!=0,a2=(diff&0xFF0000)!=0,a3=(diff&0xFF000000)!=0;
  int last=a3?3:a2?2:a1?1:a0?0:-1;if(last<0){for(int i=0;i<n;i++)a[i]=src[i];return;}
  u32 *from=src,*to=dst,*t;
  if(a0){for(int b=1;b<257;b++)H0[b]+=H0[b-1];if(last==0){for(int i=0;i<n;i++){u32 v=from[i];a[H0[v&255]++]=v;}return;}for(int i=0;i<n;i++){u32 v=from[i];to[H0[v&255]++]=v;}t=from;from=to;to=t;}
  if(a1){for(int b=1;b<257;b++)H1[b]+=H1[b-1];if(last==1){for(int i=0;i<n;i++){u32 v=from[i];a[H1[(v>>8)&255]++]=v;}return;}for(int i=0;i<n;i++){u32 v=from[i];to[H1[(v>>8)&255]++]=v;}t=from;from=to;to=t;}
  if(a2){for(int b=1;b<257;b++)H2[b]+=H2[b-1];if(last==2){for(int i=0;i<n;i++){u32 v=from[i];a[H2[(v>>16)&255]++]=v;}return;}for(int i=0;i<n;i++){u32 v=from[i];to[H2[(v>>16)&255]++]=v;}t=from;from=to;to=t;}
  for(int b=1;b<257;b++)H3[b]+=H3[b-1];for(int i=0;i<n;i++){u32 v=from[i];a[H3[(v>>24)&255]++]=v;}
}
