#include "common.hlsli"
cbuffer Shape : register(b0) {
 float2 center; float2 halfSize;
 float radius;float count;float gap;float corner;
 float outer;float inner;float hole;float appearScale;
 float dpiScale;float maskIndex;float refract;float reserved;
 float4 scales[4];
};
VSOut VSMain(uint id:SV_VertexID) {return FullscreenVS(id);}
float Sector(int k,float2 p,float pitch,float halfW) {
 if(scales[k/4][k%4]<=0)return 1e6;
 float R=halfSize.x;float angle=k*pitch;float rm=(outer+inner)*.5*R;
 float2 c=float2(sin(angle),-cos(angle))*rm;
 float sc=max(scales[k/4][k%4],.01);p=c+(p-c)/sc;
 float r=length(p);float rc=corner*R;
 float edge=-1e9;
 if(count>1.5) {
  float phi=atan2(p.x,-p.y)-angle;phi-=2*PI*floor((phi+PI)/(2*PI));
  edge=abs(r*sin(phi))*cos(halfW)-r*cos(phi)*sin(halfW);
 }
 float3 q=float3(r-outer*R,inner*R-r,edge)+rc;
 return (length(max(q,0))+min(max(q.x,max(q.y,q.z)),0)-rc)*sc;
}
float Distance(float2 at) {
 float2 p=at-center;
 if(count<.5) {
  float2 q=abs(p)-halfSize+radius;
  return length(max(q,0))+min(max(q.x,q.y),0)-radius;
 }
 p/=max(appearScale,.01);
 float pitch=2*PI/max(count,1),halfW=count<1.5?PI:pitch*.5-gap*.5;
 float phi=atan2(p.x,-p.y);if(phi<0)phi+=2*PI;
 int k=((int)floor(phi/pitch+.5))%(int)count;
 float sd=Sector(k,p,pitch,halfW);
 // Hover can extend into an adjacent slice, exactly as the visible shader does.
 [loop]for(int j=0;j<(int)count&&j<16;++j) if(j!=k&&scales[j/4][j%4]>1.001)sd=min(sd,Sector(j,p,pitch,halfW));
 float r=length(p),R=halfSize.x;
 float rim=max(r-R,R-7*dpiScale-r);
 float centre=r-hole*R;
 return min(sd,min(rim,centre))*appearScale;
}
float4 PSMain(VSOut i):SV_Target {
 float d=Distance(i.pos.xy);float coverage=saturate(.5-d);
 float2 gradient=float2(ddx(d),ddy(d));float len=length(gradient);
 float2 normal=len>.0001?gradient/len:float2(0,-1);
 float bend=pow(saturate(1-max(-d,0)/(7*dpiScale)),2)*saturate(refract);
 float2 delta=-normal*bend/max(1,abs(normal.x)+abs(normal.y));
 float w[5]={1-abs(delta.x)-abs(delta.y),max(-delta.x,0),max(delta.x,0),max(-delta.y,0),max(delta.y,0)};
 // Conditional source-over alpha preserves the weighted sum and its coverage.
 float prefix=0;[unroll]for(int k=0;k<5;++k)if(k<=(int)maskIndex)prefix+=w[k];
 float a=max(w[(int)maskIndex],0)*coverage/max(1-coverage+prefix*coverage,.00001);
 return a.xxxx;
}
