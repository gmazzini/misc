#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <ctype.h>
#include <time.h>
#include <stdlib.h>
#include <stdint.h>
#define MEMBER 9
#define OPER 17
#define BAND 6
#define NLOG 30000
#define FILEOUT "/run/tabella"
#define FILELOG "/home/www/data/wpx2026"

const char *station[MEMBER] = {"LZH", "VUS", "AOT", "ORO", "COW", "UFH", "AUY", "JBB", "TRE"};
const char *operator[OPER] = {"IU4ICT", "IU5LVM", "IU5JZQ", "I4KMW", "I4UFH", "I4YMN", "IK2WAD", "IK4AUY", "IK4LZH", "IU4JFJ", "IW2JBB", "IW4AOT", "IZ2JQP", "IZ4COW", "IZ4ORO", "IZ4VUS", "MISS"};
const char *band[BAND] = {"160", "80", "40", "20", "15", "10"};

struct header {
  char station[16];
  char destination[16];
  char type[4];
} hh;

struct status {
  char id1[4];
  char id2[16];
  char band[10];
  char mode[10];
  char func[5];
  char ot[20];
  char freq1[20];
  char freq2[20];
  char operator[16];
} ss;

struct hint {
  char id1[4];
  char id2[16];
  char fake1[12];
  char call1[16];
  char call2[16];
} tt;

struct qso {
  char id[4];
  char qso[6];
  char band[10];
  char fake1[3];
  char freq[15];
  char mode[10];
  char time[15];
  char call[16];
  char stx[5];
  char ntx[6];
  char srx[4];
  char nrx[6];
  char fake2[85];
  char station[15];
  char fake3[37];
  char operator[16];
} qq;

struct out {
  char band[10];
  char mode[10];
  char freq1[20];
  char freq2[20];
  char operator[16];
  char call1[16];
  char call2[16];
  time_t last_hint;
} oo[MEMBER];

struct out2 {
  time_t last_qso;
  int qso[BAND];
} ww[OPER];

struct dxlog {
  char band[10];
  char freq[15];
  char mode[10];
  char time[15];
  char call[16];
  char stx[5];
  char ntx[6];
  char srx[4];
  char nrx[6];
  char operator[16];
  char station[15];
  uint32_t hash;
} *dxlog;

char * wpx(char *s){
  int i;
  static char out[20];
  strcpy(out,s);
  for(i=strlen(out)-1;i>=0;i--)if(out[i]>'0' && out[i]<='9')break;
  out[i+1]='\0';
  return out;
}

static long long now_us(void){
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000000LL + (long long)(ts.tv_nsec / 1000);
}

uint32_t hash24(const char *s) {
  uint32_t h=2166136261u;
  while(*s){
    h^=(unsigned char)*s++;
    h*=16777619u;
  }
  h^=h>>16;
  h*=0x7feb352d;
  h^=h>>15;
  h*=0x846ca68b;
  h^=h>>16;
  return h&0xFFFFFFu;
}

int main(void) {
  int s,n,i,j,b,m[BAND+1],qm[BAND],nlog,l,yes=1,qso[BAND],qsoS[MEMBER];
  struct sockaddr_in addr, client;
  socklen_t client_len = sizeof(client);
  char buf[1024],buf2[1024],rx[512],*rr,*aux,*end,*p,*q;
  time_t gg,nn;
  FILE *fp,*fp2;
  long long t1,t2;
  uint32_t l1,l2,l3;
  char *ha[BAND+1],*qa[BAND];

  for(i=0;i<(BAND+1);i++)ha[i]=malloc(0xFFFFFF*sizeof(char));
  for(i=0;i<BAND;i++)qa[i]=malloc(0xFFFFFF*sizeof(char));

  dxlog=malloc(NLOG*sizeof(struct dxlog));
  fp=fopen(FILELOG,"rt");
  nlog=0;
  for(;;){
    if(fgets(buf,1024,fp)==0)break;
    q=strstr(buf,",,");
    if(q)memmove(q+6,q+2,strlen(q+2)+1),memcpy(q,",MISS,",6);
    p=strtok(buf,","); memcpy(dxlog[nlog].band,p,10);
    p=strtok(0,","); memcpy(dxlog[nlog].freq,p,15);
    p=strtok(0,","); memcpy(dxlog[nlog].mode,p,10);
    p=strtok(0,","); memcpy(dxlog[nlog].time,p,15);
    p=strtok(0,","); memcpy(dxlog[nlog].call,p,16);
    p=strtok(0,","); memcpy(dxlog[nlog].stx,p,5);
    p=strtok(0,","); memcpy(dxlog[nlog].ntx,p,6);
    p=strtok(0,","); memcpy(dxlog[nlog].srx,p,4);
    p=strtok(0,","); memcpy(dxlog[nlog].nrx,p,6);
    p=strtok(0,","); memcpy(dxlog[nlog].operator,p,16);
    p=strtok(0,",\n"); memcpy(dxlog[nlog].station,p,15);
    sprintf(buf2,"%.10s,%.15s,%.10s,%.15s,%.16s,%.5s,%.6s,%.4s,%.6s,%.16s",
      dxlog[nlog].band,dxlog[nlog].freq,dxlog[nlog].mode,dxlog[nlog].time,dxlog[nlog].call,
      dxlog[nlog].stx,dxlog[nlog].ntx,dxlog[nlog].srx,dxlog[nlog].nrx,dxlog[nlog].operator);
    l1=hash24(buf2);
    dxlog[nlog].hash=l1;

/*
    for(i=0;i<nlog;i++)if(dxlog[i].hash==l1)break;
    if(i==nlog){
      fp2=fopen("/root/prova","at");
      fprintf(fp2,"%s,%s\n",buf2,dxlog[nlog].station);
      fclose(fp2);
    }
*/

    nlog++;
  }
  fclose(fp);
  printf("log=%d\n",nlog);
 s=socket(AF_INET,SOCK_DGRAM,0);
  setsockopt(s,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));
  setsockopt(s,SOL_SOCKET,SO_REUSEPORT,&yes,sizeof(yes));
  memset(&addr,0,sizeof(addr));
  addr.sin_family=AF_INET;
  addr.sin_port=htons(9888);
  addr.sin_addr.s_addr=INADDR_ANY;
  bind(s,(struct sockaddr *)&addr,sizeof(addr));

  t1=now_us();
  for(;;){
    n=recvfrom(s,buf,1024,0,(struct sockaddr *)&client,&client_len);
    n/=2;
    for(i=j=0;i<n;i++,j+=2)rx[i]=buf[j];
    memcpy(&hh,rx,sizeof(struct header));
    rr=rx+sizeof(struct header);
    for(i=0;i<MEMBER;i++)if(strcmp(station[i],hh.station)==0)break;
    if(i==MEMBER)continue;

    if(strcmp(hh.type,"STI")==0){
      memcpy(&ss,rr,sizeof(struct status));
      memcpy(oo[i].band,ss.band,10);
      memcpy(oo[i].mode,ss.mode,10);
      memcpy(oo[i].freq1,ss.freq1,20);
      memcpy(oo[i].freq2,ss.freq2,20);
      memcpy(oo[i].operator,ss.operator,16);
    }
if(strcmp(hh.type,"TXS")==0){
     memcpy(&tt,rr,64);
     memcpy(oo[i].call1,tt.call1,16);
     memcpy(oo[i].call2,tt.call2,16);
     oo[i].last_hint=time(NULL);
    }

    if(strcmp(hh.type,"QSO")==0){
      memcpy(qq.id,rr,4);
      memcpy(qq.qso,buf+80,96+137+16);
      memcpy(dxlog[nlog].band,qq.band,10);
      memcpy(dxlog[nlog].freq,qq.freq,15);
      memcpy(dxlog[nlog].mode,qq.mode,10);
      memcpy(dxlog[nlog].time,qq.time,15);
      memcpy(dxlog[nlog].call,qq.call,16);
      memcpy(dxlog[nlog].stx,qq.stx,5);
      memcpy(dxlog[nlog].ntx,qq.ntx,6);
      memcpy(dxlog[nlog].srx,qq.srx,4);
      memcpy(dxlog[nlog].nrx,qq.nrx,6);
      memcpy(dxlog[nlog].operator,qq.operator,16);
      memcpy(dxlog[nlog].station,qq.station,15);
      sprintf(buf2,"%.10s,%.15s,%.10s,%.15s,%.16s,%.5s,%.6s,%.4s,%.6s,%.16s",
        dxlog[nlog].band,dxlog[nlog].freq,dxlog[nlog].mode,dxlog[nlog].time,dxlog[nlog].call,
        dxlog[nlog].stx,dxlog[nlog].ntx,dxlog[nlog].srx,dxlog[nlog].nrx,dxlog[nlog].operator);
      l1=hash24(buf2);
      dxlog[nlog].hash=l1;
      for(i=0;i<nlog;i++)if(dxlog[i].hash==l1)break;
      if(i==nlog){
        fp2=fopen(FILELOG,"at");
        fprintf(fp2,"%s,%.15s\n",buf2,dxlog[nlog].station);
        fclose(fp2);
        nlog++;
      }
    }
 t2=now_us();
    if(t2-t1>1000000){
      t1=t2;
      gg=time(NULL);
      nn=0;
      for(i=0;i<MEMBER;i++)qsoS[i]=0;
      for(i=0;i<OPER;i++)for(b=0;b<BAND;b++){ww[i].qso[b]=0; qso[b]=0;}
      for(i=0;i<BAND;i++){
        m[i]=0; memset(ha[i],0,0xFFFFFF);
        qm[i]=0; memset(qa[i],0,0xFFFFFF);
      }
      m[BAND]=0; memset(ha[BAND],0,0xFFFFFF);
      for(l=0;l<nlog;l++){
        for(i=0;i<MEMBER;i++)if(strcmp(station[i],dxlog[l].station)==0)break;
        if(i==MEMBER)continue;
        for(j=0;j<OPER;j++)if(strcmp(operator[j],dxlog[l].operator)==0)break;
        if(j==OPER)continue;
        for(b=0;b<BAND;b++)if(strcmp(band[b],dxlog[l].band)==0)break;
        if(b==BAND)continue;
        qsoS[i]++;
        ww[j].qso[b]++;
        qso[b]++;
        ww[j].last_qso=atol(dxlog[l].time);
        if(nn<ww[j].last_qso)nn=ww[j].last_qso;
        l2=hash24(wpx(dxlog[l].call));
        if(ha[BAND][l2]==0){
          ha[BAND][l2]=1;
          m[BAND]++;
        }
        if(ha[b][l2]==0){
          ha[b][l2]=1;
          m[b]++;
        }
        l2=hash24(dxlog[l].call);
        if(qa[b][l2]==0){
          qa[b][l2]=1;
          qm[b]++;
        }
      }

      fp=fopen(FILEOUT,"wt");
      fprintf(fp,"%s\t%s\t%s\t%s\t%s\t%s\t%s\t%16s\t%16s\t%10s\n","STAZ","QSO","BAND","MODE","FREQ1","FREQ2","OPER","CALL1","CALL2","LastHint");
      for(i=0;i<MEMBER;i++)fprintf(fp,"%s\t%d\t%s\t%s\t%s\t%s\t%s\t%16s\t%16s\t%10ld\n",station[i],qsoS[i],oo[i].band,oo[i].mode,oo[i].freq1,oo[i].freq2,oo[i].operator,oo[i].call1,oo[i].call2,gg-oo[i].last_hint);
      fprintf(fp,"\n");

      fprintf(fp,"%s\t%10s","OPER","LastQSO");
      for(b=0;b<BAND;b++)fprintf(fp,"\t%s",band[b]);

      fprintf(fp,"\t%s\n","TOT");
      for(j=0;j<OPER;j++){
        fprintf(fp,"%s\t%10ld",operator[j],gg-ww[j].last_qso);
        for(n=b=0;b<BAND;b++){
          n+=ww[j].qso[b];
          fprintf(fp,"\t%d",ww[j].qso[b]);
         }
         fprintf(fp,"\t%d\n",n);
      }
      fprintf(fp,"\n");

      fprintf(fp,"%s\t%10ld","TOT",gg-nn);
      for(b=0;b<BAND;b++)fprintf(fp,"\t%d",qso[b]);
      fprintf(fp,"\t%d\n",nlog);
      fprintf(fp,"%s\t%10ld","QSO",0);
      for(n=b=0;b<BAND;b++){fprintf(fp,"\t%d",qm[b]); n+=qm[b]; }
      fprintf(fp,"\t%d\n",n);
      fprintf(fp,"%s\t%10ld","MUL",0);
      for(b=0;b<BAND;b++)fprintf(fp,"\t%d",m[b]);
      fprintf(fp,"\t%d\n",m[BAND]);

      fclose(fp);
    }
  }
}

