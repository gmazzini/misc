#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct cty {
  char prefix[16];
  int dxcc;
  char cont[3];
  int cqzone;
  int ituzone;
} *cty;
long ncty=0;

int cmpcty(const void *p1,const void *p2){
  struct cty *a,*b;
  a=(struct cty *)p1;
  b=(struct cty *)p2;
  return strcmp(a->prefix,b->prefix);
}

struct cty *findcty(char *prefix){
  struct cty key,*res;
  strcpy(key.prefix,prefix);
  res=(struct cty *)bsearch(&key,cty,ncty,sizeof(struct cty),cmpcty);
  return res;
}

struct cty *searchcty(char *incall){
  static struct cty out;
  char *p,call[20];
  struct cty *res;
  int i,n;
  const char *suffixes[]={"P","M","LH","MM","AM","A","B","QRP","0","1","2","3","4","5","6","7","8","9"};

  out.prefix[0]='\0';
  out.dxcc=0;
  out.cont[0]='\0';
  out.cqzone=0;
  out.ituzone=0;

  n=sizeof(suffixes)/sizeof(suffixes[0]);
  strcpy(call,incall);
  p=strrchr(call,'/');
  if(p){
    for(i=0;i<n;i++)if(strcmp(p+1,suffixes[i])==0)break;
    if(i<n)*p='\0';
  }
  p=strrchr(call,'/');
  if(p){
    n=strlen(call);
    if((p-call)<(n-(p-call)-1))*p='\0';
    else strcpy(call,p+1);
  }
  n=strlen(call);
  for(i=n;i>0;i--){
    call[i]='\0';
    res=findcty(call);
    if(res){
      out=*res;
      return &out;
    }
  }
  return NULL;
}

void loadcty(){
  FILE *f;
  char riga[100000],*dd[20],*p,*ff,cont[256],prefix[256],tmp[1024],*a,*b,*c,*base,*name;
  int i,dxcc,cqzone,ituzone,cq0,itu0;
  float latitude,longitude,gmtshift,lat0,lon0,gmt0;
  
  f=fopen("cty.csv","r");
  while(fgets(riga,sizeof(riga),f)){
    for(i=0;i<20;i++)dd[i]=0;
    p=strtok(riga,",");
    i=0;
    while(p&&i<20){
      dd[i]=p;
      i++;
      p=strtok(NULL,",");
    }
    if(!dd[9])continue;
    dd[9][strcspn(dd[9],"\r\n")]=0;
    if(strlen(dd[9])>0)dd[9][strlen(dd[9])-1]=0;
    base=dd[0];
    name=dd[1];
    dxcc=atoi(dd[2]);
    cq0=atoi(dd[4]);
    itu0=atoi(dd[5]);
    lat0=(float)atof(dd[6]);
    lon0=(float)atof(dd[7]);
    gmt0=(float)atof(dd[8]);
    ff=strtok(dd[9]," ");
    while(ff){
      strcpy(tmp,ff);
      a=strchr(tmp,'{');
      if(a){
        b=strchr(a,'}');
        strncpy(cont,a+1,b-a-1);
        cont[b-a-1]=0;
        memmove(a,b+1,strlen(b+1)+1);
      }
      else strcpy(cont,dd[3]);
      a=strchr(tmp,'(');
      if(a){
        b=strchr(a,')');
        *b=0;
        cqzone=atoi(a+1);
        memmove(a,b+1,strlen(b+1)+1);
      }
      else cqzone=cq0;
      a=strchr(tmp,'[');
      if(a){
        b=strchr(a,']');
        *b=0;
        ituzone=atoi(a+1);
        memmove(a,b+1,strlen(b+1)+1);
      }
      else ituzone=itu0;
      a=strchr(tmp,'<');
      if(a){
        b=strchr(a,'>');
        c=strchr(a,'/');
        *c=0;
        *b=0;
        latitude=(float)atof(a+1);
        longitude=(float)atof(c+1);
        memmove(a,b+1,strlen(b+1)+1);
      }
      else{
        latitude=lat0;
        longitude=lon0;
      }
      a=strchr(tmp,'~');
      if(a){
        b=strchr(a+1,'~');
        *b=0;
        gmtshift=(float)atof(a+1);
        memmove(a,b+1,strlen(b+1)+1);
      }
      else gmtshift=gmt0;
      if(tmp[0]=='=')strcpy(prefix,tmp+1);
      else strcpy(prefix,tmp);
      strcpy(cty[ncty].prefix,prefix);
      cty[ncty].dxcc=dxcc;
      strcpy(cty[ncty].cont,cont);
      cty[ncty].cqzone=cqzone;
      cty[ncty].ituzone=ituzone;
      ncty++;
      ff=strtok(NULL," ");
    }
  }
  fclose(f);
  qsort(cty,ncty,sizeof(struct cty),cmpcty);
}

int main(){
    cty=(struct cty *)malloc(50000*sizeof(struct cty));
    loadcty();

  struct cty *p;
p=searchcty("IZ1ABC");
if(p)printf("%s %d %s %d %d\n",p->prefix,p->dxcc,p->cont,p->cqzone,p->ituzone);
else printf("non trovato\n");

}
